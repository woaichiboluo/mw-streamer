#ifndef MW_STREAMER_INPUT_ZLM_INPUT_H_
#define MW_STREAMER_INPUT_ZLM_INPUT_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mediakit {
class MediaPlayer;
}  // namespace mediakit

namespace toolkit {
class EventPoller;
class Timer;
}  // namespace toolkit

namespace mw::streamer::internal {
class ZlmPacketConverter;
}  // namespace mw::streamer::internal

namespace mw::streamer {

enum class InputState {
  kIdle,
  kConnecting,
  kConnected,
  kWaitingRetry,
  kEnded,
  kFailed,
  kStopped,
};

struct MW_STREAMER_API ZlmInputConfig {
  bool auto_reconnect = true;
  std::chrono::milliseconds retry_interval{2000};

  // Retries after a failed connection or disconnection. -1 means unlimited;
  // zero disables retries. The retry count resets after a successful
  // connection.
  int max_retries = -1;
};

// Receives and converts ZLM streams and packets without playback scheduling.
// Requires an active MwStreamerContext from Init(). Destroy before Shutdown().
// Set callbacks before Start(); the caller is responsible for this ordering.
// Callbacks must return promptly and must not propagate exceptions into ZLM.
// Stop() is called by the external pipeline, never from a callback.
class MW_STREAMER_API ZlmInput final {
 public:
  using OnReady = std::function<void(const std::vector<ffmpeg::StreamInfo>&)>;
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  using OnStateChanged = std::function<void(InputState, int, std::string_view)>;

  explicit ZlmInput(ZlmInputConfig config = {});
  ~ZlmInput();

  ZlmInput(const ZlmInput&) = delete;
  ZlmInput& operator=(const ZlmInput&) = delete;
  ZlmInput(ZlmInput&&) = delete;
  ZlmInput& operator=(ZlmInput&&) = delete;

  // Delivers all ready streams before packets, including after reconnection.
  // The reference is valid only for the duration of the callback.
  void SetOnReady(OnReady callback);

  // Runs synchronously on the ZLM delivery thread. Must not block.
  // Use ffmpeg::Packet::Ref() to retain a packet after the callback returns.
  void SetOnPacket(OnPacket callback);

  // Error code is zero when there is no error. The message is valid only
  // for the duration of the callback.
  void SetOnStateChanged(OnStateChanged callback);

  // Copies the URL and starts playback asynchronously. Synchronous argument
  // and usage errors may throw; playback errors use the state callback.
  // Starting while already active throws std::logic_error.
  // Reconnection uses config; normal file EOF enters kEnded without retrying.
  void Start(std::string_view url);

  // Stops playback and cancels pending reconnection. After this returns,
  // no further callbacks or reconnection attempts occur until the next Start().
  // Must be called outside callbacks; this precondition is not checked.
  void Stop();

  InputState state() const noexcept;

 private:
  struct TrackBinding;

  void BeginAttempt();
  void HandlePlayResult(const std::shared_ptr<std::atomic<bool>>& active,
                        int error_code, std::string_view message);
  void QueueFailure(const std::shared_ptr<std::atomic<bool>>& active,
                    int error_code, std::string message, bool retry);
  void HandleFailure(int error_code, std::string_view message, bool retry);
  void HandleEnd();
  void ClearAttempt();
  void NotifyState(InputState state, int error_code = 0,
                   std::string_view message = {});

  ZlmInputConfig config_;
  OnReady on_ready_;
  OnPacket on_packet_;
  OnStateChanged on_state_changed_;
  std::string url_;
  std::atomic<InputState> state_{InputState::kIdle};
  std::mutex control_mutex_;
  std::mutex delivery_mutex_;
  std::shared_ptr<toolkit::EventPoller> poller_;
  std::shared_ptr<std::atomic<bool>> attempt_active_;
  int retry_count_ = 0;
  std::shared_ptr<mediakit::MediaPlayer> player_;
  std::shared_ptr<toolkit::Timer> retry_timer_;
  std::vector<ffmpeg::StreamInfo> streams_;
  std::vector<std::shared_ptr<internal::ZlmPacketConverter>> converters_;
  std::vector<TrackBinding> bindings_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_ZLM_INPUT_H_
