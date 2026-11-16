#include "streamer/performance/live_performance.h"

#include <cmath>
#include <string>
#include <utility>

#include "log/performance.h"
#include "streamer/performance/settings.h"

namespace mw::streamer::internal {
namespace {

std::string FrameCount(std::uint64_t actual, double target_fps,
                       double seconds) {
  if (target_fps <= 0.) return fmt::format("{}", actual);
  return fmt::format("{}/{:.0f}", actual, target_fps * seconds);
}

std::string Headroom(double budget_ms, LivePerformance::Clock::duration elapsed,
                     std::uint64_t calls) {
  if (!calls || budget_ms <= 0.) return "n/a";
  const double busy_ms =
      std::chrono::duration<double, std::milli>(elapsed).count();
  return fmt::format("{:+.2f} ms", budget_ms - busy_ms / calls);
}

}  // namespace

void LivePerformance::Start(const void* instance, double target_fps) noexcept {
  // Called before any pipeline workers can sample this session.
  pending_ = {};
  recent_ = {};
  previous_ = Clock::now();
  instance_ = instance;
  target_fps_ = std::isfinite(target_fps) && target_fps > 0. ? target_fps : 0.;
  resyncs_ = video_blocks_ = video_read_ = video_to_encoder_ = 0;
}

void LivePerformance::ReadVideo() noexcept { ++video_read_; }

void LivePerformance::FilteredVideo(Clock::duration elapsed, bool produced) {
  std::lock_guard lock(mutex_);
  ++pending_.filter_in;
  if (produced) ++pending_.filter_out;
  pending_.filter_time += elapsed;
}

void LivePerformance::EncodedVideo() {
  std::lock_guard lock(mutex_);
  ++pending_.encoded;
}

void LivePerformance::VideoToEncoder() noexcept { ++video_to_encoder_; }

void LivePerformance::Resynced() noexcept { ++resyncs_; }

void LivePerformance::BlockedVideo() noexcept { ++video_blocks_; }

LivePerformance::Clock::time_point LivePerformance::Report(
    Clock::time_point now) {
  if (now < previous_ + kInterval) return previous_ + kInterval;
  Counters sample;
  {
    std::lock_guard lock(mutex_);
    now = Clock::now();
    sample = std::exchange(pending_, Counters{});
  }
  const double seconds = std::chrono::duration<double>(now - previous_).count();
  previous_ = now;
  const auto recent = recent_.Add(
      {seconds, sample.filter_in, sample.filter_out, sample.filter_time});
  const double budget_ms = target_fps_ > 0. ? 1000. / target_fps_ : 0.;
  const auto budget =
      budget_ms > 0. ? fmt::format("{:.2f} ms", budget_ms) : "n/a";
  MW_PERF_LOG_INFO(
      kPerformanceLogModule,
      "output: {0} frames ({1:.2f}s), {2} frames ({3:.2f}s) | "
      "process: headroom={4} ({1:.2f}s), {5} ({3:.2f}s), budget={6} | "
      "input: read={7}, processed={8} | "
      "encoder: submitted={9}, packets={10}, blocks={11} | "
      "syncer: resyncs={12} | pipeline={13}",
      FrameCount(sample.filter_out, target_fps_, seconds), seconds,
      FrameCount(recent.output, target_fps_, recent.seconds), recent.seconds,
      Headroom(budget_ms, sample.filter_time, sample.filter_in),
      Headroom(budget_ms, recent.work, recent.input), budget,
      std::exchange(video_read_, 0U), sample.filter_in,
      std::exchange(video_to_encoder_, 0U), sample.encoded,
      std::exchange(video_blocks_, 0U), std::exchange(resyncs_, 0U), instance_);
  return previous_ + kInterval;
}

}  // namespace mw::streamer::internal
