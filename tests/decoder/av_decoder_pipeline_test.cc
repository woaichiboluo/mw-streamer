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
          "[av-decoder][integration][reset][eof]") {
  mw::streamer::AvInputProxyConfig input_config;
  input_config.loop = true;
  input_config.input.packet_capacity = 1;
  mw::streamer::AvInputProxy proxy(input_config);
  const auto started = std::chrono::steady_clock::now();
  const auto overall_deadline = started + 30s;
  auto progress_deadline = started + 3s;
  REQUIRE(
      proxy.Start(fmt::format("{}/h264_aac.mp4", MW_AV_DECODER_TEST_DATA_DIR)));
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
  config.packet_capacity = 1;
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
  std::array<std::size_t, 2> eof_counts{};
  std::size_t source_eof_count = 0;
  auto current_info = info;
  bool source_ended = false;
  std::array<bool, 2> reset_submitted{};
  std::array<bool, 2> eof_submitted{};
  const std::array<AvDecoder*, 2> decoders{video.get(), audio.get()};
  const auto consume = [&](AvDecoder& decoder, std::size_t track,
                           int stream_index) {
    bool progress = false;
    {
      const auto result = decoder.TryReadFrame();
      if (result.status == AvDecoderReadStatus::kNull) return false;
      progress = true;
      const auto replay = eof_counts[track];
      REQUIRE(replay < 2);
      CHECK(result.stream_index == stream_index);
      if (result.status == AvDecoderReadStatus::kEof) {
        CHECK_FALSE(result.frame.has_value());
        CHECK(result.end_pts != AV_NOPTS_VALUE);
        ++eof_counts[track];
      } else {
        REQUIRE(result.frame.has_value());
        CHECK(result.frame->get()->pts ==
              result.frame->get()->best_effort_timestamp);
        ++frame_counts[track][replay];
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
    if (eof_counts[0] == 2 && eof_counts[1] == 2) break;
    if (!source_ended) {
      if (input.status == AvInputReadStatus::kNull) {
        input = proxy.TryReadPacket();
        progress |= input.status != AvInputReadStatus::kNull;
      }
      bool ready = input.status != AvInputReadStatus::kNull;
      if (ready && input.info != current_info) {
        REQUIRE(input.info != nullptr);
        for (std::size_t track = 0; track < decoders.size(); ++track) {
          if (!reset_submitted[track] && decoders[track]->TryReset()) {
            reset_submitted[track] = true;
            progress = true;
          }
        }
        ready = reset_submitted[0] && reset_submitted[1];
        if (ready) {
          current_info = input.info;
          reset_submitted = {};
        }
      }
      if (ready && input.status == AvInputReadStatus::kPacket) {
        REQUIRE(input.packet.has_value());
        const auto stream_index = input.packet->get()->stream_index;
        REQUIRE((stream_index == info->video_stream_index ||
                 stream_index == info->audio_stream_index));
        auto& target =
            stream_index == info->video_stream_index ? *video : *audio;
        if (target.TryPushPacket(*input.packet)) {
          input = {};
          progress = true;
        }
      } else if (ready && input.status == AvInputReadStatus::kEof) {
        for (std::size_t track = 0; track < decoders.size(); ++track) {
          if (!eof_submitted[track] && decoders[track]->TryPushEof()) {
            eof_submitted[track] = true;
            progress = true;
          }
        }
        if (eof_submitted[0] && eof_submitted[1]) {
          source_ended = ++source_eof_count == 2;
          eof_submitted = {};
          input = {};
        }
      }
    }
    CHECK(video->buffered_packets() <= config.packet_capacity);
    CHECK(audio->buffered_packets() <= config.packet_capacity);
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
