#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "mw/log.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/av_frame_source.h"
#include "mw/streamer/input/av_media_input.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {

using namespace std::chrono_literals;
using namespace mw::streamer;
namespace fs = std::filesystem;

class FilterLog final {
 public:
  FilterLog() {
    static std::atomic<unsigned int> sequence{0};
    path_ = fs::temp_directory_path() /
            ("mw-filter-latency-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence.fetch_add(1)) + ".log");
    InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    config.log.async.enabled = false;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = path_.string();
    config.log.modules = {{"streamer", mw::log::LogLevel::kOff},
                          {"perf.decoder.video", mw::log::LogLevel::kInfo},
                          {"perf.decoder.audio", mw::log::LogLevel::kInfo},
                          {"perf.filter.input.video", mw::log::LogLevel::kInfo},
                          {"perf.filter.input.audio", mw::log::LogLevel::kInfo},
                          {"perf.filter.video", mw::log::LogLevel::kInfo},
                          {"perf.filter.audio", mw::log::LogLevel::kInfo}};
    runtime_.reset(Init(config));
  }

  ~FilterLog() {
    runtime_.reset();
    std::error_code error;
    fs::remove(path_, error);
  }

  std::string Read() {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  }

 private:
  fs::path path_;
  std::unique_ptr<MwStreamerContext, decltype(&Shutdown)> runtime_{nullptr,
                                                                   &Shutdown};
};

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

std::string Summary(const std::string& log, const std::string& stage) {
  const std::regex pattern("[^\\n]*stage=" + stage +
                           " [^\\n]*report=summary[^\\n]*");
  std::smatch match;
  REQUIRE(std::regex_search(log, match, pattern));
  const auto remaining = match.suffix().str();
  CHECK_FALSE(std::regex_search(remaining, pattern));
  return match.str();
}

std::uint64_t Count(const std::string& line, const std::string& key) {
  const std::regex pattern(key + "=([0-9]+)");
  std::smatch match;
  INFO(line);
  REQUIRE(std::regex_search(line, match, pattern));
  return std::stoull(match[1]);
}

TEST_CASE(
    "Input fps latency counts repeated outputs and restores source opaque",
    "[performance][latency][filter]") {
  FilterLog logging;
  std::vector<ffmpeg::Packet> packets;
  ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = {1, 25};
  stream.frame_rate = {25, 1};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
  parameters->format = AV_PIX_FMT_GRAY8;
  parameters->width = 8;
  parameters->height = 8;
  for (int index = 0; index < 6; ++index) {
    ffmpeg::Packet packet;
    REQUIRE(av_new_packet(packet.get(), 64) >= 0);
    std::memset(packet->data, index, 64);
    packet->stream_index = 0;
    packet->pts = packet->dts = index;
    packet->duration = 1;
    packet->opaque_ref = av_buffer_alloc(1);
    REQUIRE(packet->opaque_ref != nullptr);
    packet->opaque_ref->data[0] = static_cast<std::uint8_t>(index);
    packets.push_back(std::move(packet));
  }
  std::size_t frames = 0;
  {
    AvDecoderConfig decoder_config;
    decoder_config.thread_count = 1;
    AvDecoder decoder(stream, decoder_config);
    AvEncoderConfig output;
    output.fps = {50, 1};
    AvFrameSource source(&decoder, nullptr, output);
    for (const auto& packet : packets) REQUIRE(decoder.PushPacket(packet, 0));
    REQUIRE(decoder.PushEof(0));
    const bool reached_eof = WaitFor([&] {
      const auto result = source.TryReadFrame();
      if (result.status == AvFrameSourceReadStatus::kFrame) {
        REQUIRE(result.video.has_value());
        const auto* frame = result.video->get();
        REQUIRE(frame->opaque_ref != nullptr);
        CHECK(frame->opaque_ref->size == 1);
        CHECK(frame->opaque_ref->data[0] == frame->data[0][0]);
        ++frames;
      }
      return result.status == AvFrameSourceReadStatus::kEof;
    });
    REQUIRE(reached_eof);
    CHECK(source.error() == nullptr);
  }
  REQUIRE(frames == packets.size() * 2);
  for (std::size_t index = 0; index < packets.size(); ++index) {
    CHECK(packets[index]->opaque_ref->size == 1);
    CHECK(packets[index]->opaque_ref->data[0] == index);
  }
  const auto summary = Summary(logging.Read(), "filter.input.video");
  CHECK(Count(summary, "samples") == frames);
  CHECK(Count(summary, "unavailable") == 0);
}

TEST_CASE("Input resampling measures propagated representative provenance",
          "[performance][latency][filter]") {
  FilterLog logging;
  std::size_t frames = 0;
  {
    ffmpeg::StreamInfo stream;
    stream.stream_index = 1;
    stream.time_base = {1, 44100};
    auto* parameters = stream.codec_parameters.get();
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
    parameters->format = AV_SAMPLE_FMT_S16;
    parameters->sample_rate = 44100;
    parameters->bits_per_coded_sample = 16;
    av_channel_layout_default(&parameters->ch_layout, 1);
    AvDecoderConfig decoder_config;
    decoder_config.thread_count = 1;
    AvDecoder decoder(stream, decoder_config);
    AvFrameSource source(nullptr, &decoder);
    for (int index = 0; index < 12; ++index) {
      ffmpeg::Packet packet;
      REQUIRE(av_new_packet(packet.get(), 997 * 2) >= 0);
      std::memset(packet->data, 0, 997 * 2);
      packet->stream_index = 1;
      packet->pts = packet->dts = index * 997;
      packet->duration = 997;
      packet->opaque_ref = av_buffer_alloc(1);
      REQUIRE(packet->opaque_ref != nullptr);
      packet->opaque_ref->data[0] = static_cast<std::uint8_t>(index);
      REQUIRE(decoder.PushPacket(std::move(packet), 0));
    }
    REQUIRE(decoder.PushEof(0));
    const bool reached_eof = WaitFor([&] {
      const auto result = source.TryReadFrame();
      if (result.status == AvFrameSourceReadStatus::kFrame) {
        REQUIRE(result.audio.has_value());
        const auto* frame = result.audio->get();
        REQUIRE(frame->opaque_ref != nullptr);
        CHECK(frame->opaque_ref->size == 1);
        CHECK(frame->opaque_ref->data[0] < 12);
        ++frames;
      }
      return result.status == AvFrameSourceReadStatus::kEof;
    });
    REQUIRE(reached_eof);
    CHECK(source.error() == nullptr);
  }
  REQUIRE(frames > 0);
  const auto summary = Summary(logging.Read(), "filter.input.audio");
  CHECK(Count(summary, "samples") == frames);
  CHECK(Count(summary, "unavailable") == 0);
}

TEST_CASE("Scheduler latency measures custom callbacks after workers join",
          "[performance][latency][filter]") {
  const bool custom = GENERATE(false, true);
  FilterLog logging;
  std::atomic<std::uint64_t> video_calls{0};
  std::atomic<std::uint64_t> audio_calls{0};
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  {
    AvMediaInputConfig input_config;
    input_config.proxy.max_retries = 0;
    auto input = std::make_shared<AvMediaInput>(input_config);
    REQUIRE(input->Start(std::string(MW_STREAMER_PERFORMANCE_TEST_DATA_DIR) +
                         "/h264_aac.mp4"));
    SchedulerConfig config;
    config.output.width = 8;
    config.output.height = 8;
    config.output.video_pixel_format = AV_PIX_FMT_GRAY8;
    Scheduler scheduler(config);
    if (custom) {
      scheduler.SetOnVideo([&](const auto&, const auto&) {
        if (++video_calls == 1) {
          std::unique_lock<std::mutex> lock(mutex);
          entered = true;
          changed.notify_all();
          changed.wait(lock, [&] { return released; });
        }
        return ffmpeg::Frame{};
      });
      scheduler.SetOnAudio([&](const auto&, const auto&) {
        ++audio_calls;
        return ffmpeg::Frame{};
      });
    }
    std::atomic<int> output_calls{0};
    scheduler.SetOnOutput([&](const auto&) {
      if (++output_calls == 2) scheduler.RequestStop();
    });
    REQUIRE(scheduler.Start({input}));
    if (custom) {
      bool observed;
      {
        std::unique_lock<std::mutex> lock(mutex);
        observed = changed.wait_for(lock, 2s, [&] { return entered; });
        // Hold an actual callback so scheduler pacing cannot masquerade as
        // latency.
        changed.wait_for(lock, 25ms, [] { return false; });
        released = true;
      }
      changed.notify_all();
      CHECK(observed);
    }
    REQUIRE(WaitFor([&] { return output_calls.load() >= 2; }));
    scheduler.Stop();
    input->Stop();
  }
  const auto log = logging.Read();
  if (custom) {
    const auto video = Summary(log, "filter.video");
    const auto audio = Summary(log, "filter.audio");
    CHECK(Count(video, "samples") == video_calls.load());
    CHECK(Count(audio, "samples") == audio_calls.load());
    std::smatch match;
    REQUIRE(std::regex_search(video, match, std::regex("max:([0-9.]+)ms")));
    CHECK(std::stod(match[1]) >= 20.0);
    CHECK(Count(video, "unavailable") == 0);
    CHECK(Count(audio, "unavailable") == 0);
  } else {
    CHECK(log.find("stage=filter.video ") == std::string::npos);
    CHECK(log.find("stage=filter.audio ") == std::string::npos);
  }
}

}  // namespace
