#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "Extension/Track.h"
#include "Record/MP4Demuxer.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"
#include "mw/decoder/video_decoder.h"
#include "mw/processor/internal/frame_adapter.h"

namespace {

using mediakit::Frame;
using mediakit::FrameWriterInterface;
using mediakit::MP4Demuxer;
using mediakit::Track;
using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::decoder::VideoDecoder;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::decoder::VideoDecoderConfig;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::processor::internal::VideoFrameAdapter;

struct VideoSample {
  StreamInfo stream_info;
  std::vector<Packet> packets;
};

std::string SamplePath() {
  return std::string(MW_VIDEO_DECODER_TEST_DATA_DIR) + "/h264_video.mp4";
}

VideoSample LoadVideoSample() {
  MP4Demuxer demuxer;
  demuxer.openMP4(SamplePath());

  Track::Ptr video_track;
  for (const auto& track : demuxer.getTracks(false)) {
    if (track->getTrackType() == mediakit::TrackVideo) {
      video_track = track;
      break;
    }
  }
  if (!video_track) {
    throw std::runtime_error("测试媒体缺少视频轨");
  }

  auto parameters = std::make_shared<ZlmCodecParametersConverter>(video_track);
  VideoSample sample{
      StreamInfo{0, parameters->codec_parameters(), parameters->time_base()},
      {}};

  auto packet_converter = std::make_shared<ZlmPacketConverter>(video_track, 0);
  packet_converter->SetOnPacket([&](const Packet& packet) {
    sample.packets.push_back(packet.Clone());
    return true;
  });

  FrameWriterInterface* delegate =
      video_track->addDelegate([packet_converter](const Frame::Ptr& frame) {
        return packet_converter->InputFrame(frame);
      });
  if (!delegate) {
    throw std::runtime_error("测试视频轨添加代理失败");
  }

  bool eof = false;
  while (!eof) {
    bool key_frame = false;
    int error = 0;
    demuxer.readFrame(key_frame, eof, &error);
    if (error != 0) {
      video_track->delDelegate(delegate);
      throw std::runtime_error("读取测试媒体失败");
    }
  }
  if (!packet_converter->Flush()) {
    video_track->delDelegate(delegate);
    throw std::runtime_error("刷新测试视频包失败");
  }

  video_track->delDelegate(delegate);
  demuxer.closeMP4();
  return sample;
}

}  // namespace

TEST_CASE("software video decoder decodes drains and flushes an H264 stream") {
  auto sample = LoadVideoSample();
  VideoDecoderConfig config;
  config.backend = VideoDecoderBackend::kSoftware;
  config.decoder_name = "h264";
  VideoDecoder decoder(sample.stream_info, config);

  CHECK(decoder.config().backend == VideoDecoderBackend::kSoftware);
  CHECK(decoder.config().decoder_name == "h264");
  CHECK(decoder.hardware_context() == nullptr);

  std::size_t decoded_frames = 0;
  std::int64_t previous_pts = AV_NOPTS_VALUE;
  bool valid_frames = true;
  decoder.SetOnFrame([&](const mw::streamer::ffmpeg::Frame& decoded_frame) {
    const auto* frame = decoded_frame.get();
    if (!frame || frame->format == AV_PIX_FMT_NONE ||
        frame->format == AV_PIX_FMT_CUDA || frame->width != 64 ||
        frame->height != 64 || frame->hw_frames_ctx) {
      valid_frames = false;
      return;
    }
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
        frame->pts != frame->best_effort_timestamp) {
      valid_frames = false;
    }
    if (frame->pts != AV_NOPTS_VALUE && previous_pts != AV_NOPTS_VALUE &&
        frame->pts < previous_pts) {
      valid_frames = false;
    }
    try {
      const VideoFrameAdapter adapter(decoded_frame);
      if (adapter.view().buffer.memory_type != kMwStreamerMemoryHost) {
        valid_frames = false;
      }
    } catch (const std::exception&) {
      valid_frames = false;
    }
    previous_pts = frame->pts;
    ++decoded_frames;
  });

  std::uint64_t reported_frames = 0;
  for (const auto& packet : sample.packets) {
    reported_frames += decoder.Decode(packet).frames;
  }
  reported_frames += decoder.Drain().frames;

  const auto first_pass_frames = decoded_frames;
  CHECK(valid_frames);
  CHECK(sample.packets.size() == 20);
  CHECK(first_pass_frames == sample.packets.size());
  CHECK(reported_frames == first_pass_frames);
  CHECK_THROWS_AS(decoder.Decode(sample.packets.front()), std::logic_error);
  CHECK_NOTHROW(decoder.Drain());

  decoder.Flush();
  previous_pts = AV_NOPTS_VALUE;
  for (const auto& packet : sample.packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();

  CHECK(valid_frames);
  CHECK(decoded_frames == first_pass_frames * 2);
}

TEST_CASE("CUDA video decoder emits CUDA frames on the configured device") {
  const auto sample = LoadVideoSample();
  VideoDecoder decoder(sample.stream_info);

  REQUIRE(decoder.hardware_context() != nullptr);
  CHECK(decoder.config().backend == VideoDecoderBackend::kCuda);
  CHECK(decoder.hardware_context()->device_index() == 0);

  std::size_t decoded_frames = 0;
  bool valid_frames = true;
  decoder.SetOnFrame([&](const mw::streamer::ffmpeg::Frame& decoded_frame) {
    const auto* frame = decoded_frame.get();
    if (!frame || frame->format != AV_PIX_FMT_CUDA || frame->width != 64 ||
        frame->height != 64 || !frame->hw_frames_ctx) {
      valid_frames = false;
      return;
    }
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
        frame->pts != frame->best_effort_timestamp) {
      valid_frames = false;
      return;
    }

    const auto* frames_context =
        reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (!frames_context || !frames_context->device_ref ||
        frames_context->device_ref->data !=
            decoder.hardware_context()->get()->data ||
        frames_context->sw_format != AV_PIX_FMT_NV12) {
      valid_frames = false;
      return;
    }
    try {
      const VideoFrameAdapter adapter(decoded_frame);
      if (adapter.view().buffer.memory_type != kMwStreamerMemoryCuda ||
          adapter.view().buffer.pixel_format !=
              kMwStreamerVideoPixelFormatNv12) {
        valid_frames = false;
      }
    } catch (const std::exception&) {
      valid_frames = false;
    }
    ++decoded_frames;
  });

  for (const auto& packet : sample.packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();

  CHECK(valid_frames);
  CHECK(decoded_frames == sample.packets.size());
}

TEST_CASE("video decoder rejects invalid decoder selections") {
  const auto sample = LoadVideoSample();

  VideoDecoderConfig missing_decoder;
  missing_decoder.decoder_name = "mw_missing_video_decoder";
  CHECK_THROWS_AS(VideoDecoder(sample.stream_info, missing_decoder),
                  std::invalid_argument);

  VideoDecoderConfig mismatched_decoder;
  mismatched_decoder.decoder_name = "hevc";
  CHECK_THROWS_AS(VideoDecoder(sample.stream_info, mismatched_decoder),
                  std::invalid_argument);

  VideoDecoderConfig invalid_device;
  invalid_device.device_index = -1;
  CHECK_THROWS_AS(VideoDecoder(sample.stream_info, invalid_device),
                  std::invalid_argument);

  CodecParameters audio_parameters;
  audio_parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_parameters.get()->codec_id = AV_CODEC_ID_AAC;
  CHECK_THROWS_AS(VideoDecoder(StreamInfo{0, audio_parameters, {1, 48000}}),
                  std::invalid_argument);
}
