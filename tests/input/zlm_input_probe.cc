#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mw/streamer/init/init.h"
#include "mw/streamer/input/zlm_input.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::InputState;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;

const char* StateName(InputState state) {
  switch (state) {
    case InputState::kIdle:
      return "Idle";
    case InputState::kConnecting:
      return "Connecting";
    case InputState::kConnected:
      return "Connected";
    case InputState::kWaitingRetry:
      return "WaitingRetry";
    case InputState::kEnded:
      return "Ended";
    case InputState::kFailed:
      return "Failed";
    case InputState::kStopped:
      return "Stopped";
  }
  return "Unknown";
}

struct StreamResult {
  ffmpeg::StreamInfo info;
  std::uint64_t packet_count = 0;
  std::uint64_t byte_count = 0;
  std::optional<ffmpeg::Packet> retained_packet;
};

struct StateResult {
  InputState state;
  int error_code;
  std::string message;
};

struct ProbeResult {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<StreamResult> streams;
  std::vector<StateResult> states;
  std::exception_ptr callback_error;
  bool terminal = false;
  bool invalid_packet = false;

  bool HasAllPackets() const {
    return !streams.empty() &&
           std::all_of(streams.begin(), streams.end(),
                       [](const StreamResult& stream) {
                         return stream.retained_packet.has_value();
                       });
  }
};

// Exceptions stay in the probe and are reported after Stop(), outside ZLM.
template <typename Callback>
void CaptureCallback(ProbeResult& result, Callback callback) noexcept {
  try {
    std::lock_guard<std::mutex> lock(result.mutex);
    callback();
  } catch (...) {
    std::lock_guard<std::mutex> lock(result.mutex);
    result.callback_error = std::current_exception();
  }
  result.changed.notify_one();
}

bool Probe(std::string_view url) {
  ProbeResult result;
  ZlmInputConfig config;
  config.auto_reconnect = false;
  ZlmInput input(config);
  input.SetOnReady([&](const std::vector<ffmpeg::StreamInfo>& streams) {
    CaptureCallback(result, [&] {
      result.streams.clear();
      for (const auto& stream : streams) {
        stream.Validate();
        StreamResult stream_result;
        stream_result.info = stream;
        result.streams.push_back(std::move(stream_result));
      }
    });
  });
  input.SetOnPacket([&](const ffmpeg::Packet& packet) {
    CaptureCallback(result, [&] {
      const auto stream = std::find_if(
          result.streams.begin(), result.streams.end(),
          [&](const StreamResult& candidate) {
            return candidate.info.stream_index == packet->stream_index;
          });
      if (stream == result.streams.end() || !packet->buf || !packet->data ||
          packet->size <= 0 ||
          av_cmp_q(packet->time_base, stream->info.time_base) != 0) {
        result.invalid_packet = true;
        return;
      }
      ++stream->packet_count;
      stream->byte_count += static_cast<std::uint64_t>(packet->size);
      if (!stream->retained_packet) {
        stream->retained_packet.emplace(packet.Ref());
      }
    });
  });
  input.SetOnStateChanged(
      [&](InputState state, int error_code, std::string_view message) {
        CaptureCallback(result, [&] {
          result.states.push_back({state, error_code, std::string(message)});
          result.terminal =
              state == InputState::kFailed || state == InputState::kEnded;
        });
      });

  std::cout << "URL " << url << '\n';
  input.Start(url);
  bool timed_out = false;
  {
    std::unique_lock<std::mutex> lock(result.mutex);
    timed_out = !result.changed.wait_for(lock, std::chrono::seconds(25), [&] {
      return result.HasAllPackets() || result.terminal ||
             result.invalid_packet || result.callback_error;
    });
  }
  input.Stop();

  for (const auto& state : result.states) {
    std::cout << "  state=" << StateName(state.state)
              << " error=" << state.error_code;
    if (!state.message.empty()) {
      std::cout << " message=" << state.message;
    }
    std::cout << '\n';
  }
  if (result.callback_error) {
    std::rethrow_exception(result.callback_error);
  }

  bool retained_valid = true;
  for (const auto& stream : result.streams) {
    const auto* parameters = stream.info.codec_parameters.get();
    std::cout << "  stream=" << stream.info.stream_index
              << " codec=" << avcodec_get_name(parameters->codec_id)
              << " codec_id=" << parameters->codec_id
              << " size=" << parameters->width << 'x' << parameters->height
              << " sample_rate=" << parameters->sample_rate
              << " channels=" << parameters->ch_layout.nb_channels
              << " extradata=" << parameters->extradata_size
              << " time_base=" << stream.info.time_base.num << '/'
              << stream.info.time_base.den << " packets=" << stream.packet_count
              << " bytes=" << stream.byte_count;
    if (stream.retained_packet) {
      const auto& packet = *stream.retained_packet;
      retained_valid &= packet->buf && packet->data && packet->size > 0;
      std::cout << " packet_time_base=" << packet->time_base.num << '/'
                << packet->time_base.den << " pts=" << packet->pts
                << " dts=" << packet->dts;
    }
    std::cout << '\n';
  }

  const bool success = result.HasAllPackets() && retained_valid &&
                       !result.invalid_packet && !timed_out;
  std::cout << "  " << (success ? "PASS" : "FAIL");
  if (timed_out) {
    std::cout << " timeout=25s";
  }
  if (result.invalid_packet) {
    std::cout << " invalid_packet";
  }
  std::cout << '\n';
  return success;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: zlm_input_probe URL [URL ...]\n";
    return 2;
  }
  bool success = true;
  mw::streamer::InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  const std::unique_ptr<mw::streamer::MwStreamerContext,
                        decltype(&mw::streamer::Shutdown)>
      context(mw::streamer::Init(config), &mw::streamer::Shutdown);
  for (int i = 1; i < argc; ++i) {
    try {
      success = Probe(argv[i]) && success;
    } catch (const std::exception& error) {
      std::cerr << "  FAIL " << error.what() << '\n';
      success = false;
    } catch (...) {
      std::cerr << "  FAIL unknown exception\n";
      success = false;
    }
  }
  return success ? 0 : 1;
}
