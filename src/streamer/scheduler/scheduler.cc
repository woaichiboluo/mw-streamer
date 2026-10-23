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
  struct WakeState {
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t revision = 0;
    std::array<std::uint64_t, 2> output_space{};
  };

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
      tracks_[0].enabled = true;
    }
    if (target.audio_sample_format != AV_SAMPLE_FMT_NONE) {
      AudioResamplerConfig audio;
      audio.output_samples_per_frame = config.output_samples_per_frame;
      audio.sample_rate = target.audio_sample_rate;
      audio.sample_format = target.audio_sample_format;
      audio.channel_layout = target.audio_channel_layout;
      resampler_ = std::make_unique<AudioResampler>(audio);
      tracks_[1].enabled = true;
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
      std::lock_guard lock(wake_->mutex);
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
      std::weak_ptr<WakeState> weak = wake_;
      source_->SetOnReadReady([weak] {
        if (const auto wake = weak.lock()) {
          {
            std::lock_guard lock(wake->mutex);
            ++wake->revision;
          }
          wake->changed.notify_one();
        }
      });
      read_ready_installed_ = true;
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
    std::lock_guard lock(wake_->mutex);
    return error_;
  }
  void NotifyOutputSpace(bool video) noexcept {
    {
      std::lock_guard lock(wake_->mutex);
      ++wake_->output_space[video ? 0 : 1];
      ++wake_->revision;
    }
    wake_->changed.notify_one();
  }
  void RequestStop() noexcept {
    {
      std::lock_guard lock(wake_->mutex);
      stopping_ = true;
    }
    WakeAll();
  }
  void Stop() noexcept {
    RequestStop();
    if (read_ready_installed_) {
      source_->SetOnReadReady({});
      read_ready_installed_ = false;
    }
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
    // Shared jobs/completions are protected by wake_->mutex. Only the driver
    // owns the lookahead, sentinel head and deadline state.
    bool busy = false;
    bool requested = false;
    bool completed = false;
    AvDecoderReadResult task;
    AvDecoderReadResult result;
    bool needs_input = true;
    AvDecoderReadResult source_head;
    AvDecoderReadResult head;
    std::int64_t syncer_wake_us = 0;
    bool output_blocked = false;
    std::uint64_t blocked_revision = 0;
    void Clear() {
      busy = requested = completed = false;
      task = result = source_head = head = {};
      needs_input = true;
      syncer_wake_us = 0;
      output_blocked = false;
      blocked_revision = 0;
    }
  };

  void CheckStopped() const {
    if (started_) {
      throw std::logic_error("配置 Scheduler 回调前需要 Stop");
    }
  }
  bool stopping() const {
    std::lock_guard lock(wake_->mutex);
    return stopping_;
  }
  void WakeAll() {
    wake_->changed.notify_all();
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

  void Complete(std::size_t index) {
    auto& track = tracks_[index];
    std::lock_guard lock(wake_->mutex);
    if (!track.completed) return;
    track.head = std::move(track.result);
    track.result = {};
    track.completed = false;
    if (track.head.status != AvDecoderReadStatus::kNull)
      MW_LOG_TRACE(
          "streamer",
          "Scheduler[{}] 工作结果就绪: track={} status={} stream={} "
          "pts={} time_base={}/{}",
          fmt::ptr(this), index == 0 ? "video" : "audio",
          static_cast<int>(track.head.status), track.head.stream_index,
          track.head.frame ? track.head.frame->get()->pts : AV_NOPTS_VALUE,
          track.head.time_base.num, track.head.time_base.den);
    // A fresh head ends the source wait. Syncer retains its own hard pause
    // after a forward-fill batch, including when another track woke us.
    if (track.head.status != AvDecoderReadStatus::kNull)
      track.syncer_wake_us = Now();
  }
  void Dispatch(std::size_t index, AvDecoderReadResult input = {}) {
    {
      std::lock_guard lock(wake_->mutex);
      auto& track = tracks_[index];
      if (stopping_) return;
      track.task = std::move(input);
      track.busy = true;
      track.requested = true;
      MW_LOG_TRACE(
          "streamer",
          "Scheduler[{}] 派发处理: track={} status={} stream={} pts={}",
          fmt::ptr(this), index == 0 ? "video" : "audio",
          static_cast<int>(track.task.status), track.task.stream_index,
          track.task.frame ? track.task.frame->get()->pts : AV_NOPTS_VALUE);
    }
    work_ready_[index].notify_one();
  }

  std::int64_t Prepare(std::size_t index, std::int64_t now) {
    auto& track = tracks_[index];
    {
      std::lock_guard lock(wake_->mutex);
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
        // Frame readiness wakes the driver; no periodic empty-source reads.
        return kNoWake;
      }
    }
    if (track.source_head.status == AvDecoderReadStatus::kFrame) {
      const auto pts_us = InputPts(track.source_head);
      const auto decision = realtime_.Schedule(pts_us, now);
      MW_LOG_TRACE("streamer",
                   "Scheduler[{}] 实时调度决策: track={} pts_us={} now_us={} "
                   "ready={} discard={} wake_us={}",
                   fmt::ptr(this), index == 0 ? "video" : "audio", pts_us, now,
                   decision.ready, decision.discard, decision.wake_us);
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

  bool DrainDisabled(std::size_t index) {
    for (int count = 0; count < kDiscardBatch; ++count) {
      if (Read(index).status == AvDecoderReadStatus::kNull) return false;
    }
    return true;
  }

  void Deliver(std::size_t index, std::int64_t now) {
    auto& track = tracks_[index];
    if (now < track.syncer_wake_us) return;
    std::uint64_t observed_space;
    {
      std::lock_guard lock(wake_->mutex);
      observed_space = wake_->output_space[index];
      if (track.output_blocked && observed_space == track.blocked_revision)
        return;
    }
    const bool retrying = track.output_blocked;
    track.output_blocked = false;
    const auto* head =
        track.head.status == AvDecoderReadStatus::kNull ? nullptr : &track.head;
    const auto& output = index == 0 ? on_video_frame_ : on_audio_frame_;
    auto step = syncer_.Step(index == 0, head, now, output);
    if (head || step.frame || step.blocked)
      MW_LOG_TRACE("streamer",
                   "Scheduler[{}] 同步交付结果: track={} now_us={} "
                   "has_input={} consumed={} output_pts={} blocked={} "
                   "wake_us={}",
                   fmt::ptr(this), index == 0 ? "video" : "audio", now,
                   head != nullptr, step.consumed,
                   step.frame ? step.frame->get()->pts : AV_NOPTS_VALUE,
                   step.blocked, step.wake_us);
    if (step.consumed) track.head = {};
    track.syncer_wake_us = step.wake_us;
    if (step.blocked) {
      if (!retrying)
        MW_LOG_DEBUG("streamer", "Scheduler 下游背压，暂停交付: track={}",
                     index == 0 ? "video" : "audio");
      track.output_blocked = true;
      track.blocked_revision = observed_space;
    } else if (retrying && step.frame) {
      MW_LOG_DEBUG("streamer", "Scheduler 下游恢复接收: track={}",
                   index == 0 ? "video" : "audio");
    }
  }

  void RunDriver() noexcept {
    try {
      while (!stopping()) {
        std::uint64_t observed;
        {
          std::lock_guard lock(wake_->mutex);
          observed = wake_->revision;
        }
        CheckSource();
        auto wake = kNoWake;
        for (std::size_t index = 0; index < tracks_.size(); ++index) {
          if (tracks_[index].enabled) {
            Complete(index);
            Deliver(index, Now());
          } else if (DrainDisabled(index)) {
            wake = Now();
          }
          if (stopping()) return;
        }
        for (std::size_t index = 0; index < tracks_.size(); ++index) {
          if (!tracks_[index].enabled) continue;
          wake = std::min(wake, Prepare(index, Now()));
          bool blocked;
          {
            std::lock_guard lock(wake_->mutex);
            const auto& track = tracks_[index];
            blocked = track.output_blocked &&
                      track.blocked_revision == wake_->output_space[index];
          }
          if (!blocked) wake = std::min(wake, tracks_[index].syncer_wake_us);
        }
        std::unique_lock lock(wake_->mutex);
        const auto changed = [&] {
          return stopping_ || wake_->revision != observed;
        };
        if (wake == kNoWake) {
          wake_->changed.wait(lock, changed);
        } else {
          wake_->changed.wait_until(lock, epoch_ + Microseconds(wake), changed);
        }
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
        std::unique_lock lock(wake_->mutex);
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
                             : std::move(input);
      }
      if (pending.status != AvDecoderReadStatus::kNull &&
          converter.TryPushFrame(pending)) {
        pending = {};
      }
      auto output = converter.TryReadFrame();
      if (index == 1 && output.status != AvDecoderReadStatus::kNull)
        output = filter_.ProcessAudio(std::move(output));
      {
        std::lock_guard lock(wake_->mutex);
        auto& track = tracks_[index];
        track.needs_input = pending.status == AvDecoderReadStatus::kNull &&
                            output.status == AvDecoderReadStatus::kNull;
        track.result = std::move(output);
        track.busy = false;
        track.completed = true;
        ++wake_->revision;
      }
      wake_->changed.notify_one();
    }
  }
  void RunWorker(std::size_t index) noexcept {
    try {
      if (index == 0 && force_fps_) {
        Process(index, *force_fps_);
      } else if (index == 1 && resampler_) {
        Process(index, *resampler_);
      } else {
        std::unique_lock lock(wake_->mutex);
        work_ready_[index].wait(lock, [this] { return stopping_; });
      }
    } catch (...) {
      // Another track may observe the filter's rejected session before the
      // throwing track reaches this handler. Keep the original business error.
      const auto filter_failure = filter_.error();
      Fail(filter_failure ? filter_failure : std::current_exception());
    }
  }
  void Fail(std::exception_ptr failure) noexcept {
    {
      std::lock_guard lock(wake_->mutex);
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
  std::shared_ptr<WakeState> wake_ = std::make_shared<WakeState>();
  std::array<std::condition_variable, 2> work_ready_;
  bool stopping_ = true;
  bool started_ = false;
  bool read_ready_installed_ = false;
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
void Scheduler::NotifyOutputSpace(bool video) noexcept {
  impl_->NotifyOutputSpace(video);
}
void Scheduler::RequestStop() noexcept { impl_->RequestStop(); }
void Scheduler::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
