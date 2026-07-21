#include "mw/streamer/internal/common/reconnect_backoff.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {

ReconnectBackoff::ReconnectBackoff(ReconnectConfig config)
    : config_(std::move(config)) {
  if (config_.initial_backoff.count() <= 0 ||
      config_.maximum_backoff < config_.initial_backoff ||
      !std::isfinite(config_.jitter_ratio) || config_.jitter_ratio < 0.0 ||
      config_.jitter_ratio > 1.0) {
    ThrowError(StatusCode::kInvalidArgument,
               "reconnect backoff configuration is invalid");
  }
}

bool ReconnectBackoff::CanRetry() const noexcept {
  return config_.max_retries == 0 || retry_count_ < config_.max_retries;
}

std::chrono::milliseconds ReconnectBackoff::NextDelay(double jitter_unit) {
  if (!CanRetry()) {
    ThrowError(StatusCode::kInvalidState,
               "reconnect retry limit has been exhausted");
  }
  if (!std::isfinite(jitter_unit) || jitter_unit < -1.0 || jitter_unit > 1.0) {
    ThrowError(StatusCode::kInvalidArgument,
               "reconnect jitter sample must be within [-1, 1]");
  }

  std::int64_t base_delay = config_.initial_backoff.count();
  for (std::size_t index = 0;
       index < retry_count_ && base_delay < config_.maximum_backoff.count();
       ++index) {
    base_delay =
        std::min(config_.maximum_backoff.count(),
                 base_delay > std::numeric_limits<std::int64_t>::max() / 2
                     ? config_.maximum_backoff.count()
                     : base_delay * 2);
  }

  const double multiplier = 1.0 + config_.jitter_ratio * jitter_unit;
  const auto delay = std::chrono::milliseconds(
      static_cast<std::int64_t>(std::llround(base_delay * multiplier)));
  ++retry_count_;
  return delay;
}

void ReconnectBackoff::Reset() noexcept { retry_count_ = 0; }

std::size_t ReconnectBackoff::retry_count() const noexcept {
  return retry_count_;
}

}  // namespace mw::streamer::internal
