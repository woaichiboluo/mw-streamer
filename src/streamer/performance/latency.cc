#include "mw/streamer/performance/latency.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>

#include "mw/log.h"

namespace mw::streamer::internal {
namespace {

char kProbeTag = 0;
std::atomic<std::uint64_t> next_token{0};

struct Probe {
  std::uint64_t token;
  StageLatency::TimePoint started;
  AVBufferRef* original = nullptr;

  ~Probe() { av_buffer_unref(&original); }
};

void FreeProbe(void*, std::uint8_t* data) noexcept {
  delete reinterpret_cast<Probe*>(data);
}

std::int64_t Elapsed(StageLatency::TimePoint begin,
                     StageLatency::TimePoint end) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
      .count();
}

}  // namespace

StageLatency::~StageLatency() { Finish(); }

void StageLatency::Start(const char* module, const void* instance,
                         const char* stage, int stream_index) noexcept {
  Finish();
  module_ = module;
  stage_ = stage;
  instance_ = instance;
  stream_index_ = stream_index;
  total_ = interval_ = {};
  active_ = mw::log::ShouldLog(module_, mw::log::LogLevel::kInfo);
  trace_ = active_ && mw::log::ShouldLog(module_, mw::log::LogLevel::kTrace);
  if (!active_) return;
  token_ = next_token.fetch_add(1, std::memory_order_relaxed) + 1;
  started_ = reported_ = Clock::now();
}

StageLatency::TimePoint StageLatency::Begin() const noexcept {
  return active_ ? Clock::now() : TimePoint{};
}

void StageLatency::End(TimePoint started) noexcept {
  if (active_) Observe(Elapsed(started, Clock::now()));
}

void StageLatency::Attach(AVBufferRef*& opaque, TimePoint started) {
  if (!active_) return;
  auto probe = std::make_unique<Probe>();
  probe->token = token_;
  probe->started = started;
  auto* buffer = av_buffer_create(reinterpret_cast<std::uint8_t*>(probe.get()),
                                  sizeof(Probe), FreeProbe, &kProbeTag, 0);
  if (!buffer) throw std::bad_alloc();
  probe->original = opaque;
  probe.release();
  opaque = buffer;
}

void StageLatency::Complete(AVBufferRef*& opaque) {
  if (active_) Complete(opaque, Clock::now());
}

void StageLatency::Complete(AVBufferRef*& opaque, TimePoint finished) {
  if (!active_) return;
  if (!opaque || opaque->size != sizeof(Probe) ||
      av_buffer_get_opaque(opaque) != &kProbeTag) {
    Observe(std::nullopt);
    return;
  }
  const auto* probe = reinterpret_cast<const Probe*>(opaque->data);
  if (probe->token != token_) {
    Observe(std::nullopt);
    return;
  }
  const auto elapsed = Elapsed(probe->started, finished);
  AVBufferRef* original = nullptr;
  if (probe->original) {
    original = av_buffer_ref(probe->original);
    if (!original) throw std::bad_alloc();
  }
  av_buffer_unref(&opaque);
  opaque = original;
  Observe(elapsed);
}

void StageLatency::Observe(std::optional<std::int64_t> nanoseconds) noexcept {
  if (!active_) return;
  const bool valid = nanoseconds && *nanoseconds >= 0;
  for (auto* sample : {&total_, &interval_}) {
    if (valid) {
      ++sample->valid;
      sample->total_ns += *nanoseconds;
      sample->max_ns = std::max(sample->max_ns, *nanoseconds);
    } else {
      ++sample->unavailable;
    }
  }
  if (trace_) {
    if (valid) {
      MW_LOG_TRACE(module_,
                   "Stage[{}] stage={} stream_index={} latency({:.3f}ms)",
                   instance_, stage_, stream_index_,
                   static_cast<double>(*nanoseconds) / 1e6);
    } else {
      MW_LOG_TRACE(module_, "Stage[{}] stage={} stream_index={} latency(N/A)",
                   instance_, stage_, stream_index_);
    }
  }
  Report(false);
}

void StageLatency::Report(bool final) noexcept {
  if (!active_) return;
  const auto now = Clock::now();
  if (!final && now - reported_ < std::chrono::seconds(2)) return;
  const auto& sample = final ? total_ : interval_;
  const auto seconds =
      std::chrono::duration<double>(now - (final ? started_ : reported_))
          .count();
  const auto count = sample.valid + sample.unavailable;
  const auto throughput =
      seconds > 0 ? static_cast<double>(count) / seconds : 0;
  if (sample.valid) {
    MW_LOG_INFO(module_,
                "Stage[{}] stage={} stream_index={} report={} window_s={:.3f} "
                "samples={} unavailable={} throughput={:.3f}/s "
                "latency(mean:{:.3f}ms,max:{:.3f}ms)",
                instance_, stage_, stream_index_,
                final ? "summary" : "interval", seconds, count,
                sample.unavailable, throughput,
                static_cast<double>(sample.total_ns) / 1e6 /
                    static_cast<double>(sample.valid),
                static_cast<double>(sample.max_ns) / 1e6);
  } else {
    MW_LOG_INFO(module_,
                "Stage[{}] stage={} stream_index={} report={} window_s={:.3f} "
                "samples={} unavailable={} throughput={:.3f}/s "
                "latency(mean:N/A,max:N/A)",
                instance_, stage_, stream_index_,
                final ? "summary" : "interval", seconds, count,
                sample.unavailable, throughput);
  }
  interval_ = {};
  reported_ = now;
}

void StageLatency::Finish() noexcept {
  if (!active_) return;
  Report(true);
  active_ = false;
}

}  // namespace mw::streamer::internal
