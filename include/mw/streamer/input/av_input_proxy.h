#ifndef MW_STREAMER_INPUT_AV_INPUT_PROXY_H_
#define MW_STREAMER_INPUT_AV_INPUT_PROXY_H_

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/input/av_input.h"

namespace mw::streamer {

struct AvInputProxyConfig {
  AvInputConfig input;
  bool loop = false;
  // Additional attempts after failure. -1 means unlimited; 0 means none.
  // Success resets the count. Normal EOF looping does not consume this budget.
  int max_retries = -1;
};

enum class AvInputProxyState {
  kStopped,
  kConnecting,
  kConnected,
  kWaitingRetry,
  // No more source data. Consumers may still be delivering returned packets;
  // drain downstream only after the consumer finishes, not from this event.
  kEnded,
  kFailed,
};

struct AvInputProxyStatus {
  AvInputProxyState state = AvInputProxyState::kStopped;
  // Each new connection attempt has its own generation, starting at zero on
  // Start. Finish old consumer calls before starting a new proxy session.
  std::uint64_t generation = 0;
  std::uint64_t retry_count = 0;
  std::chrono::seconds retry_delay{0};
  // Original exception for WaitingRetry/Failed; cleared on a successful open.
  std::exception_ptr error;
  std::shared_ptr<const AvInputInfo> info;
};

struct AvInputProxyReadResult {
  AvInputReadStatus status = AvInputReadStatus::kNull;
  std::optional<ffmpeg::Packet> packet;
  std::uint64_t generation = 0;
  // This packet's immutable description, even if a later attempt is already
  // current. Does not retain an AvInput or its connection resources.
  std::shared_ptr<const AvInputInfo> info;
};

// Owns one control worker. Each attempt uses a fresh AvInput, which owns all
// packet production and buffering. Lifecycle/configuration calls are serialized
// externally; one packet consumer may run concurrently with Stop. Snapshots and
// RequestStop are thread-safe. No packet processing or extra packet queue.
// Stop and finish consumer calls before destruction or restarting.
class MW_STREAMER_API AvInputProxy final {
 public:
  // Serial notifications on the control worker, outside locks. Observers may
  // query snapshots or RequestStop, but must not block on ReadPacket, call
  // Start/Stop/configuration, or destroy the proxy. Exceptions are logged.
  using OnStateChanged = std::function<void(const AvInputProxyStatus&)>;

  explicit AvInputProxy(AvInputProxyConfig config = {});
  ~AvInputProxy();
  AvInputProxy(const AvInputProxy&) = delete;
  AvInputProxy& operator=(const AvInputProxy&) = delete;
  AvInputProxy(AvInputProxy&&) = delete;
  AvInputProxy& operator=(AvInputProxy&&) = delete;

  void SetOnStateChanged(OnStateChanged callback);
  // Returns after launching the control worker. Connection errors are delivered
  // through status notifications; invalid calls/thread creation may throw.
  // Call Stop and finish old reads before another Start.
  void Start(std::string_view url);
  // Waits through connection attempts and loop transitions. Reports EOF only
  // for a completed non-looping source; final failure/Stop return kNull.
  // Source errors belong to status notifications, not the read result.
  AvInputProxyReadResult ReadPacket();
  // Returns kNull immediately during opening/retries or producer underflow.
  AvInputProxyReadResult TryReadPacket();
  void RequestStop() noexcept;
  // Interrupts opening, reading and retry waits, joins the control worker and
  // closes all connections. No callbacks after returning. Final Ended/Failed
  // states are retained; explicit stop reports Stopped.
  void Stop() noexcept;

  AvInputProxyStatus status() const;
  // For observations. Consumers use the description in their read result.
  std::shared_ptr<const AvInputInfo> info() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_AV_INPUT_PROXY_H_
