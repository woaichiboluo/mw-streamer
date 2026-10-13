#include <fmt/format.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/input/av_input_proxy.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoder;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::AvInputReadStatus;

TEST_CASE("Proxy selected packets feed separate decoders across looping EOF",
          "[av-decoder][integration][generation][eof]") {
  mw::streamer::AvInputProxyConfig input_config;
  input_config.loop = true;
  input_config.input.packet_capacity = 1;
  mw::streamer::AvInputProxy proxy(input_config);
  const auto started = std::chrono::steady_clock::now();
  const auto overall_deadline = started + 30s;
  auto progress_deadline = started + 3s;
  proxy.Start(fmt::format("{}/h264_aac.mp4", MW_AV_DECODER_TEST_DATA_DIR));
  mw::streamer::AvInputProxyReadResult input;
  do {
    input = proxy.TryReadPacket();
    const auto now = std::chrono::steady_clock::now();
    REQUIRE(now < overall_deadline);
    REQUIRE(now < progress_deadline);
    if (input.status == AvInputReadStatus::kNull) {
      std::this_thread::sleep_for(1ms);
    }
  } while (input.status == AvInputReadStatus::kNull);
  REQUIRE(input.status == AvInputReadStatus::kPacket);
  progress_deadline = std::chrono::steady_clock::now() + 3s;
  REQUIRE(input.info != nullptr);
  const auto info = input.info;
  REQUIRE(info->video_stream_index >= 0);
  REQUIRE(info->audio_stream_index >= 0);
  mw::streamer::AvDecoderConfig config;
  config.thread_count = 1;
  config.frame_capacity = 1;
  std::unique_ptr<AvDecoder> video;
  std::unique_ptr<AvDecoder> audio;
  for (const auto& stream : info->streams) {
    if (stream.stream_index == info->video_stream_index) {
      CHECK(stream.frame_rate.num > 0);
      video = std::make_unique<AvDecoder>(stream, config);
    } else if (stream.stream_index == info->audio_stream_index) {
      audio = std::make_unique<AvDecoder>(stream, config);
    }
  }
  REQUIRE(video != nullptr);
  REQUIRE(audio != nullptr);
  std::array<std::array<std::size_t, 2>, 2> frame_counts{};
  std::array<std::array<bool, 2>, 2> ended{};
  bool source_ended = false;
  const auto consume = [&](AvDecoder& decoder, std::size_t track,
                           int stream_index) {
    bool progress = false;
    for (;;) {
      const auto result = decoder.TryReadFrame();
      if (result.status == AvDecoderReadStatus::kNull) break;
      progress = true;
      REQUIRE(result.generation < 2);
      const auto generation = static_cast<std::size_t>(result.generation);
      CHECK(result.stream_index == stream_index);
      CHECK_FALSE(ended[track][generation]);
      if (generation == 1) CHECK(ended[track][0]);
      if (result.status == AvDecoderReadStatus::kEof) {
        CHECK_FALSE(result.frame.has_value());
        CHECK(result.end_pts != AV_NOPTS_VALUE);
        ended[track][generation] = true;
      } else {
        REQUIRE(result.frame.has_value());
        CHECK(result.frame->get()->pts ==
              result.frame->get()->best_effort_timestamp);
        ++frame_counts[track][generation];
      }
    }
    REQUIRE(decoder.error() == nullptr);
    return progress;
  };
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    REQUIRE(now < overall_deadline);
    REQUIRE(now < progress_deadline);
    bool progress = consume(*video, 0, info->video_stream_index);
    progress |= consume(*audio, 1, info->audio_stream_index);
    if (ended[0][1] && ended[1][1]) break;
    if (!source_ended && (video->wants_packet() || audio->wants_packet())) {
      if (input.status == AvInputReadStatus::kPacket) {
        REQUIRE(input.packet.has_value());
        if (input.packet->get()->stream_index == info->video_stream_index) {
          REQUIRE(
              video->PushPacket(std::move(*input.packet), input.generation));
        } else {
          REQUIRE(input.packet->get()->stream_index ==
                  info->audio_stream_index);
          REQUIRE(
              audio->PushPacket(std::move(*input.packet), input.generation));
        }
        progress = true;
      } else if (input.status == AvInputReadStatus::kEof) {
        REQUIRE(video->PushEof(input.generation));
        REQUIRE(audio->PushEof(input.generation));
        source_ended = input.generation == 1;
        progress = true;
      }
      input = {};
      if (!source_ended) {
        input = proxy.TryReadPacket();
        progress |= input.status != AvInputReadStatus::kNull;
      }
    }
    // Only packets, frames and EOF reset the stall deadline, not empty polls.
    if (progress) {
      progress_deadline = std::chrono::steady_clock::now() + 3s;
    } else {
      std::this_thread::sleep_for(1ms);
    }
  }
  proxy.Stop();
  video->Stop();
  audio->Stop();
  for (const auto& counts : frame_counts) {
    REQUIRE(counts[0] > 0);
    CHECK(counts[1] == counts[0]);
  }
}

}  // namespace
