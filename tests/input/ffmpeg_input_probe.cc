#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using Clock = std::chrono::steady_clock;
using mw::streamer::InputState;

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
  uint64_t frames = 0;
  uint64_t gpu_frames = 0;
  size_t generation = 0;
  int frame_format = -1;
  double first_pts = 0;
  double last_pts = 0;
  Clock::time_point first_arrival;
  Clock::time_point last_arrival;
  std::optional<ffmpeg::Frame> retained;
};

struct Result {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<StreamResult> streams;
  size_t ready = 0;
  size_t retries = 0;
  size_t connections = 0;
  uint64_t total_frames = 0;
  bool terminal = false;
  bool failed = false;
  bool invalid_frame = false;
  std::exception_ptr error;
  Clock::time_point started;
};

template <class Callback>
void Capture(Result& result, Callback callback) noexcept {
  try {
    std::lock_guard<std::mutex> lock(result.mutex);
    callback();
  } catch (...) {
    std::lock_guard<std::mutex> lock(result.mutex);
    result.error = std::current_exception();
  }
  result.changed.notify_one();
}

int Probe(std::string_view url, int seconds, bool loop,
          std::chrono::seconds open_timeout, ffmpeg::HwDeviceType device_type) {
  Result result;
  mw::streamer::FfmpegInputConfig config;
  config.loop = loop;
  config.open_timeout = open_timeout;
  ffmpeg::HwDeviceContext device(device_type);
  const auto* native_device =
      device.get()
          ? reinterpret_cast<const AVHWDeviceContext*>(device.get()->data)
          : nullptr;
  mw::streamer::FfmpegInput input(device, config);
  input.SetOnReady([&](const std::vector<ffmpeg::StreamInfo>& streams) {
    Capture(result, [&] {
      ++result.ready;
      for (const auto& info : streams) {
        info.Validate();
        const auto existing =
            std::find_if(result.streams.begin(), result.streams.end(),
                         [&](const auto& item) {
                           return item.info.stream_index == info.stream_index;
                         });
        if (existing == result.streams.end()) {
          StreamResult stream;
          stream.info = info;
          result.streams.push_back(std::move(stream));
        } else {
          existing->info = info;
        }
      }
    });
  });
  input.SetOnFrame([&](int index, const ffmpeg::Frame& frame) {
    Capture(result, [&] {
      const auto stream = std::find_if(
          result.streams.begin(), result.streams.end(),
          [&](const auto& item) { return item.info.stream_index == index; });
      if (stream == result.streams.end() || !frame.get() || !frame->data[0] ||
          !frame->buf[0] || frame->pts == AV_NOPTS_VALUE ||
          av_cmp_q(frame->time_base, AVRational{1, 1000000000}) != 0 ||
          frame->pkt_dts != AV_NOPTS_VALUE) {
        result.invalid_frame = true;
        return;
      }
      const auto now = Clock::now();
      const auto pts = frame->pts * av_q2d(frame->time_base);
      const auto type = stream->info.codec_parameters.get()->codec_type;
      if (type == AVMEDIA_TYPE_VIDEO) {
        const bool gpu = frame->format == AV_PIX_FMT_CUDA;
        const auto* pool = frame->hw_frames_ctx
                               ? reinterpret_cast<const AVHWFramesContext*>(
                                     frame->hw_frames_ctx->data)
                               : nullptr;
        if (native_device ? (!gpu || !pool || pool->device_ctx != native_device)
                          : (gpu || pool)) {
          result.invalid_frame = true;
          return;
        }
        stream->gpu_frames += gpu;
      } else if (frame->nb_samples <= 0 || frame->hw_frames_ctx) {
        result.invalid_frame = true;
        return;
      }
      stream->frame_format = frame->format;
      if (stream->generation != result.connections) {
        stream->generation = result.connections;
        fmt::print(
            "first_frame connection={} stream={} type={} elapsed={:.3f} "
            "pts={:.3f} media_pts={:.3f} gpu={}\n",
            result.connections, index,
            type == AVMEDIA_TYPE_VIDEO ? "video" : "audio",
            std::chrono::duration<double>(now - result.started).count(), pts,
            frame->best_effort_timestamp == AV_NOPTS_VALUE
                ? 0.0
                : frame->best_effort_timestamp * av_q2d(stream->info.time_base),
            static_cast<int>(type == AVMEDIA_TYPE_VIDEO &&
                             native_device != nullptr));
      }
      if (stream->frames == 0) {
        stream->first_pts = pts;
        stream->first_arrival = now;
        stream->retained.emplace(frame.Ref());
      }
      ++stream->frames;
      ++result.total_frames;
      stream->last_pts = pts;
      stream->last_arrival = now;
    });
  });
  input.SetOnStateChanged([&](InputState state, int code,
                              std::string_view message) {
    Capture(result, [&] {
      result.retries += state == InputState::kWaitingRetry;
      result.connections += state == InputState::kConnected;
      result.terminal =
          state == InputState::kEnded || state == InputState::kFailed;
      result.failed |= state == InputState::kFailed;
      const double elapsed =
          std::chrono::duration<double>(Clock::now() - result.started).count();
      fmt::print("state={} elapsed={:.3f} error={} message={}\n",
                 StateName(state), elapsed, code, message);
    });
  });
  result.started = Clock::now();
  fmt::print("url={} duration={} loop={} device={}\n", url, seconds,
             static_cast<int>(loop), native_device ? "cuda" : "cpu");
  input.Start(url);
  {
    std::unique_lock<std::mutex> lock(result.mutex);
    result.changed.wait_for(lock, std::chrono::seconds(seconds), [&] {
      return result.terminal || result.invalid_frame || result.error;
    });
  }
  input.Stop();
  if (result.error) std::rethrow_exception(result.error);
  for (const auto& stream : result.streams) {
    const auto* parameters = stream.info.codec_parameters.get();
    const double active = std::chrono::duration<double>(stream.last_arrival -
                                                        stream.first_arrival)
                              .count();
    const char* frame_format =
        parameters->codec_type == AVMEDIA_TYPE_VIDEO
            ? av_get_pix_fmt_name(
                  static_cast<AVPixelFormat>(stream.frame_format))
            : av_get_sample_fmt_name(
                  static_cast<AVSampleFormat>(stream.frame_format));
    fmt::print(
        "stream={} codec={} time_base={}/{} size={}x{} sample_rate={} "
        "frames={} "
        "active_seconds={:.3f} gpu_frames={} frame_format={} "
        "frames_per_second={:.3f} pts_span={:.3f}\n",
        stream.info.stream_index, avcodec_get_name(parameters->codec_id),
        stream.info.time_base.num, stream.info.time_base.den, parameters->width,
        parameters->height, parameters->sample_rate, stream.frames, active,
        stream.gpu_frames, frame_format ? frame_format : "none",
        active > 0 ? (stream.frames - 1) / active : 0,
        stream.last_pts - stream.first_pts);
    if (stream.retained && !stream.retained->get()->buf[0]) {
      result.invalid_frame = true;
    }
    if (stream.gpu_frames && stream.retained) {
      ffmpeg::Frame downloaded;
      if (av_hwframe_transfer_data(downloaded.get(), stream.retained->get(),
                                   0) < 0) {
        result.invalid_frame = true;
      }
    }
  }
  const bool success =
      result.total_frames > 0 && !result.invalid_frame && !result.failed &&
      std::all_of(result.streams.begin(), result.streams.end(),
                  [](const auto& stream) { return stream.frames > 0; });
  fmt::print("ready={} retries={} connections={} total_frames={} result={}\n",
             result.ready, result.retries, result.connections,
             result.total_frames, success ? "PASS" : "FAIL");
  return success ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 6) {
    fmt::print(stderr,
               "Usage: ffmpeg_input_probe URL [seconds=120] [once|loop] "
               "[open_timeout_seconds=30] [cpu|cuda]\n");
    return 2;
  }
  try {
    const int seconds = argc >= 3 ? std::stoi(argv[2]) : 120;
    if (seconds <= 0) throw std::invalid_argument("Duration must be positive");
    bool loop = false;
    if (argc >= 4) {
      const std::string_view argument(argv[3]);
      if (argument == "loop") {
        loop = true;
      } else if (argument != "once") {
        throw std::invalid_argument("Loop setting must be once or loop");
      }
    }
    const auto open_timeout =
        std::chrono::seconds(argc >= 5 ? std::stoi(argv[4]) : 30);
    auto device_type = ffmpeg::HwDeviceType::kCpu;
    if (argc == 6) {
      const std::string_view argument(argv[5]);
      if (argument == "cuda") {
        device_type = ffmpeg::HwDeviceType::kCuda;
      } else if (argument != "cpu") {
        throw std::invalid_argument("Device must be cpu or cuda");
      }
    }
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        context(mw::streamer::Init(config), &mw::streamer::Shutdown);
    return Probe(argv[1], seconds, loop, open_timeout, device_type);
  } catch (const std::exception& error) {
    fmt::print(stderr, "FAIL {}\n", error.what());
    return 1;
  }
}
