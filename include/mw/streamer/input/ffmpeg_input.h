#ifndef MW_STREAMER_INPUT_FFMPEG_INPUT_H_
#define MW_STREAMER_INPUT_FFMPEG_INPUT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

struct AVFormatContext;

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

namespace internal {
class PlaybackClock;
}
namespace ffmpeg {
class Decoder;
}

struct MW_STREAMER_API FfmpegInputConfig {
  // Empty names select FFmpeg's default decoder for each track's codec_id.
  std::string video_decoder_name;
  std::string audio_decoder_name;
  // Without decoding, deliver packets as read, without playback scheduling.
  bool decode = true;
  // EOF reopens the input while remaining connected; otherwise kEnded.
  bool loop = false;
  bool auto_reconnect = true;
  std::chrono::milliseconds retry_interval{2000};
  // -1 retries indefinitely; successful media preparation resets the count.
  int max_retries = -1;
  // Overall deadline for opening and discovering streams.
  std::chrono::milliseconds open_timeout{30000};
  std::chrono::milliseconds read_timeout{10000};
  // Total queued compressed payload across the selected audio/video tracks.
  // An imbalanced or malformed source exceeding this limit fails explicitly.
  std::size_t max_packet_buffer_bytes = 64 * 1024 * 1024;
};

// OBS media-playback-inspired, demand-driven reading, decoding and scheduling.
// Requires Init(); destroy before Shutdown(). Set callbacks before Start().
// A media worker owns each connection's reading, decoding and delivery. An
// on-demand reconnect worker waits and creates the next media worker.
// Callbacks must return promptly. Do not call Start/Stop from a callback.
class MW_STREAMER_API FfmpegInput final {
 public:
  // Once per Start(), after stream initialization and before any packet/frame.
  // Loops/reconnects retain this contract; changed track formats fail input.
  using OnReady = std::function<void(const std::vector<ffmpeg::StreamInfo>&)>;
  // Selected audio/video packets, once at submission, before decoding (if
  // enabled). pts/dts/duration retain the original StreamInfo.time_base;
  // stream_index identifies the stream. Per-stream order is preserved.
  // generation starts at zero on each Start(), and increments on successful
  // loop/reconnect reopening or seeking, before submitting the next packet.
  // The reference is callback-scoped; Packet::Ref() retains its buffers.
  using OnPacket =
      std::function<void(std::uint64_t generation, const ffmpeg::Packet&)>;
  // Frame pts is OBS-style output time in nanoseconds, relative to the shared
  // monotonic epoch established by the first Input start. Frame time_base is
  // {1, 1000000000}; duration uses that same unit. StreamInfo still describes
  // the original input stream. best_effort_timestamp retains the decoder's
  // original media timestamp in StreamInfo.time_base (possibly AV_NOPTS_VALUE);
  // pkt_dts is cleared because it does not belong to the output timeline.
  // Loops accumulate the previous cycle's absolute media end PTS; reconnects
  // map the new stream to current system time. Seek can move output pts back.
  // Scheduling follows OBS: discontinuities do not rebase the clock, and a
  // track leading by more than two seconds may be delivered early.
  // The reference is callback-scoped; Frame::Ref() retains its buffers.
  using OnFrame = std::function<void(int, const ffmpeg::Frame&)>;
  // State events run on either worker. Waiting retry runs on the reconnect
  // worker; kStopped runs on the caller of Stop(). Ready/packet/frame callbacks
  // run on the media worker for the current connection.
  using OnStateChanged = std::function<void(InputState, int, std::string_view)>;

  explicit FfmpegInput(FfmpegInputConfig config = {});
  // Shares the external video device across retries and loops.
  // Audio needs no device.
  explicit FfmpegInput(const ffmpeg::HwDeviceContext& video_device,
                       FfmpegInputConfig config = {});
  ~FfmpegInput();
  FfmpegInput(const FfmpegInput&) = delete;
  FfmpegInput& operator=(const FfmpegInput&) = delete;
  FfmpegInput(FfmpegInput&&) = delete;
  FfmpegInput& operator=(FfmpegInput&&) = delete;

  void SetOnReady(OnReady callback);
  void SetOnPacket(OnPacket callback);
  void SetOnFrame(OnFrame callback);
  void SetOnStateChanged(OnStateChanged callback);
  // Starting while connecting, connected or waiting to retry throws.
  // Network failures and established looping inputs follow the retry policy.
  // Initial local open failures do not retry. EOF ends input unless looping.
  void Start(std::string_view url);
  // Asynchronously seek an active local file backward to a keyframe. The
  // latest request wins; inactive input ignores it. Negative positions and
  // active network sources throw. The output timestamp mapping is retained,
  // so frame pts follows the seek and may move backward.
  // Seeking neither re-announces streams nor changes connection state.
  void Seek(std::chrono::milliseconds position);
  // Cancels I/O, scheduled delivery and retry waits; joins both workers.
  // Emits kStopped once, before returning. Start after Stop is supported.
  void Stop();
  InputState state() const noexcept;

 private:
  struct Track;

  static int InterruptCallback(void* opaque);
  void RunMedia();
  void ScheduleReconnect(int error, std::string message);
  void RunReconnect(int error, std::string message);
  void JoinWorkers();
  void OpenAttempt();
  bool ReadPacket(ffmpeg::Packet& packet);
  void SubmitPacket(const ffmpeg::Packet& packet, ffmpeg::Decoder* decoder);
  void PlayPackets(bool initialized);
  bool PrepareFrames();
  bool ApplySeek();
  bool HasSeekRequest();
  void PlayAttempt(internal::PlaybackClock& playback, bool first_attempt);
  void ClearAttempt() noexcept;
  void StopWorkers(bool notify);
  void NotifyState(InputState state, int error = 0,
                   std::string_view message = {});
  void SetIoDeadline(std::chrono::milliseconds timeout);

  FfmpegInputConfig config_;
  std::optional<ffmpeg::HwDeviceContext> video_device_;
  OnReady on_ready_;
  OnPacket on_packet_;
  OnFrame on_frame_;
  OnStateChanged on_state_changed_;
  std::atomic<InputState> state_{InputState::kIdle};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::int64_t> io_deadline_ns_{0};
  std::mutex control_mutex_;
  std::mutex thread_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable wake_;
  std::optional<std::chrono::milliseconds> seek_position_;
  std::thread media_thread_;
  std::thread reconnect_thread_;
  int retries_ = 0;
  std::uint64_t generation_ = 0;
  // OBS retains the video duration estimate across EOF resets. Preserve it
  // even though this Input reopens the decoder at each loop boundary.
  std::int64_t video_last_duration_ns_ = 0;
  std::string url_;
  std::atomic<bool> network_source_{false};
  bool input_eof_ = false;
  AVFormatContext* format_ = nullptr;
  std::vector<ffmpeg::StreamInfo> streams_;
  // First initialized tracks of the current Start() session.
  std::vector<ffmpeg::StreamInfo> ready_streams_;
  std::vector<std::unique_ptr<Track>> tracks_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_FFMPEG_INPUT_H_
