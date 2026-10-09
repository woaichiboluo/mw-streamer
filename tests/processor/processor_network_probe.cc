#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/platform/platform.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {
namespace ffmpeg = mw::streamer::ffmpeg;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr AVRational kNs{1, 1000000000};
constexpr uint64_t kMagic = 0x4d5750524f424541;

int64_t Now() {
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
int64_t SamplesNs(int samples) {
  return av_rescale_q(samples, {1, 48000}, kNs);
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

struct Pcm {
  uint64_t sequence;
  int64_t pts;
  int64_t wall;
  std::vector<float> samples;
};
struct Metadata {
  uint64_t magic = kMagic;
  std::shared_ptr<Pcm> reference;
};
struct Row {
  const char* stage;
  const char* track;
  uint64_t sequence;
  int64_t pts;
  int64_t wall;
  int64_t best;
  uintptr_t buffer;
  int samples;
  int rate;
  int format;
  int64_t source_pts = AV_NOPTS_VALUE;
  const char* status = "input";
  uint64_t source_sequence = 0;
  int source_sample_offset = 0;
  float channel0_peak = 0;
};
struct VideoReference {
  uintptr_t buffer;
  int64_t best;
  int64_t pts;
  int64_t wall;
  uint64_t sequence;
};

struct Probe {
  std::mutex mutex;
  std::vector<Row> rows;
  std::deque<VideoReference> videos;
  std::deque<std::shared_ptr<Pcm>> pcm;
  uint64_t sequence = 0;
  bool unsupported = false;
  bool gpu_ok = true;
  bool cuda = false;
  const AVHWDeviceContext* expected_device = nullptr;
  int64_t started = Now();

  Row Describe(const char* stage, const ffmpeg::Frame& frame, int64_t wall) {
    const bool video = frame->width > 0;
    if (video && cuda) {
      gpu_ok &= frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx;
      if (frame->hw_frames_ctx) {
        const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
            frame->hw_frames_ctx->data);
        gpu_ok &= pool->device_ctx == expected_device;
      }
    }
    Row row{stage,
            video ? "video" : "audio",
            ++sequence,
            Pts(frame),
            wall,
            frame->best_effort_timestamp,
            reinterpret_cast<uintptr_t>(frame->buf[0] ? frame->buf[0]->data
                                                      : nullptr),
            frame->nb_samples,
            frame->sample_rate,
            frame->format};
    if (!video && frame->format == AV_SAMPLE_FMT_FLTP) {
      const auto* samples =
          reinterpret_cast<const float*>(frame->extended_data[0]);
      for (int i = 0; i < frame->nb_samples; ++i) {
        row.channel0_peak = std::max(row.channel0_peak, std::abs(samples[i]));
      }
    }
    return row;
  }

  ffmpeg::Frame Input(const ffmpeg::Frame& frame, int64_t wall) {
    std::lock_guard<std::mutex> lock(mutex);
    auto row = Describe("input", frame, wall);
    rows.push_back(row);
    while (!videos.empty() && videos.front().wall < wall - 5'000'000'000)
      videos.pop_front();
    while (!pcm.empty() && pcm.front()->wall < wall - 5'000'000'000)
      pcm.pop_front();
    auto tracked = frame.Ref();
    if (frame->width > 0) {
      videos.push_back({row.buffer, row.best, row.pts, wall, row.sequence});
      return tracked;
    }
    if (frame->format != AV_SAMPLE_FMT_FLTP || frame->sample_rate != 48000 ||
        frame->ch_layout.nb_channels <= 0) {
      unsupported = true;
      rows.back().status = "audio_reference_unsupported";
      return tracked;
    }
    auto reference = std::make_shared<Pcm>();
    reference->sequence = row.sequence;
    reference->pts = row.pts;
    reference->wall = wall;
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    reference->samples.assign(samples, samples + frame->nb_samples);
    pcm.push_back(reference);
    auto* metadata = new Metadata{kMagic, reference};
    auto* buffer = av_buffer_create(
        reinterpret_cast<uint8_t*>(metadata), sizeof(Metadata),
        [](void*, uint8_t* data) { delete reinterpret_cast<Metadata*>(data); },
        nullptr, 0);
    if (!buffer) {
      delete metadata;
      throw std::bad_alloc();
    }
    av_buffer_unref(&tracked->opaque_ref);
    tracked->opaque_ref = buffer;
    return tracked;
  }

  // Resolve a sample position across contiguous input frames, without keeping
  // decoder frames or GPU surfaces alive in the reference ring.
  bool Reference(size_t chunk, int sample, float& value, int64_t& pts) {
    while (chunk < pcm.size() &&
           sample >= static_cast<int>(pcm[chunk]->samples.size())) {
      sample -= static_cast<int>(pcm[chunk]->samples.size());
      // RTMP timestamps are millisecond-quantized. Allow that rounding at a
      // frame boundary; all 128 PCM values must still match exactly.
      if (chunk + 1 < pcm.size() &&
          std::abs(pcm[chunk + 1]->pts - pcm[chunk]->pts -
                   SamplesNs(static_cast<int>(pcm[chunk]->samples.size()))) >
              2'000'000)
        return false;
      ++chunk;
    }
    if (chunk >= pcm.size() || sample < 0) return false;
    value = pcm[chunk]->samples[static_cast<size_t>(sample)];
    pts = pcm[chunk]->pts + SamplesNs(sample);
    return true;
  }

  void AudioSource(Row& row, const ffmpeg::Frame& frame) {
    if (frame->format != AV_SAMPLE_FMT_FLTP || frame->sample_rate != 48000) {
      row.status = "unsupported";
      return;
    }
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    if (frame->nb_samples < 128) {
      row.status = "short";
      return;
    }
    int anchor = -1;
    for (int i = 0; i < frame->nb_samples; ++i) {
      if (std::abs(samples[i]) > 1e-5f) {
        anchor = std::min(i, frame->nb_samples - 128);
        break;
      }
    }
    if (anchor < 0) {
      row.status = "silence";
      return;
    }
    if (!frame->opaque_ref || frame->opaque_ref->size != sizeof(Metadata)) {
      row.status = "no_reference";
      return;
    }
    const auto* metadata =
        reinterpret_cast<const Metadata*>(frame->opaque_ref->data);
    if (metadata->magic != kMagic) {
      row.status = "no_reference";
      return;
    }
    size_t chunk = 0;
    while (chunk < pcm.size() &&
           pcm[chunk]->sequence != metadata->reference->sequence)
      ++chunk;
    if (chunk == pcm.size()) {
      row.status = "expired_reference";
      return;
    }
    int matches = 0;
    int64_t matched = AV_NOPTS_VALUE;
    const int limit =
        static_cast<int>(pcm[chunk]->samples.size()) + frame->nb_samples;
    for (int candidate = 0; candidate < limit; ++candidate) {
      bool equal = true;
      int64_t source = 0;
      for (int i = 0; i < 128; ++i) {
        float sample = 0;
        int64_t sample_pts = 0;
        if (!Reference(chunk, candidate + i, sample, sample_pts) ||
            sample != samples[anchor + i]) {
          equal = false;
          break;
        }
        if (i == 0) source = sample_pts;
      }
      if (equal) {
        matched = source - SamplesNs(anchor);
        ++matches;
      }
    }
    if (matches == 1) {
      row.source_pts = matched;
      row.source_sequence = metadata->reference->sequence;
      row.source_sample_offset = static_cast<int>(
          av_rescale_q(matched - metadata->reference->pts, kNs, {1, 48000}));
      row.status = "matched";
    } else
      row.status = matches == 0 ? "pcm_mismatch" : "ambiguous";
  }

  void Output(const ffmpeg::Frame& frame, int64_t wall) {
    std::lock_guard<std::mutex> lock(mutex);
    auto row = Describe("output", frame, wall);
    if (frame->width > 0) {
      int matches = 0;
      for (const auto& reference : videos) {
        if (reference.buffer == row.buffer && reference.best == row.best) {
          row.source_pts = reference.pts;
          row.source_sequence = reference.sequence;
          ++matches;
        }
      }
      row.status = matches == 1   ? "matched"
                   : matches == 0 ? "video_lost"
                                  : "ambiguous";
      if (matches != 1) row.source_pts = AV_NOPTS_VALUE;
    } else
      AudioSource(row, frame);
    rows.push_back(row);
  }
};

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(std::ceil(
                    static_cast<double>(values.size()) * percentile)) -
                1];
}

bool Report(const Probe& probe) {
  std::vector<const Row*> audio;
  size_t input_video = 0, input_audio = 0, output_video = 0, output_audio = 0;
  size_t silent = 0, short_blocks = 0, lost_audio = 0, lost_video = 0;
  for (const auto& row : probe.rows) {
    const bool video = std::string(row.track) == "video";
    if (std::string(row.stage) == "input") {
      video ? ++input_video : ++input_audio;
    } else {
      video ? ++output_video : ++output_audio;
      if (row.source_pts != AV_NOPTS_VALUE && !video)
        audio.push_back(&row);
      else if (row.source_pts == AV_NOPTS_VALUE) {
        if (std::string(row.status) == "silence")
          ++silent;
        else if (std::string(row.status) == "short")
          ++short_blocks;
        else
          video ? ++lost_video : ++lost_audio;
      }
    }
  }
  std::vector<double> startup, skew;
  size_t no_audio_pair = 0;
  for (const auto& video : probe.rows) {
    if (std::string(video.stage) != "output" ||
        std::string(video.track) != "video" ||
        video.source_pts == AV_NOPTS_VALUE)
      continue;
    const Row* closest = nullptr;
    int64_t distance = 40'000'001;
    for (const auto* candidate : audio) {
      const auto delta = std::abs(candidate->pts - video.pts);
      if (delta < distance) {
        closest = candidate;
        distance = delta;
      }
    }
    if (!closest) {
      ++no_audio_pair;
      continue;
    }
    const double difference =
        static_cast<double>(std::abs((video.pts - video.source_pts) -
                                     (closest->pts - closest->source_pts))) /
        1e6;
    (video.wall < probe.started + 2'000'000'000 ? startup : skew)
        .push_back(difference);
  }
  fmt::print(
      "frames input_video={} input_audio={} output_video={} output_audio={}\n",
      input_video, input_audio, output_video, output_audio);
  fmt::print(
      "audio_reference={} audio_matches={} silence={} short={} lost_audio={} "
      "lost_video={} video_without_near_audio={} gpu_context_ok={}\n",
      probe.unsupported ? "UNSUPPORTED: requires input 48000 Hz FLTP"
                        : "exact PCM",
      audio.size(), silent, short_blocks, lost_audio, lost_video, no_audio_pair,
      probe.gpu_ok);
  fmt::print(
      "startup_2s pairs={} added_skew_max_ms={:.3f} added_skew_p95_ms={:.3f}\n",
      startup.size(), Percentile(startup, 1), Percentile(startup, 0.95));
  fmt::print(
      "steady pairs={} added_skew_max_ms={:.3f} added_skew_p95_ms={:.3f}\n",
      skew.size(), Percentile(skew, 1), Percentile(skew, 0.95));
  fmt::print(
      "FrameMetrics measure pipeline-added timestamp skew using unchanged "
      "buffers/PCM. "
      "The external source's original A/V sync is unknown; this does not "
      "measure lip sync "
      "or playback-device latency. Silence and unmatched PCM are not inferred "
      "matches.\n");
  const auto reference = skew.empty()                ? "unmeasurable"
                         : Percentile(skew, 1) <= 41 ? "met"
                                                     : "exceeded";
  fmt::print("41ms comparison: steady_added_skew={} (reference only)\n",
             reference);
  return !probe.unsupported && probe.gpu_ok && skew.size() >= 8 &&
         lost_video == 0 && lost_audio == 0;
}

void Save(const std::string& path, const Probe& probe) {
  std::ofstream file(path);
  if (!file) throw std::runtime_error("Cannot write frame TSV");
  file << "stage\ttrack\tsequence\tpts_ns\tcallback_ns\tbest_effort_"
          "timestamp\tbuffer\t"
          "samples\trate\tformat\tsource_pts_ns\tmatch_status\toffset_ns\t"
          "source_sequence\tsource_sample_offset\tchannel0_peak\n";
  for (const auto& row : probe.rows) {
    const auto offset = row.source_pts == AV_NOPTS_VALUE
                            ? AV_NOPTS_VALUE
                            : row.pts - row.source_pts;
    file << fmt::format(
        "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
        row.stage, row.track, row.sequence, row.pts, row.wall, row.best,
        row.buffer, row.samples, row.rate, row.format, row.source_pts,
        row.status, offset, row.source_sequence, row.source_sample_offset,
        row.channel0_peak);
  }
  if (!file) throw std::runtime_error("Cannot finish frame TSV");
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 5) {
    fmt::print(stderr, "Usage: {} URL [seconds=120] [cpu|cuda] [frames.tsv]\n",
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
    const std::string runtime_log =
        argc > 4 ? std::string(argv[4]) + ".log" : "";
    if (!runtime_log.empty()) {
      const bool trace_enabled =
          mw::streamer::internal::HasEnvironmentVariable("MW_STREAMER_PROBE_TRACE");
      config.log.modules = trace_enabled
                               ? "streamer:trace;perf.input:trace;"
                                 "perf.decoder.video:trace;"
                                 "perf.decoder.audio:trace;perf.scheduler:trace"
                               : "streamer:debug;perf.input:info;"
                                 "perf.decoder.video:info;"
                                 "perf.decoder.audio:info;perf.scheduler:info";
      config.log.modules_size = std::strlen(config.log.modules);
      config.log.rotating_file_enabled = 1;
      config.log.rotating_file_path = runtime_log.c_str();
      config.log.rotating_file_path_size = runtime_log.size();
    }
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
    Errors errors;
    Probe probe;
    std::atomic<bool> ready{false}, failed{false}, ended{false};
    std::atomic<int> rejected_video{0}, rejected_audio{0};
    ffmpeg::HwDeviceContext device(backend == "cuda"
                                       ? ffmpeg::HwDeviceType::kCuda
                                       : ffmpeg::HwDeviceType::kCpu);
    probe.cuda = backend == "cuda";
    probe.expected_device =
        device.get()
            ? reinterpret_cast<const AVHWDeviceContext*>(device.get()->data)
            : nullptr;
    mw::streamer::Processor processor;
    mw::streamer::Scheduler scheduler;
    scheduler.SetOnVideo([&](const ffmpeg::Frame& frame) {
      const auto wall = Now();
      errors.Capture([&] {
        auto processed = processor.ProcessVideo(frame);
        probe.Output(processed, wall);
      });
    });
    scheduler.SetOnAudio([&](const ffmpeg::Frame& frame) {
      const auto wall = Now();
      errors.Capture([&] {
        auto processed = processor.ProcessAudio(frame);
        probe.Output(processed, wall);
      });
    });
    // Keep both Processor filters unset: default Frame::Ref() pass-through.
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
      const auto wall = Now();
      errors.Capture([&] {
        auto tracked = probe.Input(frame, wall);
        const bool accepted = frame->width > 0 ? scheduler.SubmitVideo(tracked)
                                               : scheduler.SubmitAudio(tracked);
        if (!accepted) {
          if (frame->width > 0)
            ++rejected_video;
          else
            ++rejected_audio;
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
    input.Start(argv[1]);
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (!failed.load() && !ended.load() && !errors.detected.load() &&
           Clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    input.Stop();
    scheduler.Stop();
    processor.Stop();
    if (argc > 4) Save(argv[4], probe);
    errors.Rethrow();
    fmt::print(
        "rejected_video={} rejected_audio={} ready={} failed={} ended={}\n",
        rejected_video.load(), rejected_audio.load(), ready.load(),
        failed.load(), ended.load());
    const bool measured = Report(probe);
    const bool successful = measured && ready.load() && !failed.load();
    fmt::print("{}\n", successful
                           ? "MEASUREMENT SUCCESSFUL (no hard A/V skew limit)"
                           : "MEASUREMENT FAILED / UNMEASURABLE");
    return successful ? 0 : 1;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Probe failed: {}\n", error.what());
    return 1;
  }
}
