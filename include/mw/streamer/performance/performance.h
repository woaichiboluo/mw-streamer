#ifndef MW_STREAMER_PERFORMANCE_PERFORMANCE_H_
#define MW_STREAMER_PERFORMANCE_PERFORMANCE_H_

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace mw::streamer::internal {

// Internal counters. The owner serializes updates; there is no polling thread.
struct PerformanceCounters {
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint64_t frames = 0;
  std::uint64_t samples = 0;
  std::uint64_t errors = 0;
  std::uint64_t eof = 0;
  std::uint64_t again = 0;
  std::uint64_t reconnects = 0;
  std::uint64_t work_calls = 0;
  std::int64_t work_ns = 0;
  std::int64_t max_work_ns = 0;
  std::int64_t wait_ns = 0;
  std::int64_t media_ns = 0;
  bool has_media = false;

  bool operator==(const PerformanceCounters& other) const noexcept {
    return packets == other.packets && bytes == other.bytes &&
           frames == other.frames && samples == other.samples &&
           errors == other.errors && eof == other.eof && again == other.again &&
           reconnects == other.reconnects && work_calls == other.work_calls &&
           work_ns == other.work_ns && max_work_ns == other.max_work_ns &&
           wait_ns == other.wait_ns && media_ns == other.media_ns &&
           has_media == other.has_media;
  }
};

struct PerformanceReport {
  PerformanceCounters total;
  PerformanceCounters interval;
  double elapsed_seconds = 0;
  double interval_seconds = 0;

  static double Rate(double value, double seconds) noexcept {
    return seconds > 0 ? value / seconds : 0;
  }
};

class PerformanceWindow final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  // Default construction does not read the clock. Disabled log modules can
  // skip all sampling, including Reset, AddWork and Sample.
  PerformanceWindow() noexcept = default;

  void Reset(TimePoint now = Clock::now()) noexcept {
    total_ = {};
    previous_ = {};
    started_ = previous_time_ = now;
    window_max_work_ns_ = 0;
    active_ = true;
    final_reported_ = false;
  }

  PerformanceCounters& counters() noexcept { return total_; }
  const PerformanceCounters& counters() const noexcept { return total_; }

  void AddWork(std::int64_t nanoseconds) noexcept {
    nanoseconds = std::max<std::int64_t>(0, nanoseconds);
    ++total_.work_calls;
    total_.work_ns += nanoseconds;
    total_.max_work_ns = std::max(total_.max_work_ns, nanoseconds);
    window_max_work_ns_ = std::max(window_max_work_ns_, nanoseconds);
  }

  bool Sample(PerformanceReport& report, bool final = false,
              TimePoint now = Clock::now()) noexcept {
    if (!active_) return false;
    if (!final && now - previous_time_ < std::chrono::seconds(2)) return false;
    if (final && final_reported_ && total_ == previous_) return false;
    report.total = total_;
    report.interval = {total_.packets - previous_.packets,
                       total_.bytes - previous_.bytes,
                       total_.frames - previous_.frames,
                       total_.samples - previous_.samples,
                       total_.errors - previous_.errors,
                       total_.eof - previous_.eof,
                       total_.again - previous_.again,
                       total_.reconnects - previous_.reconnects,
                       total_.work_calls - previous_.work_calls,
                       total_.work_ns - previous_.work_ns,
                       window_max_work_ns_,
                       total_.wait_ns - previous_.wait_ns,
                       total_.media_ns - previous_.media_ns,
                       total_.has_media};
    report.elapsed_seconds =
        std::max(0.0, std::chrono::duration<double>(now - started_).count());
    report.interval_seconds = std::max(
        0.0, std::chrono::duration<double>(now - previous_time_).count());
    previous_ = total_;
    previous_time_ = now;
    window_max_work_ns_ = 0;
    final_reported_ = final;
    return true;
  }

 private:
  PerformanceCounters total_;
  PerformanceCounters previous_;
  TimePoint started_{};
  TimePoint previous_time_{};
  std::int64_t window_max_work_ns_ = 0;
  bool active_ = false;
  bool final_reported_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PERFORMANCE_PERFORMANCE_H_
