#ifndef MW_STREAMER_SCHEDULER_SCHEDULER_H_
#define MW_STREAMER_SCHEDULER_SCHEDULER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/performance/performance.h"
#include "mw/streamer/scheduler/scheduler_timing.h"

struct SwrContext;

namespace mw::streamer {

struct MW_STREAMER_API SchedulerConfig {
  AVRational video_frame_rate{30, 1};
  int audio_sample_rate = 48000;
  int audio_block_samples = 1024;
};

// One input is scheduled onto independent video and audio output threads.
// Set callbacks before Start; handle callback exceptions within the callback.
// Callbacks within a track are serial, and the two tracks may run concurrently.
// Serialize Start/Stop externally. Submit and Drain run serially on the input
// thread. Stop the upstream Input before stopping or destroying Scheduler.
class MW_STREAMER_API Scheduler final {
 public:
  // The reference is callback-scoped; retain with Frame::Ref() if needed.
  // Video retains its input buffers, including CUDA surfaces. Audio uses FLTP
  // at the configured rate and preserves the input channel layout.
  using OnFrame = std::function<void(const ffmpeg::Frame&)>;
  using OnEnded = std::function<void()>;

  explicit Scheduler(SchedulerConfig config = {}) noexcept;
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  void SetOnVideo(OnFrame callback) noexcept;
  void SetOnAudio(OnFrame callback) noexcept;
  void SetOnEnded(OnEnded callback) noexcept;

  // Starts a fresh session. A track absent from streams has no worker.
  // false means configuration is invalid, already started, or startup failed.
  // Stop must precede starting another session; callbacks are retained.
  bool Start(const std::vector<ffmpeg::StreamInfo>& streams) noexcept;
  bool SubmitVideo(const ffmpeg::Frame& frame) noexcept;
  bool SubmitAudio(const ffmpeg::Frame& frame) noexcept;
  // Finishes the current input, drains SWR and short PCM tails, then invokes
  // OnEnded once after both tracks and their frame callbacks finish.
  void Drain() noexcept;
  // Cancels and joins workers, waits for in-flight input and OnEnded calls,
  // then clears buffers. No callbacks run afterward until the next Start.
  // The external pipeline calls Stop, never a Scheduler callback.
  void Stop() noexcept;

 private:
  using Clock = std::chrono::steady_clock;

  struct AudioFrame {
    ffmpeg::Frame frame;
    std::int64_t start_sample;
    int offset = 0;
    int samples = 0;
  };

  bool TickVideo(Clock::time_point now);
  bool TickAudio(Clock::time_point start, Clock::time_point end);
  void RunVideo() noexcept;
  void RunAudio() noexcept;
  void WorkerStarted() noexcept;
  void FinishTrack(bool video) noexcept;
  void NotifyEnded() noexcept;
  void RequestStop() noexcept;
  void Join() noexcept;
  void Wake() noexcept;
  bool WaitUntil(Clock::time_point deadline, bool precise = false) noexcept;
  void DeliverVideo(const ffmpeg::Frame& frame, bool repeated) noexcept;
  void DeliverAudio(const ffmpeg::Frame& frame, std::size_t index) noexcept;
  std::int64_t OutputTime(Clock::time_point now) const noexcept;
  ffmpeg::Frame ResampleAudio(const ffmpeg::Frame& frame);
  bool QueueAudio(ffmpeg::Frame frame, bool append_tail = false);
  void PopAudio(std::int64_t samples, bool discarded = false) noexcept;
  void ResetAudio(std::int64_t timestamp) noexcept;
  bool DiscardStoppedAudio() noexcept;
  void FlushResampler();
  void Clear() noexcept;
  void RecordTick(bool video, Clock::time_point started,
                  Clock::time_point deadline,
                  std::int64_t interval_ns) noexcept;

  SchedulerConfig config_;
  OnFrame on_video_;
  OnFrame on_audio_;
  OnEnded on_ended_;
  std::mutex mutex_;
  std::condition_variable idle_;
  bool initialized_ = false;
  bool stopping_ = false;
  bool draining_ = false;
  bool video_done_ = true;
  bool audio_done_ = true;
  bool ended_notified_ = false;
  int audio_calls_ = 0;
  int callback_calls_ = 0;
  std::atomic<bool> stop_requested_{true};
  std::mutex wait_mutex_;
  std::condition_variable wake_;
  std::thread video_thread_;
  std::thread audio_thread_;
  int started_workers_ = 0;
  std::deque<ffmpeg::Frame> video_frames_;
  std::optional<ffmpeg::Frame> video_output_;
  bool video_timing_set_ = false;
  Clock::time_point video_system_;
  std::uint64_t video_media_ = 0;
  Clock::time_point video_end_system_;
  std::deque<AudioFrame> audio_frames_;
  std::optional<ffmpeg::Frame> audio_output_template_;
  internal::SourceAudioTiming audio_timing_;
  internal::AudioOutputTiming audio_output_timing_;
  std::int64_t audio_timestamp_ = 0;
  std::int64_t audio_sample_cursor_ = 0;
  std::int64_t audio_queued_samples_ = 0;
  std::int64_t audio_last_size_ = 0;
  bool audio_pending_stop_ = false;
  ::SwrContext* resampler_ = nullptr;
  AVSampleFormat resampler_format_ = AV_SAMPLE_FMT_NONE;
  int resampler_rate_ = 0;
  AVChannelLayout resampler_layout_{};
  std::int64_t resampler_next_pts_ = 0;
  std::int64_t resample_offset_ = 0;
  internal::SchedulerPerformance performance_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_SCHEDULER_H_
