#include <array>
#include <string>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/buffer_ref.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/gpu_frame_utils.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"

namespace {

std::string AvErrorString(int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error " + std::to_string(error);
  }
  return buffer.data();
}

TEST(FfmpegCudaSmokeTest, FindsRequiredNvidiaCodecs) {
  for (const char* name : {"h264_cuvid", "hevc_cuvid"}) {
    const AVCodec* codec = avcodec_find_decoder_by_name(name);
    ASSERT_NE(codec, nullptr) << "missing NVIDIA decoder: " << name;
    EXPECT_NE(av_codec_is_decoder(codec), 0) << name;
  }

  for (const char* name : {"h264_nvenc", "hevc_nvenc"}) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    ASSERT_NE(codec, nullptr) << "missing NVIDIA encoder: " << name;
    EXPECT_NE(av_codec_is_encoder(codec), 0) << name;
  }
}

TEST(FfmpegCudaSmokeTest, AllocatesAndTransfersNv12HardwareFrame) {
  ASSERT_EQ(av_hwdevice_find_type_by_name("cuda"), AV_HWDEVICE_TYPE_CUDA)
      << "the supplied FFmpeg was built without CUDA hwcontext support";

  mw::streamer::internal::CudaDeviceContext device;
  try {
    device.Create(0);
  } catch (const mw::streamer::internal::Error& error) {
    if (error.code() == mw::streamer::StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  auto device_reference = device.buffer().Ref();
  auto frames = mw::streamer::ffmpeg::BufferRef::Adopt(
      av_hwframe_ctx_alloc(device_reference.get()));

  constexpr int kWidth = 128;
  constexpr int kHeight = 72;
  auto* frames_context =
      reinterpret_cast<AVHWFramesContext*>(frames.get()->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = kWidth;
  frames_context->height = kHeight;

  const int frames_result = av_hwframe_ctx_init(frames.get());
  ASSERT_GE(frames_result, 0) << AvErrorString(frames_result);

  mw::streamer::ffmpeg::Frame gpu_frame;
  gpu_frame.get()->format = AV_PIX_FMT_CUDA;
  gpu_frame.get()->width = kWidth;
  gpu_frame.get()->height = kHeight;

  const int allocation_result =
      av_hwframe_get_buffer(frames.get(), gpu_frame.get(), 0);
  ASSERT_GE(allocation_result, 0) << AvErrorString(allocation_result);
  EXPECT_EQ(gpu_frame.get()->format, AV_PIX_FMT_CUDA);
  EXPECT_EQ(gpu_frame.get()->width, kWidth);
  EXPECT_EQ(gpu_frame.get()->height, kHeight);
  EXPECT_NE(gpu_frame.get()->data[0], nullptr);
  EXPECT_NE(gpu_frame.get()->data[1], nullptr);
  EXPECT_GE(gpu_frame.get()->linesize[0], kWidth);
  EXPECT_GE(gpu_frame.get()->linesize[1], kWidth);
  EXPECT_NE(gpu_frame.get()->hw_frames_ctx, nullptr);

  mw::streamer::ffmpeg::Frame software_frame;
  software_frame.get()->format = AV_PIX_FMT_NV12;
  software_frame.get()->width = kWidth;
  software_frame.get()->height = kHeight;

  const int software_allocation_result =
      av_frame_get_buffer(software_frame.get(), 32);
  ASSERT_GE(software_allocation_result, 0)
      << AvErrorString(software_allocation_result);

  const int transfer_result =
      av_hwframe_transfer_data(software_frame.get(), gpu_frame.get(), 0);
  ASSERT_GE(transfer_result, 0) << AvErrorString(transfer_result);
}

TEST(FfmpegCudaSmokeTest, AcquiresFramesFromGpuNv12FramePool) {
  mw::streamer::internal::CudaDeviceContext device;
  try {
    device.Create(0);
  } catch (const mw::streamer::internal::Error& error) {
    if (error.code() == mw::streamer::StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  constexpr int kWidth = 160;
  constexpr int kHeight = 144;
  mw::streamer::internal::GpuNv12FramePool pool;
  ASSERT_NO_THROW(pool.Create(device, kWidth, kHeight, 2));
  EXPECT_TRUE(pool.is_valid());
  EXPECT_EQ(pool.device_id(), 0);
  EXPECT_EQ(pool.width(), kWidth);
  EXPECT_EQ(pool.height(), kHeight);

  mw::streamer::ffmpeg::Frame first = pool.Acquire();
  mw::streamer::ffmpeg::Frame second = pool.Acquire();
  for (const auto* frame : {first.get(), second.get()}) {
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->format, AV_PIX_FMT_CUDA);
    EXPECT_EQ(frame->width, kWidth);
    EXPECT_EQ(frame->height, kHeight);
    EXPECT_NE(frame->data[0], nullptr);
    EXPECT_NE(frame->data[1], nullptr);
    EXPECT_NE(frame->hw_frames_ctx, nullptr);

    const auto* frames_context =
        reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    ASSERT_NE(frames_context, nullptr);
    EXPECT_EQ(frames_context->format, AV_PIX_FMT_CUDA);
    EXPECT_EQ(frames_context->sw_format, AV_PIX_FMT_NV12);
  }
}

TEST(FfmpegCudaSmokeTest, CreatesReusableLimitedRangeBlackFrameOnGpu) {
  mw::streamer::internal::CudaDeviceContext device;
  try {
    device.Create(0);
  } catch (const mw::streamer::internal::Error& error) {
    if (error.code() == mw::streamer::StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  constexpr int kWidth = 160;
  constexpr int kHeight = 144;
  mw::streamer::internal::GpuNv12FramePool pool;
  pool.Create(device, kWidth, kHeight, 1);
  mw::streamer::ffmpeg::Frame gpu_frame =
      mw::streamer::internal::CreateBlackGpuNv12Frame(&pool, AVCOL_RANGE_MPEG,
                                                      AVCOL_SPC_BT709);

  mw::streamer::ffmpeg::Frame downloaded;
  downloaded.get()->format = AV_PIX_FMT_NV12;
  downloaded.get()->width = kWidth;
  downloaded.get()->height = kHeight;
  ASSERT_EQ(av_frame_get_buffer(downloaded.get(), 32), 0);
  ASSERT_EQ(av_hwframe_transfer_data(downloaded.get(), gpu_frame.get(), 0), 0);

  for (int row = 0; row < kHeight; ++row) {
    for (int column = 0; column < kWidth; ++column) {
      EXPECT_EQ(downloaded.get()
                    ->data[0][row * downloaded.get()->linesize[0] + column],
                16);
    }
  }
  for (int row = 0; row < kHeight / 2; ++row) {
    for (int column = 0; column < kWidth; ++column) {
      EXPECT_EQ(downloaded.get()
                    ->data[1][row * downloaded.get()->linesize[1] + column],
                128);
    }
  }
  EXPECT_EQ(gpu_frame.get()->color_range, AVCOL_RANGE_MPEG);
  EXPECT_EQ(gpu_frame.get()->colorspace, AVCOL_SPC_BT709);
}

}  // namespace
