#include "mw/streamer/scheduler/scheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/filter/audio_resampler.h"
#include "mw/streamer/filter/force_fps.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;
constexpr AVRational kMicroseconds{1, 1000000};
constexpr auto kNoWake = std::numeric_limits<std::int64_t>::max();
constexpr int kDiscardBatch = 8;

std::int64_t InputPts(const AvDecoderReadResult& input) {
  if (!input.frame || input.time_base.num <= 0 || input.time_base.den <= 0 ||
      input.frame->get()->pts == AV_NOPTS_VALUE) {
    throw std::invalid_argument("Scheduler 输入需要有效的帧和时间戳");
  }
  const auto pts =
      av_rescale_q(input.frame->get()->pts, input.time_base, kMicroseconds);
  if (pts == AV_NOPTS_VALUE || pts == kNoWake) {
    throw std::overflow_error("Scheduler 输入时间戳溢出");
  }
  return pts;
}

}  // namespace

class Scheduler::Impl final {
 public:
  Impl(std::shared_ptr<AvMediaSource> source, SchedulerConfig config)
      : source_(std::move(source)),
        syncer_(config.syncer),
        realtime_(config.realtime) {
    if (!source_) {
      throw std::invalid_argument("Scheduler 需要有效的 MediaSource");
    }
    const auto& target = config.syncer.encoder;
    if (target.video_pixel_format != AV_PIX_FMT_NONE) {
      ForceFpsConfig fps;
      fps.fps = target.fps;
      force_fps_ = std::make_unique<ForceFps>(fps);
      tracks_[0].time_base = av_inv_q(target.fps);
      tracks_[0].period = 1;
      tracks_[0].enabled = true;
    }
    if (target.audio_sample_format != AV_SAMPLE_FMT_NONE) {
      AudioResamplerConfig audio;
      audio.sample_rate = target.audio_sample_rate;
      audio.sample_format = target.audio_sample_format;
      audio.channel_layout = target.audio_channel_layout;
      resampler_ = std::make_unique<AudioResampler>(audio);
      tracks_[1].time_base = {1, target.audio_sample_rate};
      tracks_[1].period = 1024;
      tracks_[1].enabled = true;
    }
    // Drain independently of output FPS so a disabled decoder cannot block
    // MediaSource's shared dispatcher when the enabled video FPS is low.
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
      if (!tracks_[index].enabled) {
        tracks_[index].time_base = kMicroseconds;
        tracks_[index].period = 22000;
      }
    }
  }

  ~Impl() { Stop(); }
  AvMediaFilter& media_filter() { return filter_; }
  void SetOnVideoFrame(OnVideoFrame callback) {
    CheckStopped();
    on_video_frame_ = std::move(callback);
  }
  void SetOnAudioFrame(OnAudioFrame callback) {
    CheckStopped();
    on_audio_frame_ = std::move(callback);
  }
  void SetOnError(OnError callback) {
    CheckStopped();
    on_error_ = std::move(callback);
  }

  bool Start() {
    if (started_) return false;
    {
      std::lock_guard lock(mutex_);
      error_ = {};
      stopping_ = false;
      for (auto& track : tracks_) track.Clear();
    }
    started_ = true;
    try {
      syncer_.Reset();
      realtime_.Reset();
      if (force_fps_) force_fps_->Reset();
      if (resampler_) resampler_->Reset();
      filter_.Start();
      epoch_ = Clock::now();
      if (!video_worker_.Start([this] { RunWorker(0); }) ||
          !audio_worker_.Start([this] { RunWorker(1); }) ||
          !driver_.Start([this] { RunDriver(); })) {
        throw std::runtime_error("Scheduler 创建工作线程失败");
      }
      MW_LOG_INFO("streamer", "Scheduler 实时驱动已启动: video={} audio={}",
                  tracks_[0].enabled, tracks_[1].enabled);
      return true;
    } catch (...) {
      Fail(std::current_exception());
      Stop();
      return false;
    }
  }

  std::exception_ptr error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }
  void RequestStop() noexcept {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    WakeAll();
  }
  void Stop() noexcept {
    RequestStop();
    driver_.Join();
    video_worker_.Join();
    audio_worker_.Join();
    if (force_fps_) force_fps_->Stop();
    if (resampler_) resampler_->Stop();
    try {
      filter_.Stop();
    } catch (...) {
      Fail(std::current_exception());
    }
    for (auto& track : tracks_) track.Clear();
    if (started_) MW_LOG_INFO("streamer", "Scheduler 已停止");
    started_ = false;
  }

 private:
  struct Track {
    bool enabled = false;
    AVRational time_base{0, 1};
    std::int64_t period = 0;
    // Shared jobs/completions are protected by mutex_. Only the driver owns
    // the lookahead, sentinel head and deadline state.
    bool busy = false;
    bool requested = false;
    bool completed = false;
    AvDecoderReadResult task;
    AvDecoderReadResult result;
    bool needs_input = true;
    AvDecoderReadResult source_head;
    AvDecoderReadResult head;
    std::int64_t next_output_us = 0;
    std::int64_t output_units = 0;
    std::int64_t output_epoch_us = 0;
    bool has_output = false;
    void Clear() {
      busy = requested = completed = false;
      task = result = source_head = head = {};
      needs_input = true;
      next_output_us = output_units = 0;
      output_epoch_us = 0;
      has_output = false;
    }
  };

  void CheckStopped() const {
    if (started_) {
      throw std::logic_error("配置 Scheduler 回调前需要 Stop");
    }
  }
  bool stopping() const {
    std::lock_guard lock(mutex_);
    return stopping_;
  }
  void WakeAll() {
    changed_.notify_all();
    for (auto& ready : work_ready_) ready.notify_all();
  }
  std::int64_t Now() const {
    return std::chrono::duration_cast<Microseconds>(Clock::now() - epoch_)
        .count();
  }
  void CheckSource() const {
    const auto status = source_->status();
    if (status.state == AvMediaSourceState::kFailed) {
      std::rethrow_exception(status.error);
    }
  }
  AvDecoderReadResult Read(std::size_t index) {
    return index == 0 ? source_->TryReadVideo() : source_->TryReadAudio();
  }

  // Absolute rational positions prevent fractional FPS/sample rounding drift.
  // Skip missed wall-clock slots after a stall instead of bursting callbacks.
  void Advance(Track& track, std::int64_t duration) {
    track.output_units += duration;
    track.next_output_us =
        track.output_epoch_us +
        av_rescale_q(track.output_units, track.time_base, kMicroseconds);
    const auto now = Now();
    if (track.next_output_us <= now) {
      const auto overdue_units =
          av_rescale_q_rnd(now - track.next_output_us, kMicroseconds,
                           track.time_base, AV_ROUND_DOWN);
      track.output_units += (overdue_units / duration + 1) * duration;
      track.next_output_us =
          track.output_epoch_us +
          av_rescale_q(track.output_units, track.time_base, kMicroseconds);
    }
  }

  void Complete(std::size_t index) {
    auto& track = tracks_[index];
    std::lock_guard lock(mutex_);
    if (!track.completed) return;
    track.head = std::move(track.result);
    track.result = {};
    track.completed = false;
    if (!track.has_output && track.head.status != AvDecoderReadStatus::kNull) {
      track.next_output_us = Now();
    }
  }
  void Dispatch(std::size_t index, AvDecoderReadResult input = {}) {
    {
      std::lock_guard lock(mutex_);
      auto& track = tracks_[index];
      if (stopping_) return;
      track.task = std::move(input);
      track.busy = true;
      track.requested = true;
    }
    work_ready_[index].notify_one();
  }

  std::int64_t Prepare(std::size_t index, std::int64_t now) {
    auto& track = tracks_[index];
    {
      std::lock_guard lock(mutex_);
      if (track.busy || track.completed) return kNoWake;
    }
    if (track.head.status != AvDecoderReadStatus::kNull) return kNoWake;
    if (!track.needs_input) {
      Dispatch(index);
      return kNoWake;
    }
    if (track.source_head.status == AvDecoderReadStatus::kNull) {
      track.source_head = Read(index);
      if (track.source_head.status == AvDecoderReadStatus::kNull) {
        // No retry timer for an empty source: the next real output deadline
        // or worker completion will drive another nonblocking read.
        return kNoWake;
      }
    }
    if (track.source_head.status == AvDecoderReadStatus::kFrame) {
      const auto decision =
          realtime_.Schedule(InputPts(track.source_head), now);
      if (decision.discard) {
        track.source_head = {};
        return now;
      }
      if (!decision.ready) return decision.wake_us;
    }
    Dispatch(index, std::move(track.source_head));
    track.source_head = {};
    return kNoWake;
  }

  void Deliver(std::size_t index, std::int64_t now) {
    auto& track = tracks_[index];
    if (now < track.next_output_us) return;
    if (!track.enabled) {
      for (int count = 0; count < kDiscardBatch; ++count) {
        if (Read(index).status == AvDecoderReadStatus::kNull) {
          Advance(track, track.period);
          return;
        }
      }
      // Yield between full batches without limiting drain throughput to FPS.
      track.next_output_us = now;
      return;
    }
    const auto* head =
        track.head.status == AvDecoderReadStatus::kNull ? nullptr : &track.head;
    auto step = syncer_.Step(index == 0, head, now);
    if (step.consumed) track.head = {};
    if (!step.frame) {
      // EOF is a control event rather than an output slot. Its successor may
      // be processed by the next driver iteration without delaying media.
      if (!step.consumed) {
        if (head) {
          track.next_output_us = step.wake_us;
        } else {
          Advance(track, track.period);
        }
      }
      return;
    }
    const auto duration =
        index == 0 ? std::int64_t{1} : (*step.frame)->nb_samples;
    if (!track.has_output) {
      track.output_epoch_us = now;
      track.output_units = 0;
      track.has_output = true;
    }
    if (index == 0) {
      if (on_video_frame_) on_video_frame_(std::move(*step.frame));
    } else {
      if (on_audio_frame_) on_audio_frame_(std::move(*step.frame));
    }
    Advance(track, duration);
  }

  void RunDriver() noexcept {
    try {
      while (!stopping()) {
        std::uint64_t observed;
        {
          std::lock_guard lock(mutex_);
          observed = revision_;
        }
        CheckSource();
        for (std::size_t index = 0; index < tracks_.size(); ++index) {
          Complete(index);
          Deliver(index, Now());
          if (stopping()) return;
        }
        auto wake = kNoWake;
        for (std::size_t index = 0; index < tracks_.size(); ++index) {
          if (tracks_[index].enabled) {
            wake = std::min(wake, Prepare(index, Now()));
          }
          wake = std::min(wake, tracks_[index].next_output_us);
        }
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, epoch_ + Microseconds(wake),
                            [&] { return stopping_ || revision_ != observed; });
      }
    } catch (...) {
      Fail(std::current_exception());
    }
  }

  template <typename Converter>
  void Process(std::size_t index, Converter& converter) {
    AvDecoderReadResult pending;
    while (true) {
      AvDecoderReadResult input;
      {
        std::unique_lock lock(mutex_);
        auto& track = tracks_[index];
        work_ready_[index].wait(lock,
                                [&] { return stopping_ || track.requested; });
        if (stopping_) return;
        input = std::move(track.task);
        track.task = {};
        track.requested = false;
      }
      if (input.status != AvDecoderReadStatus::kNull) {
        pending = index == 0 ? filter_.ProcessVideo(std::move(input))
                             : filter_.ProcessAudio(std::move(input));
      }
      if (pending.status != AvDecoderReadStatus::kNull &&
          converter.TryPushFrame(pending)) {
        pending = {};
      }
      auto output = converter.TryReadFrame();
      {
        std::lock_guard lock(mutex_);
        auto& track = tracks_[index];
        track.needs_input = pending.status == AvDecoderReadStatus::kNull &&
                            output.status == AvDecoderReadStatus::kNull;
        track.result = std::move(output);
        track.busy = false;
        track.completed = true;
        ++revision_;
      }
      changed_.notify_one();
    }
  }
  void RunWorker(std::size_t index) noexcept {
    try {
      if (index == 0 && force_fps_) {
        Process(index, *force_fps_);
      } else if (index == 1 && resampler_) {
        Process(index, *resampler_);
      } else {
        std::unique_lock lock(mutex_);
        work_ready_[index].wait(lock, [this] { return stopping_; });
      }
    } catch (...) {
      Fail(std::current_exception());
    }
  }
  void Fail(std::exception_ptr failure) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (error_) return;
      error_ = failure;
      stopping_ = true;
    }
    WakeAll();
    try {
      std::rethrow_exception(failure);
    } catch (const std::exception& exception) {
      MW_LOG_ERROR("streamer", "Scheduler 不可恢复错误: {}", exception.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "Scheduler 发生未知不可恢复错误");
    }
    if (on_error_) {
      try {
        on_error_(failure);
      } catch (const std::exception& exception) {
        MW_LOG_ERROR("streamer", "Scheduler 错误通知异常: {}",
                     exception.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "Scheduler 错误通知发生未知异常");
      }
    }
  }

  std::shared_ptr<AvMediaSource> source_;
  Syncer syncer_;
  internal::RealtimeTiming realtime_;
  AvMediaFilter filter_;
  std::unique_ptr<ForceFps> force_fps_;
  std::unique_ptr<AudioResampler> resampler_;
  std::array<Track, 2> tracks_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::array<std::condition_variable, 2> work_ready_;
  bool stopping_ = true;
  bool started_ = false;
  std::uint64_t revision_ = 0;
  std::exception_ptr error_;
  Clock::time_point epoch_;
  OnVideoFrame on_video_frame_;
  OnAudioFrame on_audio_frame_;
  OnError on_error_;
  internal::Thread video_worker_;
  internal::Thread audio_worker_;
  internal::Thread driver_;
};

Scheduler::Scheduler(std::shared_ptr<AvMediaSource> source,
                     SchedulerConfig config)
    : impl_(std::make_unique<Impl>(std::move(source), std::move(config))) {}
Scheduler::~Scheduler() = default;
AvMediaFilter& Scheduler::media_filter() { return impl_->media_filter(); }
void Scheduler::SetOnVideoFrame(OnVideoFrame callback) {
  impl_->SetOnVideoFrame(std::move(callback));
}
void Scheduler::SetOnAudioFrame(OnAudioFrame callback) {
  impl_->SetOnAudioFrame(std::move(callback));
}
void Scheduler::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
bool Scheduler::Start() { return impl_->Start(); }
std::exception_ptr Scheduler::error() const { return impl_->error(); }
void Scheduler::RequestStop() noexcept { impl_->RequestStop(); }
void Scheduler::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
