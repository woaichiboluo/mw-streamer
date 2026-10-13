#include <fmt/format.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "mw/streamer/input/av_frame_source.h"
#include "mw/streamer/input/av_input_proxy.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoder;
using mw::streamer::AvFrameSourceReadStatus;
using mw::streamer::AvInputReadStatus;

TEST_CASE("Proxy packets become cached audiovisual source bundles across loops",
          "[av-frame-source][integration][generation]") {
  mw::streamer::AvInputProxyConfig input_config;
  input_config.loop = true;
  input_config.input.packet_capacity = 2;
  mw::streamer::AvInputProxy proxy(input_config);
  proxy.Start(fmt::format("{}/h264_aac.mp4", MW_AV_FRAME_SOURCE_TEST_DATA_DIR));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  mw::streamer::AvInputProxyReadResult packet;
  do {
    packet = proxy.TryReadPacket();
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    if (packet.status == AvInputReadStatus::kNull)
      std::this_thread::sleep_for(1ms);
  } while (packet.status == AvInputReadStatus::kNull);
  REQUIRE(packet.status == AvInputReadStatus::kPacket);
  REQUIRE(packet.info != nullptr);
  const auto info = packet.info;
  mw::streamer::AvDecoderConfig decode_config;
  decode_config.thread_count = 1;
  std::unique_ptr<AvDecoder> video;
  std::unique_ptr<AvDecoder> audio;
  for (const auto& stream : info->streams) {
    if (stream.stream_index == info->video_stream_index) {
      video = std::make_unique<AvDecoder>(stream, decode_config);
    } else if (stream.stream_index == info->audio_stream_index) {
      audio = std::make_unique<AvDecoder>(stream, decode_config);
    }
  }
  REQUIRE(video != nullptr);
  REQUIRE(audio != nullptr);
  mw::streamer::AvEncoderConfig output;
  output.fps = {25, 1};
  output.width = 1920;
  output.height = 1080;
  output.video_pixel_format = AV_PIX_FMT_RGB24;
  mw::streamer::AvFrameSource source(video.get(), audio.get(), output,
                                     info->start_time_us);
  CHECK(source.frame_capacity() == 6);
  std::array<std::size_t, 2> count{};
  std::array<bool, 2> eof{};
  std::optional<mw::streamer::AvFrameSourceReadResult> retained;
  bool submitted_last_eof = false;
  for (;;) {
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    bool progress = false;
    for (;;) {
      auto result = source.TryReadFrame();
      if (result.status == AvFrameSourceReadStatus::kNull) break;
      progress = true;
      REQUIRE(result.generation < 2);
      const auto generation = static_cast<std::size_t>(result.generation);
      CHECK_FALSE(eof[generation]);
      if (generation == 1) CHECK(eof[0]);
      if (result.status == AvFrameSourceReadStatus::kEof) {
        CHECK_FALSE(result.video.has_value());
        CHECK_FALSE(result.audio.has_value());
        CHECK(result.frame_index == count[generation]);
        eof[generation] = true;
      } else {
        REQUIRE(result.video.has_value());
        REQUIRE(result.audio.has_value());
        CHECK(result.frame_index == count[generation]++);
        CHECK(result.video->get()->width ==
              video->stream_info().codec_parameters.get()->width);
        CHECK(result.video->get()->height ==
              video->stream_info().codec_parameters.get()->height);
        CHECK(result.video->get()->format != AV_PIX_FMT_RGB24);
        CHECK(result.audio->get()->format == AV_SAMPLE_FMT_FLTP);
        CHECK(result.audio->get()->sample_rate == 48000);
        CHECK(result.audio->get()->ch_layout.nb_channels == 2);
        CHECK(result.pts_us != AV_NOPTS_VALUE);
        CHECK(result.duration_us > 0);
        if (!retained) retained = std::move(result);
      }
    }
    REQUIRE(source.error() == nullptr);
    if (eof[1]) break;
    if (!submitted_last_eof &&
        (video->wants_packet() || audio->wants_packet())) {
      if (packet.status == AvInputReadStatus::kPacket) {
        REQUIRE(packet.packet.has_value());
        if (packet.packet->get()->stream_index == info->video_stream_index) {
          REQUIRE(
              video->PushPacket(std::move(*packet.packet), packet.generation));
        } else {
          REQUIRE(packet.packet->get()->stream_index ==
                  info->audio_stream_index);
          REQUIRE(
              audio->PushPacket(std::move(*packet.packet), packet.generation));
        }
        progress = true;
      } else if (packet.status == AvInputReadStatus::kEof) {
        REQUIRE(video->PushEof(packet.generation));
        REQUIRE(audio->PushEof(packet.generation));
        submitted_last_eof = packet.generation == 1;
        progress = true;
      }
      packet = {};
      if (!submitted_last_eof) packet = proxy.TryReadPacket();
    }
    if (!progress) std::this_thread::sleep_for(1ms);
  }
  source.Stop();
  video->Stop();
  audio->Stop();
  proxy.Stop();
  REQUIRE(count[0] > 0);
  CHECK(count[1] == count[0]);
  REQUIRE(retained.has_value());
  REQUIRE(retained->video.has_value());
  CHECK(retained->video->get()->data[0] != nullptr);
  CHECK(retained->audio->get()->nb_samples > 0);
}

}  // namespace
