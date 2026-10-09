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
#include "mw/streamer/performance/performance.h"

struct AVFormatContext;

namespace mw::streamer {

enum class InputMode {
  kLive,   // Decoded frames on the speed-adjusted playback timeline.
  kRemux,  // Original compressed packets, without decoding or playback waits.
  kBatch,  // Decoded media frames; speed controls delivery only.
};

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
  // Decoder selection is ignored in kRemux.
  std::string video_decoder_name;
  std::string audio_decoder_name;
  InputMode mode = InputMode::kLive;
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
  // Fixed decoded delivery speed, in the finite range [0.5, 8.0]. Ignored in
  // kRemux. kLive scales frame timestamps and audio sample_rate (changing
  // pitch); kBatch applies speed only to delivery waits, retaining media time
  // and the decoded audio sample_rate. Processing can slow delivery.
  double playback_speed = 1.0;
};

// Demand-driven reading, decoding and scheduled frame delivery.
// Requires Init(); destroy before Shutdown(). Set callbacks before Start().
// A media worker owns each connection's reading, decoding and delivery. An
// on-demand reconnect worker waits and creates the next media worker.
// kLive/kRemux callbacks must return promptly. kBatch frame callbacks may
// process synchronously; Stop waits for the current callback to return.
// Do not call Start/Stop from a callback.
class MW_STREAMER_API FfmpegInput final {
 public:
  // Once per Start(), after stream initialization and before any packet/frame.
  // Loops/reconnects retain this contract; changed track formats fail input.
  using OnReady = std::function<void(const std::vector<ffmpeg::StreamInfo>&)>;
  // Selected audio/video packets, once at submission, before decoding in
  // kLive/kBatch. pts/dts/duration retain the original StreamInfo.time_base;
  // stream_index identifies the stream. Per-stream order is preserved.
  // generation starts at zero on each Start(), and increments on successful
  // loop/reconnect reopening or seeking, before submitting the next packet.
  // The reference is callback-scoped; Packet::Ref() retains its buffers.
  using OnPacket =
      std::function<void(std::uint64_t generation, const ffmpeg::Packet&)>;
  // No frame delivery in kRemux. Frame time_base is {1, 1000000000}; pts and
  // duration use nanoseconds. StreamInfo still describes the original input.
  //
  // kLive pts is relative to the shared monotonic epoch established by the
  // first Input start. Playback speed scales pts/duration and audio
  // sample_rate. Loops accumulate the previous cycle's absolute media end PTS;
  // reconnects map the new stream to current system time.
  //
  // kBatch pts/duration retain media time (estimated when missing), without
  // rebasing or speed scaling; audio sample_rate and samples remain original.
  // Loops/reconnects expose the reopened source's media time again.
  //
  // In both decoded modes,
  // best_effort_timestamp retains the decoder's original timestamp in
  // StreamInfo.time_base (possibly AV_NOPTS_VALUE), and pkt_dts is cleared.
  // Seek can move frame pts backward. Timestamp discontinuities do not rebase
  // the scheduling clock, and a track leading by more than two seconds may be
  // delivered early. The reference is callback-scoped; Frame::Ref() retains its
  // buffers.
  using OnFrame = std::function<void(int, const ffmpeg::Frame&)>;
  // State events run on either worker. Waiting retry runs on the reconnect
  // worker; kStopped runs on the caller of Stop(). Ready/packet/frame
  // callbacks run on the media worker for the current connection.
  using OnStateChanged = std::function<void(InputState, int, std::string_view)>;

  // Invalid configuration throws std::invalid_argument.
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
  void ReportPerformance(bool final = false);
  void WaitForPlayback(std::unique_lock<std::mutex>& lock,
                       std::chrono::steady_clock::time_point deadline);

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
  // Retain the video duration estimate across EOF resets, preserving it
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
  // Media workers serialize updates; Stop samples only after joining them.
  internal::InputPerformance performance_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_FFMPEG_INPUT_H_
