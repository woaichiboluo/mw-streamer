#ifndef MW_STREAMER_OUTPUT_INTERNAL_RTSP_LISTENER_MANAGER_H_
#define MW_STREAMER_OUTPUT_INTERNAL_RTSP_LISTENER_MANAGER_H_

#include <cstdint>
#include <string>

namespace mw::streamer::internal {

// Keeps an RTSP listener alive while its published path is in use. Acquiring
// another path on the same endpoint shares the listener. The last lease closes
// it. Acquisition and release are safe from different threads.
class RtspListenerLease final {
 public:
  RtspListenerLease() noexcept = default;
  ~RtspListenerLease();

  RtspListenerLease(const RtspListenerLease&) = delete;
  RtspListenerLease& operator=(const RtspListenerLease&) = delete;
  RtspListenerLease(RtspListenerLease&& other) noexcept;
  RtspListenerLease& operator=(RtspListenerLease&& other) noexcept;

  // Starts the listener on first use. A duplicate path or failed bind throws.
  // Port zero is invalid because published RTSP addresses must stay stable.
  static RtspListenerLease Acquire(std::string bind_ip, std::uint16_t port,
                                   std::string app, std::string stream);

 private:
  RtspListenerLease(std::string bind_ip, std::uint16_t port, std::string app,
                    std::string stream);
  void Release() noexcept;

  std::string bind_ip_;
  std::uint16_t port_ = 0;
  std::string app_;
  std::string stream_;
  bool active_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_OUTPUT_INTERNAL_RTSP_LISTENER_MANAGER_H_
