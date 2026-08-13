#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/processor/internal/frame_adapter.h"

namespace {

using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::HardwareContext;
using mw::streamer::processor::internal::AudioBufferAdapter;
using mw::streamer::processor::internal::AudioFrameAdapter;
using mw::streamer::processor::internal::VideoBufferAdapter;
using mw::streamer::processor::internal::VideoFrameAdapter;

struct PixelFormatCase {
  AVPixelFormat ffmpeg_format;
  MwStreamerVideoPixelFormat processor_format;
  std::vector<std::uint32_t> row_bytes;
  std::vector<std::uint32_t> row_counts;
};

Frame CreateSoftwareVideoFrame(AVPixelFormat format) {
  Frame frame;
  frame->format = format;
  frame->width = 64;
  frame->height = 32;
  frame->pts = 90;
  frame->duration = 3;
  frame->time_base = {1, 30};
  REQUIRE(av_frame_get_buffer(frame.get(), 32) >= 0);
  return frame;
}

Frame CreateAudioFrame() {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 16;
  frame->pts = 32;
  frame->duration = 16;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  return frame;
}

TEST_CASE("VideoFrameAdapter映射常用软件YUV格式") {
  const std::array cases = {
      PixelFormatCase{
          AV_PIX_FMT_NV12, kMwStreamerVideoPixelFormatNv12, {64, 64}, {32, 16}},
      PixelFormatCase{AV_PIX_FMT_P010LE,
                      kMwStreamerVideoPixelFormatP010,
                      {128, 128},
                      {32, 16}},
      PixelFormatCase{AV_PIX_FMT_YUV420P,
                      kMwStreamerVideoPixelFormatYuv420p,
                      {64, 32, 32},
                      {32, 16, 16}},
      PixelFormatCase{AV_PIX_FMT_YUV422P,
                      kMwStreamerVideoPixelFormatYuv422p,
                      {64, 32, 32},
                      {32, 32, 32}},
      PixelFormatCase{AV_PIX_FMT_YUV444P,
                      kMwStreamerVideoPixelFormatYuv444p,
                      {64, 64, 64},
                      {32, 32, 32}},
      PixelFormatCase{AV_PIX_FMT_YUV420P10LE,
                      kMwStreamerVideoPixelFormatYuv420p10le,
                      {128, 64, 64},
                      {32, 16, 16}},
      PixelFormatCase{AV_PIX_FMT_YUV422P10LE,
                      kMwStreamerVideoPixelFormatYuv422p10le,
                      {128, 64, 64},
                      {32, 32, 32}},
      PixelFormatCase{AV_PIX_FMT_YUV444P10LE,
                      kMwStreamerVideoPixelFormatYuv444p10le,
                      {128, 128, 128},
                      {32, 32, 32}},
      PixelFormatCase{AV_PIX_FMT_P016LE,
                      kMwStreamerVideoPixelFormatP016,
                      {128, 128},
                      {32, 16}},
      PixelFormatCase{AV_PIX_FMT_YUV444P16LE,
                      kMwStreamerVideoPixelFormatYuv444p16le,
                      {128, 128, 128},
                      {32, 32, 32}},
  };

  for (const auto& test_case : cases) {
    auto frame = CreateSoftwareVideoFrame(test_case.ffmpeg_format);
    const VideoFrameAdapter adapter(frame);
    const auto& view = adapter.view();

    CHECK(view.buffer.memory_type == kMwStreamerMemoryHost);
    CHECK(view.buffer.storage_type == kMwStreamerVideoStorageLinear);
    CHECK(view.buffer.pixel_format == test_case.processor_format);
    CHECK(view.buffer.width == 64);
    CHECK(view.buffer.height == 32);
    REQUIRE(view.buffer.storage.linear.plane_count ==
            test_case.row_bytes.size());

    for (std::size_t plane = 0; plane < test_case.row_bytes.size(); ++plane) {
      const auto& mapped_plane = view.buffer.storage.linear.planes[plane];
      CHECK(mapped_plane.address ==
            reinterpret_cast<std::uintptr_t>(frame->data[plane]));
      CHECK(mapped_plane.stride_bytes == frame->linesize[plane]);
      CHECK(mapped_plane.row_bytes == test_case.row_bytes[plane]);
      CHECK(mapped_plane.row_count == test_case.row_counts[plane]);
    }
  }
}

TEST_CASE("VideoFrameAdapter映射颜色和时间戳") {
  auto frame = CreateSoftwareVideoFrame(AV_PIX_FMT_YUV420P);
  frame->color_range = AVCOL_RANGE_JPEG;
  frame->colorspace = AVCOL_SPC_BT2020_NCL;
  frame->color_primaries = AVCOL_PRI_BT2020;
  frame->color_trc = AVCOL_TRC_SMPTE2084;
  frame->chroma_location = AVCHROMA_LOC_TOPLEFT;

  const VideoFrameAdapter adapter(frame);
  const auto& view = adapter.view();

  CHECK(view.color.range == kMwStreamerColorRangeFull);
  CHECK(view.color.space == kMwStreamerColorSpaceBt2020Ncl);
  CHECK(view.color.primaries == kMwStreamerColorPrimariesBt2020);
  CHECK(view.color.transfer == kMwStreamerColorTransferSmpte2084);
  CHECK(view.color.chroma_location == kMwStreamerChromaLocationTopLeft);
  CHECK(view.timestamp.pts == 90);
  CHECK(view.timestamp.duration == 3);
  CHECK(view.timestamp.time_base.num == 1);
  CHECK(view.timestamp.time_base.den == 30);
}

TEST_CASE("VideoBufferAdapter暴露同一软件帧的可写存储") {
  auto frame = CreateSoftwareVideoFrame(AV_PIX_FMT_YUV444P);
  const VideoBufferAdapter adapter(frame);
  const auto& view = adapter.view();

  REQUIRE(view.storage.linear.plane_count == 3);
  auto* first_byte =
      reinterpret_cast<std::uint8_t*>(view.storage.linear.planes[0].address);
  REQUIRE(first_byte != nullptr);
  *first_byte = 0x5a;
  CHECK(frame->data[0][0] == 0x5a);
}

TEST_CASE("VideoFrameAdapter拒绝不支持的像素格式和无效时间基") {
  auto rgb_frame = CreateSoftwareVideoFrame(AV_PIX_FMT_RGB24);
  CHECK_THROWS_AS(VideoFrameAdapter(rgb_frame), std::invalid_argument);

  Frame unsupported_hardware_frame;
  unsupported_hardware_frame->format = AV_PIX_FMT_VAAPI;
  unsupported_hardware_frame->width = 64;
  unsupported_hardware_frame->height = 32;
  unsupported_hardware_frame->time_base = {1, 25};
  CHECK_THROWS_AS(VideoFrameAdapter(unsupported_hardware_frame),
                  std::invalid_argument);

  auto missing_time_base = CreateSoftwareVideoFrame(AV_PIX_FMT_YUV420P);
  missing_time_base->time_base = {0, 1};
  CHECK_THROWS_AS(VideoFrameAdapter(missing_time_base), std::invalid_argument);
}

TEST_CASE("VideoFrameAdapter映射CUDA线性硬件帧") {
  const auto hardware_context = HardwareContext::CreateCuda(0);
  const std::array cases = {
      PixelFormatCase{
          AV_PIX_FMT_NV12, kMwStreamerVideoPixelFormatNv12, {64, 64}, {32, 16}},
      PixelFormatCase{AV_PIX_FMT_P016LE,
                      kMwStreamerVideoPixelFormatP016,
                      {128, 128},
                      {32, 16}},
      PixelFormatCase{AV_PIX_FMT_YUV444P16LE,
                      kMwStreamerVideoPixelFormatYuv444p16le,
                      {128, 128, 128},
                      {32, 32, 32}},
  };

  for (const auto& test_case : cases) {
    AVBufferRef* frames_ref =
        av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(hardware_context.get()));
    REQUIRE(frames_ref != nullptr);

    auto* frames_context =
        reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    frames_context->format = AV_PIX_FMT_CUDA;
    frames_context->sw_format = test_case.ffmpeg_format;
    frames_context->width = 64;
    frames_context->height = 32;
    frames_context->initial_pool_size = 1;

    const int init_result = av_hwframe_ctx_init(frames_ref);
    if (init_result < 0) {
      av_buffer_unref(&frames_ref);
    }
    REQUIRE(init_result >= 0);

    Frame frame;
    const int allocation_result =
        av_hwframe_get_buffer(frames_ref, frame.get(), 0);
    av_buffer_unref(&frames_ref);
    REQUIRE(allocation_result >= 0);
    frame->pts = 10;
    frame->duration = 1;
    frame->time_base = {1, 25};

    const VideoFrameAdapter adapter(frame);
    const auto& view = adapter.view();

    CHECK(view.buffer.memory_type == kMwStreamerMemoryCuda);
    CHECK(view.buffer.storage_type == kMwStreamerVideoStorageLinear);
    CHECK(view.buffer.pixel_format == test_case.processor_format);
    REQUIRE(view.buffer.storage.linear.plane_count ==
            test_case.row_bytes.size());
    for (std::size_t plane = 0; plane < test_case.row_bytes.size(); ++plane) {
      CHECK(view.buffer.storage.linear.planes[plane].address ==
            reinterpret_cast<std::uintptr_t>(frame->data[plane]));
      CHECK(view.buffer.storage.linear.planes[plane].stride_bytes ==
            frame->linesize[plane]);
      CHECK(view.buffer.storage.linear.planes[plane].row_bytes ==
            test_case.row_bytes[plane]);
      CHECK(view.buffer.storage.linear.planes[plane].row_count ==
            test_case.row_counts[plane]);
    }
  }
}

TEST_CASE("音频Adapter映射48kHz float32交错帧") {
  auto frame = CreateAudioFrame();
  auto* samples = reinterpret_cast<float*>(frame->extended_data[0]);
  samples[0] = 0.25F;

  const AudioFrameAdapter input_adapter(frame);
  const auto& input = input_adapter.view();
  CHECK(input.data == samples);
  CHECK(input.sample_rate == 48000);
  CHECK(input.channel_count == 2);
  CHECK(input.samples_per_channel == 16);
  CHECK(input.timestamp.pts == 32);
  CHECK(input.timestamp.duration == 16);

  AudioBufferAdapter output_adapter(frame);
  const auto& output = output_adapter.view();
  CHECK(output.data == samples);
  CHECK(output.channel_count == 2);
  CHECK(output.samples_per_channel == 16);
}

TEST_CASE("音频Adapter拒绝非Processor标准格式") {
  auto frame = CreateAudioFrame();
  frame->sample_rate = 44100;
  CHECK_THROWS_AS(AudioFrameAdapter(frame), std::invalid_argument);
  CHECK_THROWS_AS(AudioBufferAdapter(frame), std::invalid_argument);
}

}  // namespace
