#ifndef MW_STREAMER_PERFORMANCE_PERFORMANCE_H_
#define MW_STREAMER_PERFORMANCE_PERFORMANCE_H_

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

extern "C" {
#include <libavutil/rational.h>
}

struct AVPacket;
struct AVFrame;
struct AVCodecContext;

namespace mw::streamer {
enum class InputState;
namespace ffmpeg {
struct StreamInfo;
}
}  // namespace mw::streamer

namespace mw::streamer::internal {

// Internal counters. The owner serializes updates; there is no polling thread.
struct PerformanceCounters {
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint64_t frames = 0;
  std::uint64_t samples = 0;
  std::uint64_t errors = 0;
  std::uint64_t eof = 0;
  std::uint64_t again = 0;
  std::uint64_t reconnects = 0;
  std::uint64_t work_calls = 0;
  std::int64_t work_ns = 0;
  std::int64_t max_work_ns = 0;
  std::int64_t wait_ns = 0;
  std::int64_t media_ns = 0;
  bool has_media = false;

  bool operator==(const PerformanceCounters& other) const noexcept {
    return packets == other.packets && bytes == other.bytes &&
           frames == other.frames && samples == other.samples &&
           errors == other.errors && eof == other.eof && again == other.again &&
           reconnects == other.reconnects && work_calls == other.work_calls &&
           work_ns == other.work_ns && max_work_ns == other.max_work_ns &&
           wait_ns == other.wait_ns && media_ns == other.media_ns &&
           has_media == other.has_media;
  }
};

struct PerformanceReport {
  PerformanceCounters total;
  PerformanceCounters interval;
  double elapsed_seconds = 0;
  double interval_seconds = 0;

  // Keep counters integral until reporting; rates accept rounding to double.
  template <typename Value, typename Time>
  static double Rate(Value value, Time seconds) noexcept {
    return seconds > 0
               ? static_cast<double>(value) / static_cast<double>(seconds)
               : 0;
  }
};

class PerformanceWindow final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  // Default construction does not read the clock. Disabled log modules can
  // skip all sampling, including Reset, AddWork and Sample.
  PerformanceWindow() noexcept = default;

  void Reset(TimePoint now = Clock::now()) noexcept {
    total_ = {};
    previous_ = {};
    started_ = previous_time_ = now;
    window_max_work_ns_ = 0;
    active_ = true;
    final_reported_ = false;
  }

  PerformanceCounters& counters() noexcept { return total_; }
  const PerformanceCounters& counters() const noexcept { return total_; }

  void AddWork(std::int64_t nanoseconds) noexcept {
    nanoseconds = std::max<std::int64_t>(0, nanoseconds);
    ++total_.work_calls;
    total_.work_ns += nanoseconds;
    total_.max_work_ns = std::max(total_.max_work_ns, nanoseconds);
    window_max_work_ns_ = std::max(window_max_work_ns_, nanoseconds);
  }

  bool Sample(PerformanceReport& report, bool final = false,
              TimePoint now = Clock::now()) noexcept {
    if (!active_) return false;
    if (!final && now - previous_time_ < std::chrono::seconds(2)) return false;
    if (final && final_reported_ && total_ == previous_) return false;
    report.total = total_;
    report.interval = {total_.packets - previous_.packets,
                       total_.bytes - previous_.bytes,
                       total_.frames - previous_.frames,
                       total_.samples - previous_.samples,
                       total_.errors - previous_.errors,
                       total_.eof - previous_.eof,
                       total_.again - previous_.again,
                       total_.reconnects - previous_.reconnects,
                       total_.work_calls - previous_.work_calls,
                       total_.work_ns - previous_.work_ns,
                       window_max_work_ns_,
                       total_.wait_ns - previous_.wait_ns,
                       total_.media_ns - previous_.media_ns,
                       total_.has_media};
    report.elapsed_seconds =
        std::max(0.0, std::chrono::duration<double>(now - started_).count());
    report.interval_seconds = std::max(
        0.0, std::chrono::duration<double>(now - previous_time_).count());
    previous_ = total_;
    previous_time_ = now;
    window_max_work_ns_ = 0;
    final_reported_ = final;
    return true;
  }

 private:
  PerformanceCounters total_;
  PerformanceCounters previous_;
  TimePoint started_{};
  TimePoint previous_time_{};
  std::int64_t window_max_work_ns_ = 0;
  bool active_ = false;
  bool final_reported_ = false;
};

class InputPerformance final {
 public:
  void Start(const void* instance);
  void SegmentReset() noexcept;
  void Seek() noexcept;
  void Loop() noexcept;
  void Reconnect() noexcept;
  void BeginRead() noexcept;
  void Read(const AVPacket& packet, int result, bool stopped,
            const std::vector<ffmpeg::StreamInfo>& streams) noexcept;
  void BeginWait() noexcept;
  void EndWait() noexcept;
  void Queued(std::size_t bytes) noexcept;
  bool Sample(bool final = false) noexcept;
  void Report(std::uint64_t generation, InputState state,
              std::size_t buffered) const;

 private:
  static const char* StateName(InputState state) noexcept;
  static std::array<char, 48> RateText(double value, bool available) noexcept;

  PerformanceWindow window_;
  PerformanceReport report_;
  PerformanceWindow::TimePoint read_started_{};
  PerformanceWindow::TimePoint wait_started_{};
  std::optional<std::int64_t> reference_pts_ns_;
  const void* instance_ = nullptr;
  std::uint64_t loops_ = 0;
  std::uint64_t seeks_ = 0;
  std::size_t peak_buffer_bytes_ = 0;
  bool enabled_ = false;
  bool trace_enabled_ = false;
  bool active_ = false;
  bool final_ = false;
};

// Decoder API wall-time and throughput sampling on the decoder's worker.
class DecoderPerformance final {
 public:
  using Clock = PerformanceWindow::Clock;
  using TimePoint = PerformanceWindow::TimePoint;

  explicit DecoderPerformance(int stream_index) noexcept;
  void Start(const void* instance, const AVCodecContext& context) noexcept;
  TimePoint Begin() const noexcept;
  void PacketSent(TimePoint started, const AVPacket* packet,
                  int result) noexcept;
  void FrameReceived(TimePoint started, const AVFrame* frame,
                     int result) noexcept;
  void Drained(TimePoint started, int result) noexcept;
  void Flush() noexcept;
  void Stop() noexcept;

 private:
  void Finish(TimePoint started, bool error = false,
              bool final = false) noexcept;
  void CountFrame(const AVFrame& frame) noexcept;
  void Report(bool final) noexcept;

  PerformanceWindow window_;
  const void* instance_ = nullptr;
  int stream_index_;
  AVRational time_base_{};
  const char* module_ = nullptr;
  const char* codec_ = "?";
  const char* backend_ = "cpu";
  bool video_ = false;
  bool enabled_ = false;
  bool trace_enabled_ = false;
  bool final_ = false;
  std::int64_t previous_pts_ = 0;
};

// Updates run under the owning Encoder's existing mutex. Worker time includes
// frame allocation/transfer, codec calls and the packet callback.
class EncoderPerformance final {
 public:
  using TimePoint = PerformanceWindow::TimePoint;

  void Start(const void* instance, bool video, bool audio) noexcept;
  void AcceptVideo(std::size_t queued_frames, bool skipped_picture) noexcept;
  void AcceptAudio(int samples, std::size_t queued_frames) noexcept;
  void VideoDiscarded(std::size_t queued_frames) noexcept;
  void AudioDequeued(std::size_t queued_frames) noexcept;
  void AudioDiscarded(int samples) noexcept;
  void Abandon(bool video) noexcept;
  TimePoint Begin(bool video) const noexcept;
  void Encoded(bool video, TimePoint started, int samples = 0,
               bool repeated = false, std::size_t queued_frames = 0) noexcept;
  void Packet(bool video, int bytes) noexcept;
  void Error(bool video) noexcept;
  void Finish() noexcept;

 private:
  struct Track {
    PerformanceWindow window;
    std::uint64_t accepted = 0;
    std::uint64_t previous_accepted = 0;
    std::uint64_t skipped_picture = 0;
    std::uint64_t repeated = 0;
    std::uint64_t discarded_start = 0;
    std::uint64_t pending = 0;
    std::size_t queued_frames = 0;
    bool enabled = false;
    bool trace_enabled = false;
    bool active = false;
  };

  void Report(bool video, bool final = false) noexcept;
  const void* instance_ = nullptr;
  Track video_;
  Track audio_;
};

// The owning Remuxer serializes every update with its existing mutex. Handoff
// means inputFrame returned normally; it does not measure network delivery.
class RemuxPerformance final {
 public:
  using Clock = PerformanceWindow::Clock;
  using TimePoint = PerformanceWindow::TimePoint;

  void Start(const void* instance,
             const std::vector<ffmpeg::StreamInfo>& streams) noexcept;
  bool enabled() const noexcept { return enabled_; }
  TimePoint Begin() const noexcept;
  void Accepted(const AVPacket& packet) noexcept;
  void Rejected() noexcept;
  void Discarded(std::uint64_t cumulative_packets,
                 std::uint64_t cumulative_bytes) noexcept;
  void Error() noexcept;
  void Interleaved(TimePoint started, TimePoint finished) noexcept;
  void HandedOff(const AVPacket& packet, std::int64_t pts_ns,
                 std::int64_t dts_ns, std::uint64_t pts_ms,
                 std::uint64_t dts_ms, TimePoint queued_at,
                 TimePoint convert_started, TimePoint mux_started,
                 TimePoint finished) noexcept;
  // Pending includes packets already popped by the worker. Final reporting
  // accounts remaining packets as aborted and emits exactly one summary.
  void Report(std::size_t queue_packets, std::size_t queue_bytes,
              bool initialized, bool draining, bool final = false) noexcept;

 private:
  struct Counts {
    std::uint64_t accepted_packets = 0;
    std::uint64_t accepted_bytes = 0;
    std::uint64_t video_packets = 0;
    std::uint64_t audio_packets = 0;
    std::uint64_t startup_packets = 0;
    std::uint64_t startup_bytes = 0;
    std::uint64_t aborted_packets = 0;
    std::uint64_t aborted_bytes = 0;
    std::uint64_t rejected_packets = 0;
    std::uint64_t interleave_calls = 0;
  };
  struct Timing {
    void Add(std::int64_t elapsed_ns) noexcept;
    std::int64_t total_ns = 0;
    std::int64_t max_ns = 0;
    std::int64_t interval_max_ns = 0;
  };
  enum Stage { kQueueWait, kConvert, kMux, kInterleave, kStages };

  static std::int64_t Elapsed(TimePoint begin, TimePoint end) noexcept;
  std::uint64_t PendingPackets() const noexcept;
  std::uint64_t PendingBytes() const noexcept;

  PerformanceWindow window_;
  Counts counts_;
  Counts previous_;
  std::array<Timing, kStages> timing_{};
  std::array<std::int64_t, kStages> previous_stage_ns_{};
  std::uint64_t peak_pending_packets_ = 0;
  std::uint64_t peak_pending_bytes_ = 0;
  const void* instance_ = nullptr;
  int video_index_ = -1;
  int audio_index_ = -1;
  bool enabled_ = false;
  bool trace_enabled_ = false;
  bool active_ = false;
};

// All mutations are serialized by the owning Scheduler's existing mutex.
class SchedulerPerformance {
 public:
  using Clock = std::chrono::steady_clock;
  void Start(const void* instance, AVRational video_frame_rate,
             int audio_sample_rate, int audio_block_samples, bool video,
             bool audio) noexcept;
  bool enabled() const noexcept { return enabled_; }
  Clock::time_point BeginWork(bool active = true) const noexcept;
  std::int64_t WorkTime(Clock::time_point started) const noexcept;
  void Accept(bool video) noexcept;
  void Error(bool video) noexcept;
  void RejectVideo(std::size_t queued_frames) noexcept;
  void DropVideo() noexcept;
  void SelectVideo() noexcept;
  void SkipVideoTicks(std::uint64_t count) noexcept;
  void RejectAudio(int samples) noexcept;
  void DropAudio(std::int64_t samples) noexcept;
  void ConsumeAudio(std::int64_t samples, bool discarded) noexcept;
  void AudioUnderload() noexcept;
  template <typename AudioQueue>
  void DiscardAudioQueue(const AudioQueue& queue, bool active) noexcept {
    if (!enabled_ || !active) return;
    for (const auto& entry : queue) DropAudio(entry.samples);
  }
  void BeginAudioBatch() noexcept;
  void BeginAudioFrame() noexcept;
  void CopyAudio(std::int64_t samples) noexcept;
  void PrepareAudioFrame();
  void DeliverVideo(bool repeated, bool callback,
                    std::int64_t elapsed) noexcept;
  void DeliverAudio(std::size_t index, int samples, bool callback,
                    std::int64_t elapsed) noexcept;
  void Tick(bool video, Clock::time_point started, Clock::time_point deadline,
            std::int64_t interval_ns, std::int64_t elapsed,
            std::size_t video_queue, std::int64_t audio_queue) noexcept;
  void Finish(std::size_t video_queue, std::int64_t audio_queue) noexcept;

 private:
  struct TrackPerformance {
    PerformanceWindow window;
    std::uint64_t selected_new = 0;
    std::uint64_t repeated = 0;
    std::uint64_t late_old = 0;
    std::uint64_t capacity_cleared = 0;
    std::uint64_t capacity_rejected = 0;
    std::uint64_t late_ticks = 0;
    std::uint64_t skipped_ticks = 0;
    std::uint64_t underload_ticks = 0;
    // Ordinary FIFO removal includes waiting/pending windows; output copy
    // counts distinguish consumed source data from emitted source data.
    std::uint64_t consumed_source_samples = 0;
    std::uint64_t dropped_source_samples = 0;
    std::uint64_t late_source_samples = 0;
    std::uint64_t capacity_rejected_samples = 0;
    std::uint64_t delivered_source_samples = 0;
    std::uint64_t zero_fill_samples = 0;
    std::uint64_t external_callback_calls = 0;
    std::int64_t external_callback_ns = 0;
    std::int64_t max_external_callback_ns = 0;
  };
  void RecordDelivery(bool video, int samples, bool callback,
                      std::int64_t elapsed) noexcept;
  void Log(bool video, std::size_t video_queue, std::int64_t audio_queue,
           bool final = false) noexcept;
  bool enabled_ = false;
  bool video_ = false;
  bool audio_ = false;
  const void* instance_ = nullptr;
  AVRational video_frame_rate_{};
  int audio_sample_rate_ = 0;
  int audio_block_samples_ = 0;
  TrackPerformance video_performance_;
  TrackPerformance audio_performance_;
  std::int64_t copied_samples_ = 0;
  std::vector<std::int64_t> source_samples_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PERFORMANCE_PERFORMANCE_H_
