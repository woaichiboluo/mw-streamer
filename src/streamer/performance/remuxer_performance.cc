#include "mw/streamer/performance/performance.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

void RemuxPerformance::Start(
    const void* instance,
    const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
  instance_ = instance;
  window_ = {};
  counts_ = previous_ = {};
  timing_ = {};
  previous_stage_ns_ = {};
  peak_pending_packets_ = peak_pending_bytes_ = 0;
  video_index_ = audio_index_ = -1;
  enabled_ = mw::log::ShouldLog("perf.remux", mw::log::LogLevel::kInfo);
  trace_enabled_ =
      enabled_ && mw::log::ShouldLog("perf.remux", mw::log::LogLevel::kTrace);
  active_ = enabled_;
  if (!enabled_) return;
  for (const auto& stream : streams) {
    const auto* parameters = stream.codec_parameters.get();
    if (!parameters) continue;
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO)
      video_index_ = stream.stream_index;
    else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO)
      audio_index_ = stream.stream_index;
  }
  window_.Reset();
}

RemuxPerformance::TimePoint RemuxPerformance::Begin() const noexcept {
  return active_ ? Clock::now() : TimePoint{};
}

std::uint64_t RemuxPerformance::PendingPackets() const noexcept {
  auto remaining = counts_.accepted_packets;
  remaining -= std::min(remaining, window_.counters().packets);
  remaining -= std::min(remaining, counts_.aborted_packets);
  return remaining;
}

std::uint64_t RemuxPerformance::PendingBytes() const noexcept {
  auto remaining = counts_.accepted_bytes;
  remaining -= std::min(remaining, window_.counters().bytes);
  remaining -= std::min(remaining, counts_.aborted_bytes);
  return remaining;
}

void RemuxPerformance::Accepted(const AVPacket& packet) noexcept {
  if (!active_) return;
  ++counts_.accepted_packets;
  counts_.accepted_bytes +=
      static_cast<std::uint64_t>(std::max(packet.size, 0));
  peak_pending_packets_ = std::max(peak_pending_packets_, PendingPackets());
  peak_pending_bytes_ = std::max(peak_pending_bytes_, PendingBytes());
}

void RemuxPerformance::Rejected() noexcept {
  if (active_) ++counts_.rejected_packets;
}

void RemuxPerformance::Error() noexcept {
  if (active_) ++window_.counters().errors;
}

void RemuxPerformance::Interleaved(TimePoint started,
                                   TimePoint finished) noexcept {
  if (!active_) return;
  ++counts_.interleave_calls;
  timing_[kInterleave].Add(Elapsed(started, finished));
}

std::int64_t RemuxPerformance::Elapsed(TimePoint begin,
                                       TimePoint end) noexcept {
  return std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
             .count());
}

void RemuxPerformance::Timing::Add(std::int64_t elapsed_ns) noexcept {
  total_ns += elapsed_ns;
  max_ns = std::max(max_ns, elapsed_ns);
  interval_max_ns = std::max(interval_max_ns, elapsed_ns);
}

void RemuxPerformance::HandedOff(const AVPacket& packet, std::uint64_t pts_ms,
                                 std::uint64_t dts_ms, TimePoint queued_at,
                                 TimePoint convert_started,
                                 TimePoint mux_started,
                                 TimePoint finished) noexcept {
  if (!active_) return;
  auto& counters = window_.counters();
  ++counters.packets;
  counters.bytes += static_cast<std::uint64_t>(std::max(packet.size, 0));
  if (packet.stream_index == video_index_) ++counts_.video_packets;
  if (packet.stream_index == audio_index_) ++counts_.audio_packets;
  const auto wait = Elapsed(queued_at, convert_started);
  const auto convert = Elapsed(convert_started, mux_started);
  const auto mux = Elapsed(mux_started, finished);
  const auto work = Elapsed(convert_started, finished);
  timing_[kQueueWait].Add(wait);
  timing_[kConvert].Add(convert);
  timing_[kMux].Add(mux);
  window_.AddWork(work);
  if (!trace_enabled_) return;
  MW_LOG_TRACE(
      "perf.remux",
      "Remuxer[{}] event=handoff stream_index={} raw_pts={} raw_dts={} "
      "time_base={}/{} zlm_pts_ms={} "
      "zlm_dts_ms={} payload_bytes={} queue_wait_ms={:.3f} "
      "convert_ms={:.3f} mux_ms={:.3f} work_ms={:.3f}",
      instance_, packet.stream_index, packet.pts, packet.dts,
      packet.time_base.num, packet.time_base.den, pts_ms, dts_ms, packet.size,
      static_cast<double>(wait) / 1e6, static_cast<double>(convert) / 1e6,
      static_cast<double>(mux) / 1e6, static_cast<double>(work) / 1e6);
}

void RemuxPerformance::Report(std::size_t queue_packets,
                              std::size_t queue_bytes, bool initialized,
                              bool draining, bool final) noexcept {
  if (!active_) return;
  if (final) {
    counts_.aborted_packets += PendingPackets();
    counts_.aborted_bytes += PendingBytes();
    queue_packets = queue_bytes = 0;
  }
  PerformanceReport report;
  if (!window_.Sample(report, final, Clock::now())) return;
  const auto accepted = counts_.accepted_packets - previous_.accepted_packets;
  const auto accepted_bytes = counts_.accepted_bytes - previous_.accepted_bytes;
  const auto videos = counts_.video_packets - previous_.video_packets;
  const auto audios = counts_.audio_packets - previous_.audio_packets;
  const auto rejected = counts_.rejected_packets - previous_.rejected_packets;
  const auto aborted = counts_.aborted_packets - previous_.aborted_packets;
  const auto aborted_bytes = counts_.aborted_bytes - previous_.aborted_bytes;
  const auto interleave_calls =
      counts_.interleave_calls - previous_.interleave_calls;
  const auto pending = PendingPackets();
  const auto pending_bytes = PendingBytes();
  const auto in_flight =
      pending - std::min<std::uint64_t>(pending, queue_packets);
  const auto in_flight_bytes =
      pending_bytes - std::min<std::uint64_t>(pending_bytes, queue_bytes);
  const auto& overview = final ? report.total : report.interval;
  const auto seconds = final ? report.elapsed_seconds : report.interval_seconds;
  const auto* state = final         ? "stopped"
                      : draining    ? "draining"
                      : initialized ? "running"
                                    : "starting";
  std::array<std::int64_t, kStages> interval_stage_ns{};
  for (std::size_t stage = 0; stage < timing_.size(); ++stage)
    interval_stage_ns[stage] =
        timing_[stage].total_ns - previous_stage_ns_[stage];
  const auto mean_ms = [](std::int64_t ns, std::uint64_t calls) noexcept {
    return PerformanceReport::Rate(static_cast<double>(ns) / 1e6, calls);
  };
  MW_LOG_INFO(
      "perf.remux",
      "Remuxer[{}] report={} state={} accepted_pps={:.1f} video_pps={:.1f} "
      "audio_pps={:.1f} payload_Mb_per_second={:.3f} pending_packets={} "
      "pending_MiB={:.3f} queue_wait_mean_ms={:.3f} interleave_mean_ms={:.3f} "
      "work_mean_ms={:.3f} "
      "errors={}",
      instance_, final ? "summary" : "interval", state,
      PerformanceReport::Rate(final ? counts_.accepted_packets : accepted,
                              seconds),
      PerformanceReport::Rate(final ? counts_.video_packets : videos, seconds),
      PerformanceReport::Rate(final ? counts_.audio_packets : audios, seconds),
      PerformanceReport::Rate(static_cast<double>(overview.bytes) * 8.0 / 1e6,
                              seconds),
      pending, static_cast<double>(pending_bytes) / 1048576.0,
      mean_ms(
          final ? timing_[kQueueWait].total_ns : interval_stage_ns[kQueueWait],
          overview.packets),
      mean_ms(final ? timing_[kInterleave].total_ns
                    : interval_stage_ns[kInterleave],
              final ? counts_.interleave_calls : interleave_calls),
      mean_ms(overview.work_ns, overview.work_calls), overview.errors);
  if (trace_enabled_) {
    MW_LOG_TRACE(
        "perf.remux",
        "Remuxer[{}] report={} state={} counts=since_start elapsed_s={:.3f} "
        "window_s={:.3f} accepted_packets={} accepted_bytes={} "
        "handed_off_packets={} payload_bytes={} video_packets={} "
        "audio_packets={} "
        "aborted_packets={} aborted_bytes={} rejected_packets={} errors={} "
        "pending_packets={} pending_bytes={} queue_packets={} queue_bytes={} "
        "in_flight_packets={} in_flight_bytes={} peak_pending_packets={} "
        "peak_pending_bytes={} interleave_scope=push_pop_not_mutex_wait "
        "interleave_calls={} interleave_total_ms={:.3f} "
        "interleave_mean_ms={:.3f} interleave_max_ms={:.3f} "
        "queue_wait_total_ms={:.3f} "
        "queue_wait_mean_ms={:.3f} queue_wait_max_ms={:.3f} "
        "convert_total_ms={:.3f} convert_mean_ms={:.3f} convert_max_ms={:.3f} "
        "mux_total_ms={:.3f} mux_mean_ms={:.3f} mux_max_ms={:.3f} "
        "work_total_ms={:.3f} work_mean_ms={:.3f} work_max_ms={:.3f} "
        "interval_accepted_packets={} interval_accepted_bytes={} "
        "interval_handed_off_packets={} interval_payload_bytes={} "
        "interval_video_packets={} interval_audio_packets={} "
        "interval_aborted_packets={} interval_aborted_bytes={} "
        "interval_rejected_packets={} interval_errors={} "
        "interval_interleave_calls={} interval_interleave_total_ms={:.3f} "
        "interval_interleave_mean_ms={:.3f} interval_interleave_max_ms={:.3f} "
        "interval_queue_wait_total_ms={:.3f} "
        "interval_queue_wait_mean_ms={:.3f} "
        "interval_queue_wait_max_ms={:.3f} interval_convert_total_ms={:.3f} "
        "interval_convert_mean_ms={:.3f} interval_convert_max_ms={:.3f} "
        "interval_mux_total_ms={:.3f} interval_mux_mean_ms={:.3f} "
        "interval_mux_max_ms={:.3f} interval_work_total_ms={:.3f} "
        "interval_work_mean_ms={:.3f} interval_work_max_ms={:.3f}",
        instance_, final ? "summary" : "interval", state,
        report.elapsed_seconds, report.interval_seconds,
        counts_.accepted_packets, counts_.accepted_bytes, report.total.packets,
        report.total.bytes, counts_.video_packets, counts_.audio_packets,
        counts_.aborted_packets, counts_.aborted_bytes,
        counts_.rejected_packets, report.total.errors, pending, pending_bytes,
        queue_packets, queue_bytes, in_flight, in_flight_bytes,
        peak_pending_packets_, peak_pending_bytes_, counts_.interleave_calls,
        static_cast<double>(timing_[kInterleave].total_ns) / 1e6,
        mean_ms(timing_[kInterleave].total_ns, counts_.interleave_calls),
        static_cast<double>(timing_[kInterleave].max_ns) / 1e6,
        static_cast<double>(timing_[kQueueWait].total_ns) / 1e6,
        mean_ms(timing_[kQueueWait].total_ns, report.total.packets),
        static_cast<double>(timing_[kQueueWait].max_ns) / 1e6,
        static_cast<double>(timing_[kConvert].total_ns) / 1e6,
        mean_ms(timing_[kConvert].total_ns, report.total.packets),
        static_cast<double>(timing_[kConvert].max_ns) / 1e6,
        static_cast<double>(timing_[kMux].total_ns) / 1e6,
        mean_ms(timing_[kMux].total_ns, report.total.packets),
        static_cast<double>(timing_[kMux].max_ns) / 1e6,
        static_cast<double>(report.total.work_ns) / 1e6,
        mean_ms(report.total.work_ns, report.total.work_calls),
        static_cast<double>(report.total.max_work_ns) / 1e6, accepted,
        accepted_bytes, report.interval.packets, report.interval.bytes, videos,
        audios, aborted, aborted_bytes, rejected, report.interval.errors,
        interleave_calls,
        static_cast<double>(interval_stage_ns[kInterleave]) / 1e6,
        mean_ms(interval_stage_ns[kInterleave], interleave_calls),
        static_cast<double>(timing_[kInterleave].interval_max_ns) / 1e6,
        static_cast<double>(interval_stage_ns[kQueueWait]) / 1e6,
        mean_ms(interval_stage_ns[kQueueWait], report.interval.packets),
        static_cast<double>(timing_[kQueueWait].interval_max_ns) / 1e6,
        static_cast<double>(interval_stage_ns[kConvert]) / 1e6,
        mean_ms(interval_stage_ns[kConvert], report.interval.packets),
        static_cast<double>(timing_[kConvert].interval_max_ns) / 1e6,
        static_cast<double>(interval_stage_ns[kMux]) / 1e6,
        mean_ms(interval_stage_ns[kMux], report.interval.packets),
        static_cast<double>(timing_[kMux].interval_max_ns) / 1e6,
        static_cast<double>(report.interval.work_ns) / 1e6,
        mean_ms(report.interval.work_ns, report.interval.work_calls),
        static_cast<double>(report.interval.max_work_ns) / 1e6);
  }
  previous_ = counts_;
  for (std::size_t stage = 0; stage < timing_.size(); ++stage) {
    previous_stage_ns_[stage] = timing_[stage].total_ns;
    timing_[stage].interval_max_ns = 0;
  }
  if (final) active_ = false;
}

}  // namespace mw::streamer::internal
