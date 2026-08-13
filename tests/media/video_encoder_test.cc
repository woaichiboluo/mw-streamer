#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/decoder/video_decoder.h"
#include "mw/encoder/video_encoder.h"
#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"

namespace {

using mw::streamer::decoder::VideoDecoder;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::decoder::VideoDecoderConfig;
using mw::streamer::encoder::VideoEncodeMode;
using mw::streamer::encoder::VideoEncoder;
using mw::streamer::encoder::VideoEncoderConfig;
using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::HardwareContext;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::ThrowIfError;

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr AVRational kTimeBase{1, 25};

Frame MakeVideoFrame(std::int64_t pts) {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = kWidth;
  frame->height = kHeight;
  frame->time_base = kTimeBase;
  frame->pts = pts;
  frame->duration = 1;
  frame->sample_aspect_ratio = {1, 1};
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->chroma_location = AVCHROMA_LOC_LEFT;
  ThrowIfError(av_frame_get_buffer(frame.get(), 32), "分配测试视频帧");
  ThrowIfError(av_frame_make_writable(frame.get()), "写入测试视频帧");

  std::memset(frame->data[0], 16,
              static_cast<std::size_t>(frame->linesize[0]) * kHeight);
  std::memset(frame->data[1], 128,
              static_cast<std::size_t>(frame->linesize[1]) * (kHeight / 2));
  std::memset(frame->data[2], 128,
              static_cast<std::size_t>(frame->linesize[2]) * (kHeight / 2));
  return frame;
}

VideoEncoderConfig MakeConfig() {
  VideoEncoderConfig config;
  config.codec = kMwStreamerCodecH264;
  config.encoder_name = "libx264";
  config.frame_rate = {25, 1};
  config.properties = {
      {"preset", "ultrafast"},
      {"tune", "zerolatency"},
      {"bf", "2"},
      {"mw_unknown_video_option", "ignored"},
  };
  return config;
}

bool StartsWithAnnexB(const Packet& packet) {
  const auto* raw_packet = packet.get();
  return raw_packet && raw_packet->data && raw_packet->size >= 4 &&
         raw_packet->data[0] == 0 && raw_packet->data[1] == 0 &&
         (raw_packet->data[2] == 1 ||
          (raw_packet->data[2] == 0 && raw_packet->data[3] == 1));
}

Frame MakeCudaVideoFrame() {
  constexpr int kCudaWidth = 256;
  constexpr int kCudaHeight = 144;
  const auto hardware_context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(hardware_context.get()));
  if (!frames_ref) {
    throw std::bad_alloc();
  }

  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = kCudaWidth;
  frames_context->height = kCudaHeight;
  frames_context->initial_pool_size = 4;
  try {
    ThrowIfError(av_hwframe_ctx_init(frames_ref), "初始化测试CUDA视频帧池");

    Frame software;
    software->format = AV_PIX_FMT_NV12;
    software->width = kCudaWidth;
    software->height = kCudaHeight;
    ThrowIfError(av_frame_get_buffer(software.get(), 32), "分配测试CUDA上传帧");
    std::memset(software->data[0], 16,
                static_cast<std::size_t>(software->linesize[0]) * kCudaHeight);
    std::memset(
        software->data[1], 128,
        static_cast<std::size_t>(software->linesize[1]) * (kCudaHeight / 2));

    Frame cuda;
    ThrowIfError(av_hwframe_get_buffer(frames_ref, cuda.get(), 0),
                 "分配测试CUDA视频帧");
    ThrowIfError(av_hwframe_transfer_data(cuda.get(), software.get(), 0),
                 "上传测试CUDA视频帧");
    cuda->time_base = kTimeBase;
    cuda->duration = 1;
    cuda->sample_aspect_ratio = {1, 1};
    cuda->color_range = AVCOL_RANGE_MPEG;
    cuda->colorspace = AVCOL_SPC_BT709;
    cuda->color_primaries = AVCOL_PRI_BT709;
    cuda->color_trc = AVCOL_TRC_BT709;
    cuda->chroma_location = AVCHROMA_LOC_LEFT;
    av_buffer_unref(&frames_ref);
    return cuda;
  } catch (...) {
    av_buffer_unref(&frames_ref);
    throw;
  }
}

Frame MakeHostNv12VideoFrame(std::int64_t pts) {
  constexpr int kHostWidth = 256;
  constexpr int kHostHeight = 144;
  Frame frame;
  frame->format = AV_PIX_FMT_NV12;
  frame->width = kHostWidth;
  frame->height = kHostHeight;
  frame->time_base = kTimeBase;
  frame->pts = pts;
  frame->duration = 1;
  frame->sample_aspect_ratio = {1, 1};
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->chroma_location = AVCHROMA_LOC_LEFT;
  ThrowIfError(av_frame_get_buffer(frame.get(), 32), "分配测试Host NV12视频帧");
  ThrowIfError(av_frame_make_writable(frame.get()), "写入测试Host NV12视频帧");

  std::memset(frame->data[0], 16,
              static_cast<std::size_t>(frame->linesize[0]) * kHostHeight);
  std::memset(frame->data[1], 128,
              static_cast<std::size_t>(frame->linesize[1]) * (kHostHeight / 2));
  return frame;
}

}  // namespace

TEST_CASE("software video encoder emits decodable H264 without B frames") {
  VideoEncoder encoder(MakeConfig(), 2);
  CHECK_FALSE(encoder.is_open());
  CHECK_THROWS_AS(encoder.stream_info(), std::logic_error);

  std::vector<Packet> packets;
  encoder.SetOnPacket(
      [&](const Packet& packet) { packets.push_back(packet.Ref()); });

  auto first_frame = MakeVideoFrame(0);
  encoder.Open(first_frame);
  CHECK(encoder.is_open());
  CHECK(encoder.stream_info().stream_index == 2);
  CHECK(encoder.stream_info().time_base.num == kTimeBase.num);
  CHECK(encoder.stream_info().time_base.den == kTimeBase.den);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_type ==
        AVMEDIA_TYPE_VIDEO);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_H264);
  CHECK(encoder.stream_info().codec_parameters.get()->video_delay == 0);
  CHECK_THROWS_AS(encoder.Open(first_frame), std::logic_error);

  constexpr std::int64_t kFrameCount = 12;
  for (std::int64_t pts = 0; pts < kFrameCount; ++pts) {
    auto frame = MakeVideoFrame(pts);
    encoder.Encode(frame, pts == 5 ? VideoEncodeMode::kForceKeyFrame
                                   : VideoEncodeMode::kAutomatic);
  }
  encoder.Drain();

  REQUIRE(packets.size() == kFrameCount);
  CHECK(StartsWithAnnexB(packets.front()));
  bool forced_key_frame = false;
  std::int64_t previous_dts = packets.front()->dts;
  for (const auto& packet : packets) {
    REQUIRE(packet.get());
    CHECK(packet->stream_index == 2);
    CHECK(packet->size > 0);
    CHECK(packet->dts == packet->pts);
    CHECK(packet->dts >= previous_dts);
    if (packet->pts == 5 && (packet->flags & AV_PKT_FLAG_KEY) != 0) {
      forced_key_frame = true;
    }
    previous_dts = packet->dts;
  }
  CHECK(forced_key_frame);
  CHECK_THROWS_AS(encoder.Encode(first_frame), std::logic_error);
  CHECK_NOTHROW(encoder.Drain());

  VideoDecoderConfig decoder_config;
  decoder_config.backend = VideoDecoderBackend::kSoftware;
  decoder_config.decoder_name = "h264";
  VideoDecoder decoder(encoder.stream_info(), decoder_config);
  std::size_t decoded_frames = 0;
  decoder.SetOnFrame([&](const Frame& frame) {
    CHECK(frame->width == kWidth);
    CHECK(frame->height == kHeight);
    CHECK(frame->format == AV_PIX_FMT_YUV420P);
    ++decoded_frames;
  });
  for (const auto& packet : packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();
  CHECK(decoded_frames == kFrameCount);
}

TEST_CASE("video encoder validates codec and prototype") {
  CHECK_THROWS_AS(VideoEncoder(VideoEncoderConfig{}, -1),
                  std::invalid_argument);

  auto config = MakeConfig();
  config.codec = kMwStreamerCodecH265;
  VideoEncoder mismatched(config);
  auto frame = MakeVideoFrame(0);
  CHECK_THROWS_AS(mismatched.Open(frame), std::invalid_argument);

  config = MakeConfig();
  config.frame_rate = {-1, 1};
  CHECK_THROWS_AS(VideoEncoder(config), std::invalid_argument);

  config = MakeConfig();
  VideoEncoder invalid_frame_encoder(config);
  frame->time_base = {0, 1};
  CHECK_THROWS_AS(invalid_frame_encoder.Open(frame), std::invalid_argument);
}

TEST_CASE("CUDA video encoder reuses the Processor hardware frame pool") {
  VideoEncoderConfig config;
  config.codec = kMwStreamerCodecH264;
  config.frame_rate = {25, 1};
  config.properties = {
      {"preset", "p1"},
      {"tune", "ull"},
      {"bf", "0"},
      {"forced-idr", "0"},
  };
  VideoEncoder encoder(config, 4);
  auto prototype = MakeCudaVideoFrame();
  REQUIRE(prototype->format == AV_PIX_FMT_CUDA);
  REQUIRE(prototype->hw_frames_ctx != nullptr);

  std::vector<Packet> packets;
  encoder.SetOnPacket(
      [&](const Packet& packet) { packets.push_back(packet.Ref()); });
  encoder.Open(prototype);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_H264);
  CHECK(encoder.stream_info().codec_parameters.get()->video_delay == 0);

  constexpr std::int64_t kFrameCount = 6;
  for (std::int64_t pts = 0; pts < kFrameCount; ++pts) {
    auto frame = prototype.Ref();
    frame->pts = pts;
    encoder.Encode(frame, pts == 3 ? VideoEncodeMode::kForceKeyFrame
                                   : VideoEncodeMode::kAutomatic);
  }
  encoder.Drain();

  REQUIRE(packets.size() == kFrameCount);
  CHECK(StartsWithAnnexB(packets.front()));
  bool forced_key_frame = false;
  for (const auto& packet : packets) {
    CHECK(packet->stream_index == 4);
    CHECK(packet->dts == packet->pts);
    if (packet->pts == 3 && (packet->flags & AV_PKT_FLAG_KEY) != 0) {
      forced_key_frame = true;
    }
  }
  CHECK(forced_key_frame);

  VideoDecoderConfig decoder_config;
  decoder_config.backend = VideoDecoderBackend::kSoftware;
  decoder_config.decoder_name = "h264";
  VideoDecoder decoder(encoder.stream_info(), decoder_config);
  std::size_t decoded_frames = 0;
  decoder.SetOnFrame([&](const Frame& frame) {
    CHECK(frame->width == 256);
    CHECK(frame->height == 144);
    ++decoded_frames;
  });
  for (const auto& packet : packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();
  CHECK(decoded_frames == kFrameCount);
}

TEST_CASE("NVENC accepts Host NV12 video frames") {
  VideoEncoderConfig config;
  config.codec = kMwStreamerCodecH264;
  config.encoder_name = "h264_nvenc";
  config.frame_rate = {25, 1};
  config.properties = {
      {"preset", "p1"},
      {"tune", "ull"},
  };
  VideoEncoder encoder(config, 6);
  std::vector<Packet> packets;
  encoder.SetOnPacket(
      [&](const Packet& packet) { packets.push_back(packet.Ref()); });

  constexpr std::int64_t kFrameCount = 3;
  auto prototype = MakeHostNv12VideoFrame(0);
  REQUIRE(prototype->hw_frames_ctx == nullptr);
  encoder.Open(prototype);
  for (std::int64_t pts = 0; pts < kFrameCount; ++pts) {
    encoder.Encode(MakeHostNv12VideoFrame(pts));
  }
  encoder.Drain();

  REQUIRE(packets.size() == kFrameCount);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_H264);
  CHECK(StartsWithAnnexB(packets.front()));
}

TEST_CASE("software video encoder supports H265") {
  VideoEncoderConfig config;
  config.codec = kMwStreamerCodecH265;
  config.encoder_name = "libx265";
  config.frame_rate = {25, 1};
  config.properties = {
      {"preset", "ultrafast"},
      {"tune", "zerolatency"},
      {"x265-params", "log-level=error:pools=1"},
      {"bf", "0"},
  };
  VideoEncoder encoder(config, 5);
  std::vector<Packet> packets;
  encoder.SetOnPacket(
      [&](const Packet& packet) { packets.push_back(packet.Ref()); });

  auto prototype = MakeVideoFrame(0);
  encoder.Open(prototype);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_HEVC);
  CHECK(encoder.stream_info().codec_parameters.get()->video_delay == 0);

  constexpr std::int64_t kFrameCount = 3;
  for (std::int64_t pts = 0; pts < kFrameCount; ++pts) {
    auto frame = MakeVideoFrame(pts);
    encoder.Encode(frame);
  }
  encoder.Drain();

  REQUIRE(packets.size() == kFrameCount);
  CHECK(StartsWithAnnexB(packets.front()));
  for (const auto& packet : packets) {
    CHECK(packet->stream_index == 5);
    CHECK(packet->dts == packet->pts);
  }

  VideoDecoderConfig decoder_config;
  decoder_config.backend = VideoDecoderBackend::kSoftware;
  decoder_config.decoder_name = "hevc";
  VideoDecoder decoder(encoder.stream_info(), decoder_config);
  std::size_t decoded_frames = 0;
  decoder.SetOnFrame([&](const Frame&) { ++decoded_frames; });
  for (const auto& packet : packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();
  CHECK(decoded_frames == kFrameCount);
}
