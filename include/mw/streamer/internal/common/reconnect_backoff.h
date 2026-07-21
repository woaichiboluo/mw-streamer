#ifndef MW_STREAMER_INTERNAL_COMMON_RECONNECT_BACKOFF_H_
#define MW_STREAMER_INTERNAL_COMMON_RECONNECT_BACKOFF_H_

#include <chrono>
#include <cstddef>

#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {

class ReconnectBackoff final {
 public:
  explicit ReconnectBackoff(ReconnectConfig config);

  [[nodiscard]] bool CanRetry() const noexcept;
  [[nodiscard]] std::chrono::milliseconds NextDelay(double jitter_unit);
  void Reset() noexcept;

  [[nodiscard]] std::size_t retry_count() const noexcept;

 private:
  ReconnectConfig config_;
  std::size_t retry_count_ = 0;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMMON_RECONNECT_BACKOFF_H_
