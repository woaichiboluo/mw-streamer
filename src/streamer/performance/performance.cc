#include "mw/streamer/performance/performance.h"

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/input/ffmpeg_input.h"

namespace mw::streamer::internal {

void InputPerformance::Start(const void* instance) {
  instance_ = instance;
  enabled_ = mw::log::ShouldLog("perf.input", mw::log::LogLevel::kInfo);
  trace_enabled_ = mw::log::ShouldLog("perf.input", mw::log::LogLevel::kTrace);
  active_ = enabled_;
  if (!enabled_) return;
  window_.Reset();
  reference_pts_ns_.reset();
  loops_ = 0;
  seeks_ = 0;
  peak_buffer_bytes_ = 0;
}

void InputPerformance::SegmentReset() noexcept {
  if (enabled_) reference_pts_ns_.reset();
}

void InputPerformance::Seek() noexcept {
  if (!enabled_) return;
  reference_pts_ns_.reset();
  ++seeks_;
}

void InputPerformance::Loop() noexcept {
  if (enabled_) ++loops_;
}

void InputPerformance::Reconnect() noexcept {
  if (enabled_) ++window_.counters().reconnects;
}

void InputPerformance::BeginRead() noexcept {
  if (enabled_) read_started_ = PerformanceWindow::Clock::now();
}

void InputPerformance::BeginWait() noexcept {
  if (trace_enabled_) wait_started_ = PerformanceWindow::Clock::now();
}

void InputPerformance::EndWait() noexcept {
  if (!trace_enabled_) return;
  window_.counters().wait_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          PerformanceWindow::Clock::now() - wait_started_)
          .count();
}

void InputPerformance::Queued(std::size_t bytes) noexcept {
  if (trace_enabled_) peak_buffer_bytes_ = std::max(peak_buffer_bytes_, bytes);
}

bool InputPerformance::Sample(bool final) noexcept {
  if (!enabled_ || !active_) return false;
  if (!window_.Sample(report_, final)) return false;
  final_ = final;
  if (final) active_ = false;
  return true;
}

const char* InputPerformance::StateName(InputState state) noexcept {
  switch (state) {
    case InputState::kIdle:
      return "kIdle";
    case InputState::kConnecting:
      return "kConnecting";
    case InputState::kConnected:
      return "kConnected";
    case InputState::kWaitingRetry:
      return "kWaitingRetry";
    case InputState::kEnded:
      return "kEnded";
    case InputState::kFailed:
      return "kFailed";
    case InputState::kStopped:
      return "kStopped";
  }
  return "unknown";
}

std::array<char, 48> InputPerformance::RateText(double value,
                                                bool available) noexcept {
  std::array<char, 48> text{};
  if (available) {
    std::snprintf(text.data(), text.size(), "%.3f", value);
  } else {
    std::snprintf(text.data(), text.size(), "N/A");
  }
  return text;
}

void InputPerformance::Read(
    const AVPacket& packet, int result, bool stopped,
    const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
  if (!enabled_) return;
  const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               PerformanceWindow::Clock::now() - read_started_)
                               .count();
  window_.AddWork(duration_ns);
  auto& counters = window_.counters();
  if (result == AVERROR_EOF) {
    ++counters.eof;
  } else if (result < 0 && !(result == AVERROR_EXIT && stopped)) {
    ++counters.errors;
    if (result == AVERROR(EAGAIN)) ++counters.again;
  } else if (result >= 0 && packet.size > 0) {
    const auto selected =
        std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
          return stream.stream_index == packet.stream_index;
        });
    if (selected != streams.end()) {
      ++counters.packets;
      counters.bytes += static_cast<std::uint64_t>(packet.size);
      // The selected stream list is video-first; audio is the fallback clock.
      // Demux payload includes both selected tracks, but media time only one.
      if (selected == streams.begin()) {
        const auto pts = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
        if (pts != AV_NOPTS_VALUE) {
          const auto media_ns =
              av_rescale_q(pts, selected->time_base, AVRational{1, 1000000000});
          counters.has_media = true;
          if (reference_pts_ns_ && media_ns > *reference_pts_ns_) {
            counters.media_ns += media_ns - *reference_pts_ns_;
          }
          reference_pts_ns_ = media_ns;
        }
      }
    }
  }
}

void InputPerformance::Report(std::uint64_t generation, InputState state,
                              std::size_t buffered) const {
  const auto& report = report_;
  const bool final = final_;
  const auto& total = report.total;
  const auto& interval = report.interval;
  const auto& selected = final ? total : interval;
  const auto seconds = final ? report.elapsed_seconds : report.interval_seconds;
  const auto selected_media_seconds =
      static_cast<double>(selected.media_ns) / 1e9;
  const auto selected_speed =
      RateText(PerformanceReport::Rate(selected_media_seconds, seconds),
               selected.has_media && selected_media_seconds > 0 && seconds > 0);
  MW_LOG_INFO(
      "perf.input",
      "Input[{}] report={} state={} packets_per_second={:.1f} "
      "payload_MiB_per_second={:.2f} speed={} read_mean_ms={:.2f} "
      "packet_buffer_MiB={:.2f} errors={}",
      instance_, final ? "summary" : "interval", StateName(state),
      PerformanceReport::Rate(selected.packets, seconds),
      PerformanceReport::Rate(static_cast<double>(selected.bytes) / 1048576.0,
                              seconds),
      selected_speed.data(),
      PerformanceReport::Rate(static_cast<double>(selected.work_ns) / 1e6,
                              selected.work_calls),
      static_cast<double>(buffered) / 1048576.0, selected.errors);
  if (!trace_enabled_) return;
  const auto interval_media_seconds =
      static_cast<double>(interval.media_ns) / 1e9;
  const auto total_media_seconds = static_cast<double>(total.media_ns) / 1e9;
  const auto bitrate = RateText(
      PerformanceReport::Rate(8.0 * static_cast<double>(interval.bytes),
                              interval_media_seconds),
      interval.has_media && interval_media_seconds > 0);
  const auto speed = RateText(
      PerformanceReport::Rate(interval_media_seconds, report.interval_seconds),
      interval.has_media && interval_media_seconds > 0 &&
          report.interval_seconds > 0);
  const auto total_bitrate =
      RateText(PerformanceReport::Rate(8.0 * static_cast<double>(total.bytes),
                                       total_media_seconds),
               total.has_media && total_media_seconds > 0);
  const auto total_speed = RateText(
      PerformanceReport::Rate(total_media_seconds, report.elapsed_seconds),
      total.has_media && total_media_seconds > 0 && report.elapsed_seconds > 0);
  MW_LOG_TRACE(
      "perf.input",
      "Input[{}] generation={} state={} report={} elapsed_since_start_s={:.3f} "
      "window_s={:.3f} packets={} payload_bytes={} pps={:.3f} "
      "payload_bytes_per_second={:.3f} media_bitrate_bps={} speed={} "
      "read_calls={} read_mean_ms={:.3f} read_max_ms={:.3f} "
      "read_eof={} read_errors={} read_again={} playback_wait_ms={:.3f} "
      "packet_buffer_bytes={} total_packet_buffer_peak_bytes={} "
      "total_packets={} total_payload_bytes={} total_pps={:.3f} "
      "total_payload_bytes_per_second={:.3f} total_media_bitrate_bps={} "
      "total_speed={} total_read_calls={} total_read_ms={:.3f} "
      "total_read_max_ms={:.3f} total_eof={} total_read_errors={} "
      "total_read_again={} total_playback_wait_ms={:.3f} "
      "total_reconnects={} total_loops={} total_seeks={}",
      instance_, generation, StateName(state), final ? "summary" : "interval",
      report.elapsed_seconds, report.interval_seconds, interval.packets,
      interval.bytes,
      PerformanceReport::Rate(interval.packets, report.interval_seconds),
      PerformanceReport::Rate(interval.bytes, report.interval_seconds),
      bitrate.data(), speed.data(), interval.work_calls,
      PerformanceReport::Rate(static_cast<double>(interval.work_ns) / 1e6,
                              interval.work_calls),
      static_cast<double>(interval.max_work_ns) / 1e6, interval.eof,
      interval.errors, interval.again,
      static_cast<double>(interval.wait_ns) / 1e6, buffered, peak_buffer_bytes_,
      total.packets, total.bytes,
      PerformanceReport::Rate(total.packets, report.elapsed_seconds),
      PerformanceReport::Rate(total.bytes, report.elapsed_seconds),
      total_bitrate.data(), total_speed.data(), total.work_calls,
      static_cast<double>(total.work_ns) / 1e6,
      static_cast<double>(total.max_work_ns) / 1e6, total.eof, total.errors,
      total.again, static_cast<double>(total.wait_ns) / 1e6, total.reconnects,
      loops_, seeks_);
}

DecoderPerformance::DecoderPerformance(int stream_index) noexcept
    : stream_index_(stream_index) {}

void DecoderPerformance::Start(const void* instance,
                               const AVCodecContext& context) noexcept {
  instance_ = instance;
  time_base_ = context.pkt_timebase;
  video_ = context.codec_type == AVMEDIA_TYPE_VIDEO;
  module_ = video_ ? "perf.decoder.video" : "perf.decoder.audio";
  codec_ = context.codec ? context.codec->name : "?";
  backend_ = context.hw_device_ctx ? "cuda" : "cpu";
  enabled_ = mw::log::ShouldLog(module_, mw::log::LogLevel::kInfo);
  trace_enabled_ = mw::log::ShouldLog(module_, mw::log::LogLevel::kTrace);
  final_ = false;
  previous_pts_ = AV_NOPTS_VALUE;
  if (enabled_) window_.Reset(Clock::now());
}

DecoderPerformance::TimePoint DecoderPerformance::Begin() const noexcept {
  return enabled_ ? Clock::now() : TimePoint{};
}

void DecoderPerformance::PacketSent(TimePoint started, const AVPacket* packet,
                                    int result) noexcept {
  if (!enabled_) return;
  auto& counters = window_.counters();
  if (result == AVERROR(EAGAIN)) {
    ++counters.again;
  } else if (result >= 0 && packet) {
    ++counters.packets;
    counters.bytes += static_cast<std::uint64_t>(packet->size);
  }
  Finish(started, result < 0 && result != AVERROR(EAGAIN));
}

void DecoderPerformance::FrameReceived(TimePoint started, const AVFrame* frame,
                                       int result) noexcept {
  if (!enabled_) return;
  if (result == AVERROR(EAGAIN)) {
    ++window_.counters().again;
  } else if (result == AVERROR_EOF) {
    ++window_.counters().eof;
  } else if (result >= 0 && frame) {
    CountFrame(*frame);
  }
  Finish(started,
         result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF,
         result == AVERROR_EOF);
}

void DecoderPerformance::Drained(TimePoint started, int result) noexcept {
  if (!enabled_) return;
  if (result == AVERROR(EAGAIN)) ++window_.counters().again;
  Finish(started,
         result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF);
}

void DecoderPerformance::Flush() noexcept {
  if (!enabled_) return;
  previous_pts_ = AV_NOPTS_VALUE;
  final_ = false;
}

void DecoderPerformance::Stop() noexcept { Report(true); }

void DecoderPerformance::Finish(TimePoint started, bool error,
                                bool final) noexcept {
  if (!enabled_) return;
  const auto now = Clock::now();
  window_.AddWork(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - started)
          .count());
  if (error) ++window_.counters().errors;
  Report(final);
}

void DecoderPerformance::CountFrame(const AVFrame& frame) noexcept {
  auto& counters = window_.counters();
  ++counters.frames;
  std::int64_t media_ns = 0;
  if (!video_) {
    if (frame.nb_samples > 0)
      counters.samples += static_cast<std::uint64_t>(frame.nb_samples);
    if (frame.nb_samples > 0 && frame.sample_rate > 0)
      media_ns = av_rescale_q(frame.nb_samples, {1, frame.sample_rate},
                              {1, 1000000000});
  } else {
    const auto pts = frame.best_effort_timestamp != AV_NOPTS_VALUE
                         ? frame.best_effort_timestamp
                         : frame.pts;
    if (frame.duration > 0) {
      media_ns = av_rescale_q(frame.duration, time_base_, {1, 1000000000});
    } else if (pts != AV_NOPTS_VALUE && previous_pts_ != AV_NOPTS_VALUE &&
               pts > previous_pts_) {
      media_ns = av_rescale_q(pts - previous_pts_, time_base_, {1, 1000000000});
    }
    // Backward PTS establishes a new segment; Flush clears the baseline too.
    previous_pts_ = pts;
  }
  if (media_ns > 0) {
    counters.media_ns += media_ns;
    counters.has_media = true;
  }
}

void DecoderPerformance::Report(bool final) noexcept {
  if (!enabled_ || final_) return;
  PerformanceReport report;
  if (!window_.Sample(report, final, Clock::now())) return;
  const auto rate = [](std::uint64_t count, double seconds) noexcept {
    return seconds > 0 ? static_cast<double>(count) / seconds : 0.0;
  };
  const auto& selected = final ? report.total : report.interval;
  const auto seconds = final ? report.elapsed_seconds : report.interval_seconds;
  char selected_speed[32] = "N/A";
  if (selected.has_media && selected.media_ns > 0 && seconds > 0) {
    std::snprintf(
        selected_speed, sizeof(selected_speed), "%.3f",
        static_cast<double>(selected.media_ns) / 1000000000.0 / seconds);
  }
  const auto* backend = backend_;
  const auto api_ms_per_frame =
      selected.frames ? static_cast<double>(selected.work_ns) / 1000000.0 /
                            static_cast<double>(selected.frames)
                      : 0.0;
  if (video_) {
    MW_LOG_INFO(module_,
                "Decoder instance={} report={} codec={} backend={} fps={:.1f} "
                "speed={} api_ms_per_frame={:.2f} errors={}",
                instance_, final ? "summary" : "interval", codec_, backend,
                rate(selected.frames, seconds), selected_speed,
                api_ms_per_frame, selected.errors);
  } else {
    MW_LOG_INFO(
        module_,
        "Decoder instance={} report={} codec={} backend={} "
        "samples_per_second={:.1f} speed={} api_ms_per_frame={:.2f} errors={}",
        instance_, final ? "summary" : "interval", codec_, backend,
        rate(selected.samples, seconds), selected_speed, api_ms_per_frame,
        selected.errors);
  }
  if (final) final_ = true;
  if (!trace_enabled_) return;
  char speed[32] = "N/A";
  if (report.total.has_media && report.elapsed_seconds > 0) {
    std::snprintf(speed, sizeof(speed), "%.3f",
                  static_cast<double>(report.total.media_ns) / 1000000000.0 /
                      report.elapsed_seconds);
  }
  MW_LOG_TRACE(
      module_,
      "Decoder instance={} stream={} codec={} backend={} final={} packets={} "
      "bytes={} frames={} fps={:.3f} fps_now={:.3f} samples={} "
      "samples_per_second={:.3f} samples_per_second_now={:.3f} speed={} "
      "elapsed_s={:.3f} interval_s={:.3f} api_work_calls={} api_work_ms={:.3f} "
      "api_work_mean_ms={:.3f} "
      "api_work_ms_now={:.3f} api_work_max_ms={:.3f} "
      "api_work_max_ms_now={:.3f} eagain={} eof={} errors={} (API wall time, "
      "not GPU "
      "kernel "
      "time)",
      instance_, stream_index_, codec_, backend_, final, report.total.packets,
      report.total.bytes, report.total.frames,
      rate(report.total.frames, report.elapsed_seconds),
      rate(report.interval.frames, report.interval_seconds),
      report.total.samples, rate(report.total.samples, report.elapsed_seconds),
      rate(report.interval.samples, report.interval_seconds), speed,
      report.elapsed_seconds, report.interval_seconds, report.total.work_calls,
      static_cast<double>(report.total.work_ns) / 1000000.0,
      report.total.work_calls
          ? static_cast<double>(report.total.work_ns) / 1000000.0 /
                static_cast<double>(report.total.work_calls)
          : 0.0,
      static_cast<double>(report.interval.work_ns) / 1000000.0,
      static_cast<double>(report.total.max_work_ns) / 1000000.0,
      static_cast<double>(report.interval.max_work_ns) / 1000000.0,
      report.total.again, report.total.eof, report.total.errors);
}

void EncoderPerformance::Start(const void* instance, bool video,
                               bool audio) noexcept {
  instance_ = instance;
  video_ = {};
  audio_ = {};
  for (const bool is_video : {true, false}) {
    auto& track = is_video ? video_ : audio_;
    const auto* module = is_video ? "perf.encoder.video" : "perf.encoder.audio";
    track.enabled = (is_video ? video : audio) &&
                    mw::log::ShouldLog(module, mw::log::LogLevel::kInfo);
    track.trace_enabled =
        track.enabled && mw::log::ShouldLog(module, mw::log::LogLevel::kTrace);
    track.active = track.enabled;
    if (track.enabled) track.window.Reset();
  }
}

void EncoderPerformance::AcceptVideo(std::size_t queued_frames,
                                     bool skipped_picture) noexcept {
  if (!video_.active) return;
  ++video_.accepted;
  ++video_.pending;
  video_.queued_frames = queued_frames;
  if (skipped_picture) ++video_.skipped_picture;
}

void EncoderPerformance::AcceptAudio(int samples,
                                     std::size_t queued_frames) noexcept {
  if (!audio_.active) return;
  ++audio_.accepted;
  audio_.pending += static_cast<std::uint64_t>(samples);
  audio_.queued_frames = queued_frames;
}

void EncoderPerformance::VideoDiscarded(std::size_t queued_frames) noexcept {
  if (!video_.active) return;
  if (video_.pending) --video_.pending;
  ++video_.discarded_start;
  video_.queued_frames = queued_frames;
}

void EncoderPerformance::AudioDequeued(std::size_t queued_frames) noexcept {
  if (audio_.active) audio_.queued_frames = queued_frames;
}

void EncoderPerformance::AudioDiscarded(int samples) noexcept {
  if (!audio_.active) return;
  audio_.pending -=
      std::min(audio_.pending, static_cast<std::uint64_t>(samples));
  audio_.discarded_start += static_cast<std::uint64_t>(samples);
}

void EncoderPerformance::Abandon(bool video) noexcept {
  auto& track = video ? video_ : audio_;
  if (!track.active) return;
  track.discarded_start += track.pending;
  track.pending = 0;
  track.queued_frames = 0;
}

EncoderPerformance::TimePoint EncoderPerformance::Begin(
    bool video) const noexcept {
  return (video ? video_ : audio_).active ? PerformanceWindow::Clock::now()
                                          : TimePoint{};
}

void EncoderPerformance::Encoded(bool video, TimePoint started, int samples,
                                 bool repeated,
                                 std::size_t queued_frames) noexcept {
  auto& track = video ? video_ : audio_;
  if (!track.active) return;
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           PerformanceWindow::Clock::now() - started)
                           .count();
  track.window.AddWork(elapsed);
  ++track.window.counters().frames;
  track.window.counters().samples += static_cast<std::uint64_t>(samples);
  const auto consumed = static_cast<std::uint64_t>(video ? 1 : samples);
  track.pending -= std::min(track.pending, consumed);
  if (video) {
    track.queued_frames = queued_frames;
    if (repeated) ++track.repeated;
  }
  Report(video);
}

void EncoderPerformance::Packet(bool video, int bytes) noexcept {
  auto& track = video ? video_ : audio_;
  if (!track.active) return;
  ++track.window.counters().packets;
  if (bytes > 0)
    track.window.counters().bytes += static_cast<std::uint64_t>(bytes);
}

void EncoderPerformance::Error(bool video) noexcept {
  auto& track = video ? video_ : audio_;
  if (track.active) ++track.window.counters().errors;
}

void EncoderPerformance::Finish() noexcept {
  Report(true, true);
  Report(false, true);
}

void EncoderPerformance::Report(bool video, bool final) noexcept {
  auto& track = video ? video_ : audio_;
  if (!track.active) return;
  PerformanceReport report;
  if (!track.window.Sample(report, final)) return;
  const auto& overview = final ? report.total : report.interval;
  const auto seconds = final ? report.elapsed_seconds : report.interval_seconds;
  const auto accepted =
      final ? track.accepted : track.accepted - track.previous_accepted;
  track.previous_accepted = track.accepted;
  const auto* module = video ? "perf.encoder.video" : "perf.encoder.audio";
  const auto worker_ms = PerformanceReport::Rate(
      static_cast<double>(overview.work_ns) / 1e6, overview.work_calls);
  if (video) {
    MW_LOG_INFO(
        module,
        "Encoder[{}] report={} input_fps={:.1f} encoded_fps={:.1f} "
        "payload_Mb_per_second={:.2f} queue_frames={} pending_frames={} "
        "worker_mean_ms={:.2f} repeat_total={} skip_new_total={} errors={}",
        instance_, final ? "summary" : "interval",
        PerformanceReport::Rate(accepted, seconds),
        PerformanceReport::Rate(overview.frames, seconds),
        PerformanceReport::Rate(static_cast<double>(overview.bytes) * 8.0 / 1e6,
                                seconds),
        track.queued_frames, track.pending, worker_ms, track.repeated,
        track.skipped_picture, overview.errors);
  } else {
    MW_LOG_INFO(
        module,
        "Encoder[{}] report={} encoded_fps={:.1f} samples_per_second={:.0f} "
        "payload_Mb_per_second={:.2f} queue_frames={} pending_samples={} "
        "worker_mean_ms={:.2f} errors={}",
        instance_, final ? "summary" : "interval",
        PerformanceReport::Rate(overview.frames, seconds),
        PerformanceReport::Rate(overview.samples, seconds),
        PerformanceReport::Rate(static_cast<double>(overview.bytes) * 8.0 / 1e6,
                                seconds),
        track.queued_frames, track.pending, worker_ms, overview.errors);
  }
  if (track.trace_enabled) {
    const auto& total = report.total;
    MW_LOG_TRACE(
        module,
        "Encoder[{}] final={} elapsed_s={:.3f} accepted={} encoded={} "
        "samples={} packets={} payload_bytes={} repeated={} skipped_new={} "
        "discarded_start_{}={} worker_calls={} worker_total_ms={:.3f} "
        "worker_max_ms={:.3f} errors={} "
        "worker_scope=frame_preparation_codec_packet_callback",
        instance_, final, report.elapsed_seconds, track.accepted, total.frames,
        total.samples, total.packets, total.bytes, track.repeated,
        track.skipped_picture, video ? "frames" : "samples",
        track.discarded_start, total.work_calls,
        static_cast<double>(total.work_ns) / 1e6,
        static_cast<double>(total.max_work_ns) / 1e6, total.errors);
  }
  if (final) track.active = false;
}

void SchedulerPerformance::Start(const void* instance,
                                 AVRational video_frame_rate,
                                 int audio_sample_rate, int audio_block_samples,
                                 bool video, bool audio) noexcept {
  enabled_ = mw::log::ShouldLog("perf.scheduler", mw::log::LogLevel::kInfo);
  instance_ = instance;
  video_frame_rate_ = video_frame_rate;
  audio_sample_rate_ = audio_sample_rate;
  audio_block_samples_ = audio_block_samples;
  video_ = video;
  audio_ = audio;
  video_performance_ = {};
  audio_performance_ = {};
  source_samples_.clear();
  copied_samples_ = 0;
  if (enabled_) {
    const auto now = Clock::now();
    video_performance_.window.Reset(now);
    audio_performance_.window.Reset(now);
  }
}

SchedulerPerformance::Clock::time_point SchedulerPerformance::BeginWork(
    bool active) const noexcept {
  return enabled_ && active ? Clock::now() : Clock::time_point{};
}

std::int64_t SchedulerPerformance::WorkTime(
    Clock::time_point started) const noexcept {
  if (!enabled_ || started == Clock::time_point{}) return 0;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
      .count();
}

void SchedulerPerformance::Accept(bool video) noexcept {
  if (enabled_)
    ++(video ? video_performance_ : audio_performance_)
          .window.counters()
          .packets;
}

void SchedulerPerformance::Error(bool video) noexcept {
  if (enabled_)
    ++(video ? video_performance_ : audio_performance_)
          .window.counters()
          .errors;
}

void SchedulerPerformance::RejectVideo(std::size_t queued_frames) noexcept {
  if (!enabled_) return;
  video_performance_.capacity_cleared += queued_frames;
  ++video_performance_.capacity_rejected;
}

void SchedulerPerformance::DropVideo() noexcept {
  if (enabled_) ++video_performance_.late_old;
}

void SchedulerPerformance::SelectVideo() noexcept {
  if (enabled_) ++video_performance_.selected_new;
}

void SchedulerPerformance::SkipVideoTicks(std::uint64_t count) noexcept {
  if (enabled_) video_performance_.skipped_ticks += count;
}

void SchedulerPerformance::RejectAudio(int samples) noexcept {
  if (!enabled_) return;
  ++audio_performance_.capacity_rejected;
  audio_performance_.capacity_rejected_samples +=
      static_cast<std::uint64_t>(samples);
}

void SchedulerPerformance::DropAudio(std::int64_t samples) noexcept {
  if (enabled_)
    audio_performance_.dropped_source_samples +=
        static_cast<std::uint64_t>(samples);
}

void SchedulerPerformance::ConsumeAudio(std::int64_t samples,
                                        bool discarded) noexcept {
  if (!enabled_) return;
  if (discarded) {
    audio_performance_.dropped_source_samples +=
        static_cast<std::uint64_t>(samples);
    audio_performance_.late_source_samples +=
        static_cast<std::uint64_t>(samples);
  } else {
    audio_performance_.consumed_source_samples +=
        static_cast<std::uint64_t>(samples);
  }
}

void SchedulerPerformance::AudioUnderload() noexcept {
  if (enabled_) ++audio_performance_.underload_ticks;
}

void SchedulerPerformance::BeginAudioBatch() noexcept {
  if (enabled_) source_samples_.clear();
}

void SchedulerPerformance::BeginAudioFrame() noexcept {
  if (enabled_) copied_samples_ = 0;
}

void SchedulerPerformance::CopyAudio(std::int64_t samples) noexcept {
  if (enabled_) copied_samples_ += samples;
}

void SchedulerPerformance::PrepareAudioFrame() {
  if (enabled_) source_samples_.push_back(copied_samples_);
}

void SchedulerPerformance::RecordDelivery(bool video, int samples,
                                          bool callback,
                                          std::int64_t elapsed) noexcept {
  if (!enabled_) return;
  auto& performance = video ? video_performance_ : audio_performance_;
  auto& counters = performance.window.counters();
  ++counters.frames;
  counters.samples += static_cast<std::uint64_t>(samples);
  if (callback) {
    ++performance.external_callback_calls;
    performance.external_callback_ns += elapsed;
    performance.max_external_callback_ns =
        std::max(performance.max_external_callback_ns, elapsed);
  }
}

void SchedulerPerformance::DeliverVideo(bool repeated, bool callback,
                                        std::int64_t elapsed) noexcept {
  if (!enabled_) return;
  RecordDelivery(true, 0, callback, elapsed);
  if (repeated) ++video_performance_.repeated;
}

void SchedulerPerformance::DeliverAudio(std::size_t index, int samples,
                                        bool callback,
                                        std::int64_t elapsed) noexcept {
  if (!enabled_) return;
  RecordDelivery(false, samples, callback, elapsed);
  const auto copied = source_samples_[index];
  audio_performance_.delivered_source_samples +=
      static_cast<std::uint64_t>(copied);
  if (copied) ++audio_performance_.selected_new;
  audio_performance_.zero_fill_samples +=
      static_cast<std::uint64_t>(samples - copied);
}

void SchedulerPerformance::Tick(bool video, Clock::time_point started,
                                Clock::time_point deadline,
                                std::int64_t interval_ns, std::int64_t elapsed,
                                std::size_t video_queue,
                                std::int64_t audio_queue) noexcept {
  if (!enabled_) return;
  auto& performance = video ? video_performance_ : audio_performance_;
  performance.window.AddWork(elapsed);
  if (interval_ns > 0 &&
      std::chrono::duration_cast<std::chrono::nanoseconds>(started - deadline)
              .count() >= interval_ns) {
    ++performance.late_ticks;
  }
  Log(video, video_queue, audio_queue);
}

void SchedulerPerformance::Finish(std::size_t video_queue,
                                  std::int64_t audio_queue) noexcept {
  Log(true, video_queue, audio_queue, true);
  Log(false, video_queue, audio_queue, true);
}

void SchedulerPerformance::Log(bool video, std::size_t video_queue,
                               std::int64_t audio_queue, bool final) noexcept {
  if (!enabled_ || (video ? !video_ : !audio_)) {
    return;
  }
  auto& performance = video ? video_performance_ : audio_performance_;
  PerformanceReport report;
  if (!performance.window.Sample(report, final, Clock::now())) return;
  const auto& total = report.total;
  const auto current_fps = PerformanceReport::Rate(
      static_cast<double>(report.interval.frames), report.interval_seconds);
  const auto source_fps = PerformanceReport::Rate(
      static_cast<double>(report.interval.packets), report.interval_seconds);
  const auto callback_mean_ms =
      performance.external_callback_calls
          ? static_cast<double>(performance.external_callback_ns) / 1e6 /
                static_cast<double>(performance.external_callback_calls)
          : 0;
  const auto tick_mean_ms = total.work_calls
                                ? static_cast<double>(total.work_ns) / 1e6 /
                                      static_cast<double>(total.work_calls)
                                : 0;
  const auto& overview = final ? total : report.interval;
  const auto overview_seconds =
      final ? report.elapsed_seconds : report.interval_seconds;
  if (video) {
    MW_LOG_INFO(
        "perf.scheduler",
        "Scheduler[{}] track=video report={} fps={:.1f}/{:.1f} "
        "queue_frames={} repeated_total={} dropped_total={} "
        "callback_mean_ms={:.3f}",
        instance_, final ? "summary" : "interval",
        PerformanceReport::Rate(static_cast<double>(overview.frames),
                                overview_seconds),
        static_cast<double>(video_frame_rate_.num) / video_frame_rate_.den,
        video_queue, performance.repeated,
        performance.late_old + performance.capacity_cleared +
            performance.capacity_rejected,
        callback_mean_ms);
    MW_LOG_TRACE(
        "perf.scheduler",
        "Scheduler[{}] track=video final={} counts=since_start "
        "elapsed_s={:.3f} window_s={:.3f} "
        "target_fps={:.3f} source_fps={:.3f} current_fps={:.3f} "
        "accepted_new={} deliver_frames={} selected_new={} repeated={} "
        "dropped_late_old={} dropped_capacity_clear={} "
        "capacity_rejected_new={} "
        "late_ticks={} skipped_ticks={} late_tick_threshold_ms={:.3f} "
        "queue_frames={} "
        "external_callback_calls={} external_callback_mean_ms={:.3f} "
        "external_callback_max_ms={:.3f} "
        "tick_calls={} tick_include_callback_mean_ms={:.3f} "
        "tick_include_callback_max_ms={:.3f} errors={}",
        instance_, final, report.elapsed_seconds, report.interval_seconds,
        static_cast<double>(video_frame_rate_.num) / video_frame_rate_.den,
        source_fps, current_fps, total.packets, total.frames,
        performance.selected_new, performance.repeated, performance.late_old,
        performance.capacity_cleared, performance.capacity_rejected,
        performance.late_ticks, performance.skipped_ticks,
        static_cast<double>(av_rescale_q_rnd(1, av_inv_q(video_frame_rate_),
                                             AVRational{1, 1000000000},
                                             AV_ROUND_DOWN)) /
            1e6,
        video_queue, performance.external_callback_calls, callback_mean_ms,
        static_cast<double>(performance.max_external_callback_ns) / 1e6,
        total.work_calls, tick_mean_ms,
        static_cast<double>(total.max_work_ns) / 1e6, total.errors);
  } else {
    MW_LOG_INFO(
        "perf.scheduler",
        "Scheduler[{}] track=audio report={} samples_per_second={:.0f}/{} "
        "queue_ms={:.1f} silence_ms_total={:.1f} dropped_ms_total={:.1f} "
        "callback_mean_ms={:.3f}",
        instance_, final ? "summary" : "interval",
        PerformanceReport::Rate(static_cast<double>(overview.samples),
                                overview_seconds),
        audio_sample_rate_,
        static_cast<double>(audio_queue) * 1000 / audio_sample_rate_,
        static_cast<double>(performance.zero_fill_samples) * 1000 /
            audio_sample_rate_,
        static_cast<double>(performance.dropped_source_samples) * 1000 /
            audio_sample_rate_,
        callback_mean_ms);
    MW_LOG_TRACE(
        "perf.scheduler",
        "Scheduler[{}] track=audio final={} counts=since_start "
        "elapsed_s={:.3f} window_s={:.3f} "
        "target_tick_fps={:.3f} sample_rate={} block_samples={} "
        "source_fps={:.3f} current_fps={:.3f} "
        "accepted_new={} deliver_frames={} selected_new={} "
        "selected_unit=source_payload_frames repeated=0 "
        "delivered_samples={} consumed_source_samples={} "
        "delivered_source_samples={} "
        "zero_fill_samples={} underload_ticks={} "
        "dropped_queued_source_samples={} "
        "dropped_late_source_samples={} capacity_rejected_new={} "
        "capacity_rejected_samples={} "
        "late_ticks={} late_tick_threshold_ms={:.3f} queue_samples={} "
        "queue_ms={:.3f} "
        "external_callback_calls={} external_callback_mean_ms={:.3f} "
        "external_callback_max_ms={:.3f} "
        "tick_calls={} tick_include_callback_mean_ms={:.3f} "
        "tick_include_callback_max_ms={:.3f} errors={}",
        instance_, final, report.elapsed_seconds, report.interval_seconds,
        static_cast<double>(audio_sample_rate_) / audio_block_samples_,
        audio_sample_rate_, audio_block_samples_, source_fps, current_fps,
        total.packets, total.frames, performance.selected_new, total.samples,
        performance.consumed_source_samples,
        performance.delivered_source_samples, performance.zero_fill_samples,
        performance.underload_ticks, performance.dropped_source_samples,
        performance.late_source_samples, performance.capacity_rejected,
        performance.capacity_rejected_samples, performance.late_ticks,
        static_cast<double>(av_rescale_q_rnd(
            audio_block_samples_, AVRational{1, audio_sample_rate_},
            AVRational{1, 1000000000}, AV_ROUND_DOWN)) /
            1e6,
        audio_queue,
        static_cast<double>(audio_queue) * 1000 / audio_sample_rate_,
        performance.external_callback_calls, callback_mean_ms,
        static_cast<double>(performance.max_external_callback_ns) / 1e6,
        total.work_calls, tick_mean_ms,
        static_cast<double>(total.max_work_ns) / 1e6, total.errors);
  }
}

}  // namespace mw::streamer::internal
