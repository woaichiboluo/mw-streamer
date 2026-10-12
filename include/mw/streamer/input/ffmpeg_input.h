#ifndef MW_STREAMER_INPUT_FFMPEG_INPUT_H_
#define MW_STREAMER_INPUT_FFMPEG_INPUT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
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
#include "mw/streamer/input/input_frame_source.h"
#include "mw/streamer/performance/performance.h"

struct AVFormatContext;

namespace mw::streamer {

enum class InputMode {
  kLive,   // Original decoded media frames for downstream live scheduling.
  kRemux,  // Original compressed packets, without decoding or playback waits.
  kBatch,  // Original decoded media frames for synchronous batch processing.
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
class FfmpegPacketReader;
}  // namespace internal
namespace ffmpeg {
class Decoder;
}

struct MW_STREAMER_API FfmpegInputConfig {
  // Empty names prefer libvpx for VP8/VP9, then FFmpeg's default decoder.
  // Explicit decoder selection is ignored in kRemux.
  std::string video_decoder_name;
  std::string audio_decoder_name;
  InputMode mode = InputMode::kLive;
  // EOF reopens while remaining connected. Decoded segments with at most
  // two produced frames end instead; pull consumers report channel frames.
  // Remux loops at packet EOF without this decoded-frame restriction.
  bool loop = false;
  // Network inputs include RTSP/RTSPS demuxer URLs and the AVIO protocols
  // supported by the linked FFmpeg (for example RTMP(S), HTTP(S), and SRT).
  bool auto_reconnect = true;
  std::chrono::milliseconds retry_interval{2000};
  // -1 retries indefinitely; successful media preparation resets the count.
  int max_retries = -1;
  // Overall deadline for opening and discovering streams.
  std::chrono::milliseconds open_timeout{60000};
  std::chrono::milliseconds read_timeout{60000};
  // Optional total compressed-payload limit. Zero uses packet-count watermarks
  // without adding a byte cutoff. A caller-supplied limit
  // fails explicitly if a malformed/imbalanced source cannot make progress.
  std::size_t max_packet_buffer_bytes = 0;
  // FFmpeg input/protocol options passed to avformat_open_input on every open,
  // including reconnects and loops. User values override built-in defaults.
  // Unknown or inapplicable options retain FFmpeg's behavior: opening may
  // succeed, with unused option names logged as warnings. Invalid values for
  // recognized options follow the existing input error/retry policy.
  std::map<std::string, std::string> options;
};

// Full-speed reading and per-track decoding, bounded by downstream consumption.
// Requires Init(); destroy before Shutdown(). Set callbacks before Start().
// All modes prefetch up to 256 raw packets on a separate demux worker.
// Decoded modes use one independent decoder worker and an eight-frame output
// queue per track. The media worker dispatches packets and serializes public
// callbacks without playback waits or timestamp normalization. A reconnect
// worker waits and creates the next attempt. kLive/kRemux callbacks must return
// promptly. kBatch frame callbacks may process synchronously; Stop waits for
// the current callback to return. Do not call Start/Stop from a callback.
class MW_STREAMER_API FfmpegInput final {
 public:
  // Before creating decoders or starting packet delivery, once per open or
  // seek. Receives all audio/video descriptions and returns the required source
  // indices. Only these tracks are decoded/delivered. Explicit selection may
  // include non-default tracks. Empty, duplicate or invalid indices fail the
  // attempt; selector exceptions propagate through the input failure path.
  // Without a selector, selects the first video and first audio in source
  // order, regardless of disposition. An empty callback restores this default.
  using OnSelectStreams =
      std::function<std::vector<int>(const std::vector<ffmpeg::StreamInfo>&)>;
  // After each open/reopen and decoder initialization, before packet/frame
  // callbacks for the generation, also after seeking. Describes selected tracks
  // only. Source tracks and formats may change;
  // downstream consumers decide whether they can accept the new description.
  using OnReady = std::function<void(const std::vector<ffmpeg::StreamInfo>&)>;
  // Selected original audio/video packets, once at submission, before decoding
  // in kLive/kBatch. pts/dts/duration retain the original StreamInfo.time_base;
  // stream_index identifies the stream. Per-stream order is preserved.
  // generation starts at zero on each Start(), and increments on successful
  // loop/reconnect reopening or seeking, before submitting the next packet.
  // The reference is callback-scoped; Packet::Ref() retains its buffers.
  using OnPacket =
      std::function<void(std::uint64_t generation, const ffmpeg::Packet&)>;
  // No frame delivery in kRemux. Both decoded modes deliver at available
  // throughput, without an Input clock. pts=best_effort_timestamp uses the
  // stream time_base; unknown PTS remains AV_NOPTS_VALUE. duration, pkt_dts,
  // sample_rate and sample buffers retain decoded values. Loops, reconnection
  // and seeking expose source media time again, including backward jumps.
  // Per-track order is preserved; cross-track delivery has no PTS ordering.
  // The reference is callback-scoped; Frame::Ref() retains its buffers.
  using OnFrame = std::function<void(int, const ffmpeg::Frame&)>;
  // Alternative to OnFrame: the downstream filter pulls each track as needed.
  // Called after OnReady on each open/seek. The consumer calls source.complete
  // after its filtered segment ends, with the produced frame count. Input waits
  // for completion before EOF or looping; decoded queues alone do not complete
  // a segment. Retained queues and normal EOF bounds remain readable after the
  // segment closes. Configure only one delivery API.
  using OnDecodedReady = std::function<void(const DecodedInputSource&)>;
  // Normal decoder EOF after its last frame has been consumed, once per track
  // and generation. end_pts uses StreamInfo.time_base; AV_NOPTS_VALUE means
  // no known boundary. Duration estimation serves this boundary only.
  // No event for cancelled or failed tracks. Runs on the media worker.
  using OnTrackEnded = std::function<void(int, std::int64_t end_pts)>;
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
  void SetOnSelectStreams(OnSelectStreams callback);
  void SetOnPacket(OnPacket callback);
  void SetOnFrame(OnFrame callback);
  void SetOnDecodedReady(OnDecodedReady callback);
  void SetOnTrackEnded(OnTrackEnded callback);
  void SetOnStateChanged(OnStateChanged callback);
  // Starting while connecting, connected or waiting to retry throws.
  // Network failures and established looping inputs follow the retry policy.
  // Initial local open failures do not retry. EOF ends input unless looping.
  void Start(std::string_view url);
  // Asynchronously seek an active local file backward to a keyframe. The
  // latest request wins; inactive input ignores it. Negative positions and
  // active network sources throw. Decoders are recreated; raw media PTS
  // follows the source position and may move backward.
  // Seeking to zero reopens the source. Every seek reselects and re-announces
  // tracks before delivering the new segment. Connection state is unchanged.
  void Seek(std::chrono::milliseconds position);
  // Cancels I/O, queue and retry waits; joins all workers.
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
  void PrepareStreams();
  void CreateDecoders();
  void AnnounceDecodedSource(std::int64_t start_time_us, bool flush = false);
  bool ReadPacket(ffmpeg::Packet& packet);
  int ReadSelectedPacket(ffmpeg::Packet& packet);
  void StartReadAhead();
  void StopReadAhead() noexcept;
  void SubmitPacket(const ffmpeg::Packet& packet);
  void PlayPackets(bool initialized);
  bool PumpPackets();
  bool ApplySeek();
  bool HasSeekRequest();
  std::int64_t PlayAttempt();
  void ClearAttempt() noexcept;
  void StopWorkers(bool notify);
  void NotifyState(InputState state, int error = 0,
                   std::string_view message = {});
  void SetIoDeadline(std::chrono::milliseconds timeout);
  void ReportPerformance(bool final = false);

  FfmpegInputConfig config_;
  std::optional<ffmpeg::HwDeviceContext> video_device_;
  OnReady on_ready_;
  OnSelectStreams on_select_streams_;
  OnPacket on_packet_;
  OnFrame on_frame_;
  OnDecodedReady on_decoded_ready_;
  OnTrackEnded on_track_ended_;
  OnStateChanged on_state_changed_;
  std::atomic<InputState> state_{InputState::kIdle};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> read_stop_requested_{false};
  std::atomic<std::int64_t> io_deadline_ns_{0};
  std::mutex control_mutex_;
  std::mutex thread_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable wake_;
  // Published under wait_mutex_; only the media worker moves/destroys it.
  std::unique_ptr<internal::FfmpegPacketReader> packet_reader_;
  std::optional<std::chrono::milliseconds> seek_position_;
  std::thread media_thread_;
  std::thread reconnect_thread_;
  int retries_ = 0;
  std::uint64_t generation_ = 0;
  std::string url_;
  std::atomic<bool> network_source_{false};
  bool input_eof_ = false;
  std::exception_ptr read_error_;
  AVFormatContext* format_ = nullptr;
  std::vector<ffmpeg::StreamInfo> streams_;
  // First initialized tracks of the current Start() session.
  std::vector<ffmpeg::StreamInfo> ready_streams_;
  std::vector<std::shared_ptr<Track>> tracks_;
  std::shared_ptr<std::atomic<std::int64_t>> source_completed_frames_;
  // The demux and media workers share counters; never lock across I/O.
  std::mutex performance_mutex_;
  internal::InputPerformance performance_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_FFMPEG_INPUT_H_
