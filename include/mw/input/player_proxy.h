#ifndef MW_STREAMER_INCLUDE_MW_INPUT_PLAYER_PROXY_H_
#define MW_STREAMER_INCLUDE_MW_INPUT_PLAYER_PROXY_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

#include "Network/Socket.h"
#include "Util/mini.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::input {

struct ReconnectPolicy {
  // Number of retries after the initial attempt. A negative value retries
  // indefinitely.
  int max_retries = -1;
  std::chrono::milliseconds min_delay{2000};
  std::chrono::milliseconds max_delay{60000};
  std::chrono::milliseconds delay_step{3000};
};

enum class PlayerState {
  kIdle,
  kConnecting,
  kReady,
  kWaitingRetry,
  kEnded,
  kFailed,
  kStopped,
};

enum class ControlResult {
  kAccepted,
  kInvalidState,
  kNotSupported,
  kInvalidArgument,
  kFailed,
};

enum class TimelineResetReason {
  kSeek,
};

struct StreamInfo {
  int stream_index = -1;
  std::shared_ptr<AVCodecParameters> codec_parameters;
  AVRational time_base{0, 1};
};

class PlayerProxy final {
 public:
  using Ptr = std::shared_ptr<PlayerProxy>;
  using OnPacket =
      std::function<bool(std::uint64_t generation, const AVPacket* packet)>;
  using OnStreamsReady = std::function<void(
      std::uint64_t generation, const std::vector<StreamInfo>& streams)>;
  using OnState = std::function<void(
      std::uint64_t generation, PlayerState state,
      const toolkit::SockException& reason, bool will_retry)>;
  using OnTimelineReset =
      std::function<void(std::uint64_t generation, TimelineResetReason reason,
                         std::chrono::milliseconds position)>;
  using OnControlCompleted =
      std::function<void(ControlResult result, std::uint64_t generation)>;
  using OnStopped = std::function<void()>;

  explicit PlayerProxy(std::shared_ptr<toolkit::EventPoller> poller = nullptr,
                       ReconnectPolicy reconnect_policy = {});
  ~PlayerProxy();

  PlayerProxy(const PlayerProxy&) = delete;
  PlayerProxy& operator=(const PlayerProxy&) = delete;

  // Callback setters and control methods are serialized on the owner poller.
  // Callbacks are invoked on that poller. AVPacket ownership remains with the
  // proxy and is valid only for the duration of OnPacket.
  void SetOnPacket(OnPacket callback);
  void SetOnStreamsReady(OnStreamsReady callback);
  void SetOnState(OnState callback);
  void SetOnTimelineReset(OnTimelineReset callback);

  // One proxy manages one active URL. Start again only after stop completes or
  // the previous finite input reaches a terminal state.
  void Start(std::string url, toolkit::mINI options = {});

  // Playback controls are accepted only while a finite local input is Ready.
  // Completion reports that validation passed and the command was synchronously
  // submitted to ZLM. Seek and playback-rate changes resume a paused file.
  void Pause(bool paused, OnControlCompleted completed = {});
  void SeekTo(std::chrono::milliseconds position,
              OnControlCompleted completed = {});
  void SetPlaybackRate(float rate, OnControlCompleted completed = {});

  // Completion means retry tasks, Track delegates, converters, and the active
  // ZLM player attempt have all been released on the owner poller.
  void Stop(OnStopped on_stopped = {});

  PlayerState state() const noexcept;
  std::uint64_t generation() const noexcept;
  std::uint64_t reconnect_count() const noexcept;
  std::shared_ptr<toolkit::EventPoller> poller() const;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer::input

#endif  // MW_STREAMER_INCLUDE_MW_INPUT_PLAYER_PROXY_H_
