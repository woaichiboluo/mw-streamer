#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {
namespace ffmpeg = mw::streamer::ffmpeg;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr AVRational kNs{1, 1000000000};

int64_t ClockNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}
int64_t Pts(const ffmpeg::Frame& frame) {
  if (frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
      frame->time_base.den <= 0)
    throw std::runtime_error("Invalid frame PTS");
  return av_rescale_q(frame->pts, frame->time_base, kNs);
}

struct Errors {
  std::mutex mutex;
  std::exception_ptr error;
  std::atomic<bool> detected{false};
  template <typename F>
  void Capture(F&& action) noexcept {
    try {
      action();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!error) error = std::current_exception();
      detected.store(true);
    }
  }
  void Rethrow() {
    std::lock_guard<std::mutex> lock(mutex);
    if (error) std::rethrow_exception(error);
  }
};

struct Event {
  int id = -1;
  int64_t pts = 0;
  int64_t callback = 0;
  int64_t sample_wall = 0;
  bool startup = false;
  double frequency = 0;
};

double Sample(const AVFrame& frame, int index) {
  const auto format = static_cast<AVSampleFormat>(frame.format);
  const bool planar = av_sample_fmt_is_planar(format);
  const auto* data = frame.extended_data[0];
  const int offset = planar ? index : index * frame.ch_layout.nb_channels;
  switch (av_get_packed_sample_fmt(format)) {
    case AV_SAMPLE_FMT_FLT:
      return reinterpret_cast<const float*>(data)[offset];
    case AV_SAMPLE_FMT_DBL:
      return reinterpret_cast<const double*>(data)[offset];
    case AV_SAMPLE_FMT_S16:
      return reinterpret_cast<const int16_t*>(data)[offset] / 32768.0;
    case AV_SAMPLE_FMT_S32:
      return reinterpret_cast<const int32_t*>(data)[offset] / 2147483648.0;
    default:
      throw std::runtime_error("Unsupported probe audio sample format");
  }
}

struct Stage {
  std::mutex mutex;
  std::vector<Event> video;
  std::vector<Event> audio;
  int64_t first_video = AV_NOPTS_VALUE, last_video = AV_NOPTS_VALUE;
  int64_t first_audio = AV_NOPTS_VALUE, last_audio = AV_NOPTS_VALUE;
  int incomplete_startup = 0;
  bool white = false;
  bool saw_black = false;
  bool saw_quiet = false;
  bool armed = true;
  bool pending = false;
  int quiet = 0;
  int rate = 48000;
  int samples = 0;
  int crossings = 0;
  int first_crossing = 0;
  int last_crossing = 0;
  double previous = 0;
  Event candidate;

  void Video(const ffmpeg::Frame& frame, int64_t wall) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto pts = Pts(frame);
    if (first_video == AV_NOPTS_VALUE) first_video = pts;
    last_video = pts;
    ffmpeg::Frame downloaded;
    const AVFrame* pixels = frame.get();
    if (frame->format == AV_PIX_FMT_CUDA) {
      ffmpeg::FfmpegException::throwIfError(
          av_hwframe_transfer_data(downloaded.get(), frame.get(), 0),
          "Probe读取CUDA帧");
      pixels = downloaded.get();
    }
    if (pixels->width < 96 || pixels->height < 32 || !pixels->data[0])
      throw std::runtime_error("Probe marker video is too small");
    switch (pixels->format) {
      case AV_PIX_FMT_YUV420P:
      case AV_PIX_FMT_YUVJ420P:
      case AV_PIX_FMT_NV12:
      case AV_PIX_FMT_NV21:
      case AV_PIX_FMT_YUV422P:
      case AV_PIX_FMT_YUV444P:
      case AV_PIX_FMT_GRAY8:
        break;
      default:
        throw std::runtime_error("Unsupported probe video pixel format");
    }
    const bool current =
        pixels->data[0][(pixels->height / 2) * pixels->linesize[0] +
                        pixels->width / 2] > 128;
    if (current && !white) {
      int id = 0;
      for (int bit = 0; bit < 4; ++bit)
        if (pixels->data[0][8 * pixels->linesize[0] + 24 * bit + 8] > 128)
          id |= 1 << bit;
      video.push_back({id, pts, wall, wall, !saw_black, 0});
    }
    if (!current) saw_black = true;
    white = current;
  }

  void Audio(const ffmpeg::Frame& frame, int64_t wall,
             bool block_ends_at_callback = true) {
    std::lock_guard<std::mutex> lock(mutex);
    if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0)
      throw std::runtime_error("Invalid probe audio layout");
    rate = frame->sample_rate;
    const auto start = Pts(frame);
    const auto duration = av_rescale_q(frame->nb_samples, {1, rate}, kNs);
    if (first_audio == AV_NOPTS_VALUE) first_audio = start;
    last_audio = start + duration;
    for (int i = 0; i < frame->nb_samples; ++i) {
      const double value = Sample(*frame.get(), i);
      if (std::abs(value) > 0.2) {
        quiet = 0;
        if (armed) {
          const auto offset = av_rescale_q(i, {1, rate}, kNs);
          candidate = {-1,
                       start + offset,
                       wall,
                       wall - (block_ends_at_callback ? duration : 0) + offset,
                       !saw_quiet,
                       0};
          armed = false;
          pending = true;
          samples = crossings = first_crossing = last_crossing = 0;
        }
      } else if (++quiet >= rate / 100) {
        armed = true;
        saw_quiet = true;
      }
      if (pending) {
        if (previous <= 0 && value > 0) {
          if (crossings++ == 0) first_crossing = samples;
          last_crossing = samples;
        }
        if (++samples == 2048) {
          if (crossings < 3 || last_crossing <= first_crossing) {
            if (candidate.startup) {
              ++incomplete_startup;
              pending = false;
              previous = value;
              continue;
            }
            throw std::runtime_error("Cannot identify probe beep frequency");
          }
          candidate.frequency = static_cast<double>(crossings - 1) * rate /
                                (last_crossing - first_crossing);
          candidate.id =
              static_cast<int>(std::lround((candidate.frequency - 1000) / 200));
          if (candidate.id < 0 || candidate.id > 15 ||
              std::abs(candidate.frequency - (1000 + 200 * candidate.id)) > 60)
            throw std::runtime_error("Unexpected probe beep frequency");
          audio.push_back(candidate);
          pending = false;
        }
      }
      previous = value;
    }
  }
  bool Quiet() {
    std::lock_guard<std::mutex> lock(mutex);
    return saw_black && saw_quiet && !white && !pending && quiet >= rate / 100;
  }
};

struct Metrics {
  size_t matches = 0;
  size_t missing = 0;
  std::vector<double> pts;
  std::vector<double> wall;
};

size_t MissingSeconds(const std::vector<Event>& events, int64_t first,
                      int64_t last) {
  size_t missing = 0;
  if (!events.empty()) {
    const auto leading = events.front().pts - first - 150'000'000;
    const auto trailing = last - events.back().pts - 150'000'000;
    if (leading > 1'000'000'000)
      missing += static_cast<size_t>(leading / 1'000'000'000);
    if (trailing > 1'000'000'000)
      missing += static_cast<size_t>(trailing / 1'000'000'000);
  }
  for (size_t i = 1; i < events.size(); ++i) {
    const auto distance = events[i].pts - events[i - 1].pts;
    const auto seconds = std::lround(static_cast<double>(distance) / 1e9);
    if (seconds > 1) missing += static_cast<size_t>(seconds - 1);
    if (seconds < 1 || events[i].id != ((events[i - 1].id + seconds) % 16))
      ++missing;
  }
  return missing;
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(std::ceil(
                    percentile * static_cast<double>(values.size()))) -
                1];
}

Metrics Report(const char* name, const Stage& stage) {
  Metrics result;
  const auto overlap_start = std::max(stage.first_video, stage.first_audio);
  const auto overlap_end = std::min(stage.last_video, stage.last_audio);
  const auto outside_overlap = [&](const Event& event) {
    return event.pts < overlap_start - 150'000'000 ||
           event.pts > overlap_end + 150'000'000;
  };
  std::vector<bool> used(stage.audio.size(), false);
  for (const auto& video : stage.video) {
    if (outside_overlap(video)) continue;
    size_t closest = stage.audio.size();
    int64_t distance = 500'000'001;
    for (size_t i = 0; i < stage.audio.size(); ++i) {
      const auto delta = std::abs(stage.audio[i].pts - video.pts);
      if (!used[i] && stage.audio[i].id == video.id && delta < distance) {
        closest = i;
        distance = delta;
      }
    }
    if (closest == stage.audio.size()) {
      if (!video.startup) ++result.missing;
      continue;
    }
    used[closest] = true;
    const auto& audio = stage.audio[closest];
    const double delta_pts = static_cast<double>(audio.pts - video.pts) / 1e6;
    const double delta_wall =
        static_cast<double>(audio.sample_wall - video.sample_wall) / 1e6;
    if (video.startup || audio.startup) {
      fmt::print("{} startup id={} pts_delta_ms={:.3f} wall_delta_ms={:.3f}\n",
                 name, video.id, delta_pts, delta_wall);
      continue;
    }
    ++result.matches;
    result.pts.push_back(std::abs(delta_pts));
    result.wall.push_back(std::abs(delta_wall));
  }
  for (size_t i = 0; i < stage.audio.size(); ++i)
    if (!used[i] && !stage.audio[i].startup && !outside_overlap(stage.audio[i]))
      ++result.missing;
  result.missing +=
      MissingSeconds(stage.video, stage.first_video, stage.last_video) +
      MissingSeconds(stage.audio, stage.first_audio, stage.last_audio);
  if (stage.incomplete_startup)
    fmt::print("{} incomplete_startup_audio={}\n", name,
               stage.incomplete_startup);
  fmt::print(
      "{} video_events={} audio_events={} matches={} missing={} "
      "pts_max_ms={:.3f} pts_p95_ms={:.3f} wall_max_ms={:.3f} "
      "wall_p95_ms={:.3f}\n",
      name, stage.video.size(), stage.audio.size(), result.matches,
      result.missing, Percentile(result.pts, 1), Percentile(result.pts, 0.95),
      Percentile(result.wall, 1), Percentile(result.wall, 0.95));
  return result;
}

void Save(const std::string& path, const Stage& input, const Stage& output) {
  std::ofstream file(path);
  if (!file) throw std::runtime_error("Cannot write event TSV");
  file << "stage\ttrack\tid\tpts_ns\tcallback_clock_ns\tsample_wall_"
          "ns\tstartup\tfrequency_hz\n";
  for (const auto& entry :
       {std::pair{"input", &input}, std::pair{"output", &output}}) {
    for (const auto& track : {std::pair{"video", &entry.second->video},
                              std::pair{"audio", &entry.second->audio}}) {
      for (const auto& event : *track.second)
        file << fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.3f}\n", entry.first,
                            track.first, event.id, event.pts, event.callback,
                            event.sample_wall, event.startup ? 1 : 0,
                            event.frequency);
    }
  }
  if (!file) throw std::runtime_error("Cannot finish event TSV");
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 5) {
    fmt::print(stderr, "Usage: {} URL [seconds=120] [cpu|cuda] [events.tsv]\n",
               argv[0]);
    return 2;
  }
  try {
    const int seconds = argc > 2 ? std::stoi(argv[2]) : 120;
    const std::string backend = argc > 3 ? argv[3] : "cpu";
    if (seconds <= 0 || (backend != "cpu" && backend != "cuda"))
      throw std::invalid_argument("Invalid duration or backend");
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
    Errors errors;
    Stage input_events, output_events;
    std::atomic<bool> failed{false}, ready{false}, ended{false};
    ffmpeg::HwDeviceContext device(backend == "cuda"
                                       ? ffmpeg::HwDeviceType::kCuda
                                       : ffmpeg::HwDeviceType::kCpu);
    mw::streamer::Processor processor;
    mw::streamer::Scheduler scheduler;
    scheduler.SetOnVideo([&](const ffmpeg::Frame& frame) {
      const auto wall = ClockNs();
      errors.Capture([&] {
        auto processed = processor.ProcessVideo(frame);
        output_events.Video(processed, wall);
      });
    });
    scheduler.SetOnAudio([&](const ffmpeg::Frame& frame) {
      const auto wall = ClockNs();
      errors.Capture([&] {
        auto processed = processor.ProcessAudio(frame);
        output_events.Audio(processed, wall);
      });
    });
    // No Processor filters are configured: both use the default Frame::Ref().
    processor.SetOnEnded([&] { ended.store(true); });
    scheduler.SetOnEnded([&] { processor.End(); });
    mw::streamer::FfmpegInputConfig input_config;
    input_config.auto_reconnect = false;
    mw::streamer::FfmpegInput input(device, input_config);
    input.SetOnReady([&](const auto& streams) {
      errors.Capture([&] {
        ready.store(processor.Start(streams, device) &&
                    scheduler.Start(streams));
        if (!ready.load()) failed.store(true);
      });
    });
    input.SetOnFrame([&](int, const ffmpeg::Frame& frame) {
      const auto wall = ClockNs();
      errors.Capture([&] {
        if (frame->width > 0) {
          input_events.Video(frame, wall);
          if (!scheduler.SubmitVideo(frame)) failed.store(true);
        } else {
          input_events.Audio(frame, wall, false);
          if (!scheduler.SubmitAudio(frame)) failed.store(true);
        }
      });
    });
    input.SetOnStateChanged([&](mw::streamer::InputState state, int error,
                                std::string_view message) {
      errors.Capture([&] {
        if (state == mw::streamer::InputState::kFailed) {
          failed.store(true);
          fmt::print(stderr, "Input failed: {} {}\n", error, message);
        }
        if (state == mw::streamer::InputState::kEnded) scheduler.Drain();
      });
    });
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    input.Start(argv[1]);
    while (!failed.load() && !ended.load() && !errors.detected.load()) {
      if (Clock::now() >= deadline &&
          ((input_events.Quiet() && output_events.Quiet()) ||
           Clock::now() >= deadline + 300ms))
        break;
      std::this_thread::sleep_for(10ms);
    }
    input.Stop();
    scheduler.Stop();
    processor.Stop();
    const auto input_result = Report("input", input_events);
    const auto output_result = Report("output", output_events);
    if (argc > 4) Save(argv[4], input_events, output_events);
    errors.Rethrow();
    fmt::print(
        "Wall values reconstruct sample delivery from callback timing; "
        "this is not playback-device latency measurement.\n");
    const auto pts_reference = output_result.pts.empty() ? "unmeasurable"
                               : Percentile(output_result.pts, 1) <= 41
                                   ? "met"
                                   : "exceeded";
    const auto wall_reference = output_result.wall.empty() ? "unmeasurable"
                                : Percentile(output_result.wall, 1) <= 41
                                    ? "met"
                                    : "exceeded";
    fmt::print("41ms comparison: pts={} sample_wall={} (reference only)\n",
               pts_reference, wall_reference);
    const bool successful =
        ready.load() && !failed.load() && input_result.matches >= 8 &&
        output_result.matches >= 8 && input_result.missing == 0 &&
        output_result.missing == 0;
    fmt::print("{}\n", successful
                           ? "MEASUREMENT SUCCESSFUL (no hard A/V skew limit)"
                           : "MEASUREMENT FAILED / INCOMPLETE");
    return successful ? 0 : 1;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Probe failed: {}\n", error.what());
    return 1;
  }
}
