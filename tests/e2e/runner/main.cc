#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "Poller/EventPoller.h"
#include "mw/cache/packet_queue.h"
#include "mw/init/init.h"
#include "mw/input/player_proxy.h"
#include "mw/output/output_session.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::cache::PacketQueue;
using mw::streamer::cache::PacketQueueState;
using mw::streamer::cache::PacketStream;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::input::PlayerProxy;
using mw::streamer::input::PlayerState;
using mw::streamer::output::OutputConfig;
using mw::streamer::output::OutputSession;

std::atomic_bool g_stop_requested = false;

void HandleSignal(int) {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

const char* ToString(PlayerState state) {
  switch (state) {
    case PlayerState::kIdle:
      return "idle";
    case PlayerState::kConnecting:
      return "connecting";
    case PlayerState::kReady:
      return "ready";
    case PlayerState::kWaitingRetry:
      return "waiting_retry";
    case PlayerState::kEnded:
      return "ended";
    case PlayerState::kFailed:
      return "failed";
    case PlayerState::kStopped:
      return "stopped";
  }
  return "unknown";
}

const char* ToString(PacketQueueState state) {
  switch (state) {
    case PacketQueueState::kFilling:
      return "filling";
    case PacketQueueState::kPlaying:
      return "playing";
    case PacketQueueState::kPaused:
      return "paused";
    case PacketQueueState::kStarved:
      return "starved";
    case PacketQueueState::kStopped:
      return "stopped";
  }
  return "unknown";
}

const char* ToString(AVMediaType media_type) {
  switch (media_type) {
    case AVMEDIA_TYPE_AUDIO:
      return "audio";
    case AVMEDIA_TYPE_VIDEO:
      return "video";
    default:
      return "unsupported";
  }
}

class EventWriter final {
 public:
  explicit EventWriter(const std::string& path)
      : started_at_(std::chrono::steady_clock::now()), output_(path) {
    if (!output_) {
      throw std::runtime_error("无法打开事件输出文件: " + path);
    }
  }

  void Write(
      const std::string& event,
      const std::vector<std::pair<std::string, std::string>>& fields = {}) {
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_)
            .count();
    auto line = fmt::format("ts_ms={} event={}", elapsed_ms, event);
    for (const auto& [key, value] : fields) {
      fmt::format_to(std::back_inserter(line), " {}={}", key, value);
    }
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    output_.write(line.data(), static_cast<std::streamsize>(line.size()));
    output_.flush();
  }

 private:
  std::chrono::steady_clock::time_point started_at_;
  std::mutex mutex_;
  std::ofstream output_;
};

struct Arguments {
  std::string input;
  std::vector<std::string> outputs;
  std::string events_path;
  std::chrono::milliseconds cache_duration{1000};
  std::chrono::milliseconds duration{10000};
};

std::string RequireValue(int argc, char* argv[], int& index) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("缺少参数值: ") + argv[index]);
  }
  return argv[++index];
}

std::chrono::milliseconds ParseMilliseconds(const std::string& value,
                                            const char* option,
                                            std::int64_t minimum) {
  std::size_t parsed = 0;
  const auto number = std::stoll(value, &parsed);
  if (parsed != value.size() || number < minimum) {
    throw std::invalid_argument(
        fmt::format("{}必须为不小于{}的整数", option, minimum));
  }
  return std::chrono::milliseconds(number);
}

Arguments ParseArguments(int argc, char* argv[]) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--input") {
      arguments.input = RequireValue(argc, argv, index);
    } else if (option == "--output") {
      arguments.outputs.push_back(RequireValue(argc, argv, index));
    } else if (option == "--events") {
      arguments.events_path = RequireValue(argc, argv, index);
    } else if (option == "--cache-ms") {
      arguments.cache_duration =
          ParseMilliseconds(RequireValue(argc, argv, index), "--cache-ms", 0);
    } else if (option == "--duration-ms") {
      arguments.duration = ParseMilliseconds(RequireValue(argc, argv, index),
                                             "--duration-ms", 1);
    } else {
      throw std::invalid_argument("未知参数: " + option);
    }
  }

  if (arguments.input.empty()) {
    throw std::invalid_argument("--input不能为空");
  }
  if (arguments.events_path.empty()) {
    throw std::invalid_argument("--events不能为空");
  }
  return arguments;
}

int Run(const Arguments& arguments) {
  mw::streamer::InitConfig init_config;
  init_config.log.console.color = false;
  init_config.log.modules.zlm = mw::streamer::log::LogLevel::kInfo;
  init_config.log.modules.streamer = mw::streamer::log::LogLevel::kInfo;
  mw::streamer::Init(init_config);

  EventWriter events(arguments.events_path);
  auto poller = toolkit::EventPollerPool::Instance().getPoller();
  auto queue = std::make_shared<PacketQueue>(arguments.cache_duration, poller);
  auto player = std::make_shared<PlayerProxy>(poller);
  std::shared_ptr<OutputSession> output;

  std::atomic_bool ready_seen = false;
  std::atomic_bool failed = false;
  std::atomic_int audio_stream_index = -1;
  std::atomic_int video_stream_index = -1;
  std::atomic_uint64_t packet_generation = 0;
  std::atomic_uint64_t audio_packets = 0;
  std::atomic_uint64_t video_packets = 0;
  std::atomic_uint64_t total_packets = 0;

  queue->SetOnState(
      [&events](std::uint64_t generation, PacketQueueState state) {
        events.Write("queue_state", {{"generation", std::to_string(generation)},
                                     {"state", ToString(state)}});
      });
  queue->SetOnTimelineReset([&events](std::uint64_t generation) {
    events.Write("timeline_reset",
                 {{"generation", std::to_string(generation)}});
  });
  queue->SetOnPacket([&](std::uint64_t, const Packet& packet) {
    ++total_packets;
    if (packet->stream_index ==
        audio_stream_index.load(std::memory_order_relaxed)) {
      ++audio_packets;
    }
    if (packet->stream_index ==
        video_stream_index.load(std::memory_order_relaxed)) {
      ++video_packets;
    }
    if (output) {
      output->Write(packet);
    }
  });

  player->SetOnStreamsReady([&](std::uint64_t generation,
                                const std::vector<StreamInfo>& streams) {
    std::vector<PacketStream> packet_streams;
    std::vector<StreamInfo> output_streams;
    packet_streams.reserve(streams.size());
    output_streams.reserve(streams.size());
    audio_stream_index.store(-1, std::memory_order_relaxed);
    video_stream_index.store(-1, std::memory_order_relaxed);
    audio_packets.store(0, std::memory_order_relaxed);
    video_packets.store(0, std::memory_order_relaxed);
    total_packets.store(0, std::memory_order_relaxed);
    packet_generation.store(generation, std::memory_order_relaxed);

    for (const auto& stream : streams) {
      const auto media_type = stream.codec_parameters.get()->codec_type;
      packet_streams.push_back(
          {stream.stream_index, media_type, stream.time_base});
      output_streams.push_back(
          {stream.stream_index, stream.codec_parameters, stream.time_base});
      if (media_type == AVMEDIA_TYPE_AUDIO) {
        audio_stream_index.store(stream.stream_index,
                                 std::memory_order_relaxed);
      } else if (media_type == AVMEDIA_TYPE_VIDEO) {
        video_stream_index.store(stream.stream_index,
                                 std::memory_order_relaxed);
      }
      events.Write("stream",
                   {{"generation", std::to_string(generation)},
                    {"stream_index", std::to_string(stream.stream_index)},
                    {"media_type", ToString(media_type)},
                    {"codec_id",
                     std::to_string(stream.codec_parameters.get()->codec_id)}});
    }

    queue->SetStreams(generation, std::move(packet_streams));
    if (!output && !arguments.outputs.empty()) {
      OutputConfig config;
      config.streams = std::move(output_streams);
      config.targets = arguments.outputs;
      output = std::make_shared<OutputSession>(std::move(config), poller);
      output->Open();
      events.Write(
          "output_opened",
          {{"target_count", std::to_string(arguments.outputs.size())}});
    }
    events.Write("streams_ready",
                 {{"generation", std::to_string(generation)},
                  {"stream_count", std::to_string(streams.size())}});
  });
  player->SetOnPacket([&](std::uint64_t generation, const Packet& packet) {
    return queue->Input(generation, packet);
  });
  player->SetOnState([&](std::uint64_t generation, PlayerState state,
                         const toolkit::SockException& reason,
                         bool will_retry) {
    if (state == PlayerState::kReady) {
      ready_seen.store(true, std::memory_order_relaxed);
    }
    if (state == PlayerState::kWaitingRetry || state == PlayerState::kEnded ||
        state == PlayerState::kFailed) {
      queue->EndInput(generation);
    }
    if (state == PlayerState::kFailed) {
      failed.store(true, std::memory_order_relaxed);
    }
    events.Write("player_state", {{"generation", std::to_string(generation)},
                                  {"state", ToString(state)},
                                  {"reason", reason.what()},
                                  {"will_retry", will_retry ? "1" : "0"},
                                  {"reconnect_count",
                                   std::to_string(player->reconnect_count())}});
  });

  events.Write("runner_started");
  player->Start(arguments.input);

  const auto finish_at = std::chrono::steady_clock::now() + arguments.duration;
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < finish_at) {
    std::this_thread::sleep_for(1s);
    events.Write(
        "heartbeat",
        {{"generation", std::to_string(packet_generation.load())},
         {"reconnect_count", std::to_string(player->reconnect_count())},
         {"total_packets", std::to_string(total_packets.load())},
         {"audio_packets", std::to_string(audio_packets.load())},
         {"video_packets", std::to_string(video_packets.load())},
         {"queued_packets", std::to_string(queue->packet_count())}});
  }

  auto stopped = std::make_shared<std::promise<void>>();
  auto stopped_future = stopped->get_future();
  player->Stop([stopped]() { stopped->set_value(); });
  stopped_future.wait();
  queue->Stop();
  poller->sync([]() {});
  if (output) {
    output->Close();
  }

  events.Write(
      "summary",
      {{"ready_seen", ready_seen.load() ? "1" : "0"},
       {"failed", failed.load() ? "1" : "0"},
       {"total_packets", std::to_string(total_packets.load())},
       {"audio_packets", std::to_string(audio_packets.load())},
       {"video_packets", std::to_string(video_packets.load())},
       {"reconnect_count", std::to_string(player->reconnect_count())}});

  output.reset();
  player.reset();
  queue.reset();
  mw::streamer::Shutdown();

  return ready_seen.load() && total_packets.load() > 0 && !failed.load() ? 0
                                                                         : 2;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  try {
    return Run(ParseArguments(argc, argv));
  } catch (const std::exception& error) {
    fmt::print(stderr, "mw_streamer_e2e_runner: {}\n", error.what());
    if (mw::streamer::IsInitialized()) {
      mw::streamer::Shutdown();
    }
    return 1;
  }
}
