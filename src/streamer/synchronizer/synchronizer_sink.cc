#include "mw/streamer/synchronizer/synchronizer_sink.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mw/log.h"
#include "mw/streamer/common/blocking_queue.h"
#include "mw/streamer/common/thread.h"
#include "mw/streamer/performance/operation_recorder.h"
#include "mw/streamer/synchronizer/internal/realtime_frame_scheduler.h"

namespace mw::streamer {

class SynchronizerSink::Impl final {
 public:
  Impl(SynchronizerSink& owner, SynchronizerSinkConfig config)
      : owner_(owner),
        config_(std::move(config)),
        outputs_(owner.downstream()) {
    if (config_.frame_queue_capacity == 0 ||
        config_.max_frame_lateness_ms < std::chrono::milliseconds::zero() ||
        config_.standby_timeout_ms < std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("SynchronizerSink容量或等待时间无效");
    }
    scheduler_ = std::make_unique<Scheduler>(config_);
  }

  ~Impl() { Stop(); }

  void OnStreamsReady(const FrameStreamsReady& streams) {
    Submit([&]() {
      if (streams.generation == 0 || streams.source_streams.empty()) {
        throw std::invalid_argument("SynchronizerSink代次和轨道不能为空");
      }
      if (streams.generation < input_generation_) return;
      if (source_ready_ ||
          (input_generation_ != 0 &&
           (!pending_reset_ || streams.generation != input_generation_))) {
        throw std::logic_error("SynchronizerSink新代次必须先重置时间线");
      }
      ValidateTracks(streams);
      input_generation_ = streams.generation;
      pending_reset_ = false;
      source_ready_ = true;
      input_ended_ = false;
      Work work;
      work.kind = Kind::kReady;
      work.streams = streams;
      queue_.Push(std::move(work));
    });
  }

  void OnFrame(const FrameReady& frame, bool audio) {
    Submit([&]() {
      if (!CurrentGeneration(frame.generation)) return;
      if (!(audio ? has_audio_ : has_video_) || !frame.frame.get() ||
          frame.frame->pts == AV_NOPTS_VALUE ||
          frame.frame->time_base.num <= 0 || frame.frame->time_base.den <= 0) {
        throw std::invalid_argument("SynchronizerSink帧轨道或时间戳无效");
      }
      Work work;
      work.kind = audio ? Kind::kAudio : Kind::kVideo;
      work.frame = frame;
      const auto same_track = [kind = work.kind](const Work& queued) {
        return queued.kind == kind;
      };
      bool replaced = false;
      if (!queue_.PushReplacingOldest(std::move(work),
                                      config_.frame_queue_capacity, same_track,
                                      same_track, &replaced)) {
        throw std::runtime_error("SynchronizerSink无法投递输入帧");
      }
      auto& dropped = audio ? dropped_audio_frames_ : dropped_video_frames_;
      const char* media = audio ? "audio" : "video";
      if (replaced) {
        ++dropped;
        if (dropped == 1) {
          MW_LOG_WARNING("sync",
                         "同步器输入队列已满，开始丢弃同轨最旧帧: media={}, "
                         "generation={}, capacity={}, pts={}",
                         media, frame.generation, config_.frame_queue_capacity,
                         frame.frame->pts);
        }
      } else if (dropped != 0) {
        MW_LOG_INFO("sync",
                    "同步器输入队列恢复: media={}, generation={}, "
                    "本轮丢弃帧数={}",
                    media, frame.generation, dropped);
        dropped = 0;
      }
    });
  }

  void OnTimelineReset(const TimelineReset& reset) {
    Submit([&]() {
      if (final_input_end_) {
        throw std::logic_error("SynchronizerSink最终结束后不能重置输入");
      }
      if (reset.generation == 0) {
        throw std::invalid_argument("SynchronizerSink重置代次必须大于零");
      }
      if (reset.generation <= input_generation_) return;
      FinishIngressDrops("timeline_reset");
      queue_.EraseIf(IsFrame);
      input_generation_ = reset.generation;
      pending_reset_ = true;
      source_ready_ = false;
      Work work;
      work.kind = Kind::kReset;
      queue_.Push(std::move(work));
    });
  }

  void OnInputEnded(const StreamEnded& end) {
    Submit([&]() {
      if (end.generation < input_generation_ || input_ended_) return;
      if (!CurrentGeneration(end.generation)) return;
      input_ended_ = true;
      final_input_end_ = end.reason != StreamEndReason::kInterrupted;
      FinishIngressDrops("input_end");
      if (end.reason != StreamEndReason::kEof) queue_.EraseIf(IsFrame);
      Work work;
      work.kind = Kind::kEnd;
      work.end = end;
      queue_.Push(std::move(work));
    });
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    if (stopped_) return;
    stopping_.store(true);
    queue_.Close();
    {
      std::lock_guard<std::mutex> input_lock(input_mutex_);
      FinishIngressDrops("stop");
      queue_.Clear();
    }
    if (worker_) worker_->Join();
    owner_.StopDownstream();
    // Children can retain CUDA frame buffers until their own workers exit.
    scheduler_.reset();
    cached_frames_.store(0);
    SetState(SynchronizerSinkState::kStopped);
    stopped_ = true;
  }

  SynchronizerSinkState state() const noexcept { return state_.load(); }

  std::string error() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return error_;
  }

  std::size_t queue_depth() const {
    return queue_.size() + cached_frames_.load();
  }

  NodeSnapshot GetOwnPerformance() const {
    NodeSnapshot result;
    result.name = "SynchronizerSink";
    result.operations.push_back(performance_.GetSnapshot());
    return result;
  }

  void HandleFatalError(const std::string& error) noexcept {
    Fail(error, true);
  }

 private:
  using Scheduler = internal::RealtimeFrameScheduler;
  enum class Kind { kReady, kAudio, kVideo, kReset, kEnd };
  struct Work {
    Kind kind = Kind::kReady;
    std::optional<FrameStreamsReady> streams;
    std::optional<FrameReady> frame;
    std::optional<StreamEnded> end;
  };

  static bool IsFrame(const Work& work) {
    return work.kind == Kind::kAudio || work.kind == Kind::kVideo;
  }

  void FinishIngressDrops(const char* reason) {
    const auto finish = [&](const char* media, std::size_t& dropped) {
      if (dropped == 0) return;
      MW_LOG_INFO("sync",
                  "同步器输入队列丢帧阶段结束: media={}, generation={}, "
                  "reason={}, 本轮丢弃帧数={}",
                  media, input_generation_, reason, dropped);
      dropped = 0;
    };
    finish("audio", dropped_audio_frames_);
    finish("video", dropped_video_frames_);
  }

  template <typename Action>
  void Submit(Action action) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (stopping_.load() || failed_.load() || queue_.closed()) {
      throw std::logic_error("SynchronizerSink已结束、失败或停止");
    }
    try {
      if (outputs_.empty()) {
        throw std::logic_error("SynchronizerSink至少需要一个下游");
      }
      action();
      if (!worker_) {
        worker_ =
            std::make_unique<Thread>("mw-synchronize", [this]() { Run(); });
      }
    } catch (const std::exception& error) {
      // Preserve synchronous diagnostics and stop any already-running worker.
      Fail(error.what());
      throw;
    } catch (...) {
      Fail("SynchronizerSink输入通知发生未知异常");
      throw;
    }
  }

  void ValidateTracks(const FrameStreamsReady& streams) {
    std::optional<int> audio;
    std::optional<int> video;
    for (const auto& stream : streams.source_streams) {
      stream.Validate();
      const auto type = stream.codec_parameters.get()->codec_type;
      if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) {
        throw std::invalid_argument("SynchronizerSink仅支持音视频轨道");
      }
      auto& index = type == AVMEDIA_TYPE_AUDIO ? audio : video;
      if (index) {
        throw std::invalid_argument("SynchronizerSink每种媒体仅支持一路轨道");
      }
      index = stream.stream_index;
    }
    if (audio && video && *audio == *video) {
      throw std::invalid_argument("SynchronizerSink轨道索引重复");
    }
    has_audio_ = audio.has_value();
    has_video_ = video.has_value();
  }

  bool CurrentGeneration(std::uint64_t generation) const {
    if (generation < input_generation_) return false;
    if (!source_ready_ || input_ended_ || generation != input_generation_) {
      throw std::logic_error("SynchronizerSink收到未就绪代次的数据");
    }
    return true;
  }

  bool CanRun() const { return !stopping_.load() && !failed_.load(); }

  void Run() noexcept {
    try {
      while (CanRun() && !output_ended_) {
        const auto deadline = poll_again_
                                  ? std::optional{Scheduler::Clock::now()}
                                  : scheduler_->deadline();
        auto work =
            deadline ? queue_.WaitPopUntil(*deadline) : queue_.WaitPop();
        if (!CanRun() || (!work && queue_.closed())) break;
        if (work) Process(*work);
        if (!CanRun() || output_ended_) break;
        DrainReadyFrames();
        cached_frames_.store(scheduler_->queue_depth());
        if (finishing_ && scheduler_->finished()) {
          NotifyEnd(StreamEndReason::kEof);
          SetState(SynchronizerSinkState::kEnded);
          queue_.Close();
        } else {
          const bool standby = scheduler_->standby();
          if (!last_standby_ || *last_standby_ != standby) {
            if (standby) {
              MW_LOG_WARNING(
                  "sync",
                  "同步器进入备播: generation={}, scheduled_queue_depth={}",
                  output_generation_, scheduler_->queue_depth());
            } else {
              MW_LOG_INFO(
                  "sync",
                  "同步器恢复真实画面: generation={}, scheduled_queue_depth={}",
                  output_generation_, scheduler_->queue_depth());
            }
            last_standby_ = standby;
          }
          SetState(finishing_ ? SynchronizerSinkState::kDraining
                              : (standby ? SynchronizerSinkState::kStandby
                                         : SynchronizerSinkState::kRunning));
        }
      }
    } catch (const FatalError& error) {
      Fail(error.what(), true);
    } catch (const std::exception& error) {
      Fail(error.what());
    } catch (...) {
      Fail("SynchronizerSink调度发生未知异常");
    }
    if (failed_.load() && !output_ended_) {
      try {
        NotifyEnd(StreamEndReason::kFailed);
      } catch (const FatalError& error) {
        Fail(error.what(), true);
      } catch (...) {
        // The first diagnostic is retained; Stop still releases every child.
      }
    }
    queue_.Clear();
  }

  void Process(const Work& work) {
    switch (work.kind) {
      case Kind::kReady:
        scheduler_->Configure(*work.streams);
        owner_.StartMessages();
        if (!output_ready_) {
          output_generation_ = work.streams->generation;
          output_ready_ = true;
          for (auto& output : outputs_) {
            if (!CanRun()) return;
            ++ready_outputs_;
            output->OnStreamsReady(*work.streams);
          }
        }
        return;
      case Kind::kAudio:
      case Kind::kVideo: {
        performance_.AddInput(1);
        OperationRecorder::Call call(performance_);
        scheduler_->Push(*work.frame, work.kind == Kind::kAudio,
                         Scheduler::Clock::now());
        return;
      }
      case Kind::kReset:
        scheduler_->Reset();
        return;
      case Kind::kEnd:
        End(work.end->reason);
        return;
    }
  }

  void DrainReadyFrames() {
    constexpr int kOutputBatchSize = 8;
    poll_again_ = false;
    for (int count = 0; count < kOutputBatchSize && CanRun(); ++count) {
      OperationRecorder::Call call(performance_);
      auto frame = scheduler_->TakeReady(Scheduler::Clock::now());
      call.Finish();
      if (!frame) return;
      performance_.AddOutput(1);
      Forward(*frame);
    }
    // A persistently slow child can keep every next slot overdue. Revisit
    // input/control traffic before continuing, even without a future deadline.
    poll_again_ = true;
  }

  void End(StreamEndReason reason) {
    if (reason == StreamEndReason::kInterrupted) {
      scheduler_->Reset();
    } else if (reason == StreamEndReason::kEof) {
      scheduler_->Finish();
      finishing_ = true;
    } else {
      NotifyEnd(reason);
      if (reason == StreamEndReason::kFailed) {
        Fail("SynchronizerSink上游输入失败");
      } else {
        SetState(SynchronizerSinkState::kStopped);
      }
      queue_.Close();
    }
  }

  void Forward(const Scheduler::OutputFrame& frame) {
    const FrameReady ready{output_generation_, frame.frame};
    for (auto& output : outputs_) {
      if (!CanRun()) return;
      if (frame.audio) {
        output->OnAudioFrame(ready);
      } else {
        output->OnVideoFrame(ready);
      }
    }
  }

  void NotifyEnd(StreamEndReason reason) {
    if (!output_ready_ || output_ended_) return;
    output_ended_ = true;
    std::exception_ptr first_error;
    for (std::size_t index = 0; index < ready_outputs_; ++index) {
      try {
        outputs_[index]->OnInputEnded({output_generation_, reason});
      } catch (const FatalError& error) {
        owner_.ReportFatalError(error.what());
        if (!first_error) first_error = std::current_exception();
      } catch (...) {
        if (!first_error) first_error = std::current_exception();
      }
    }
    if (first_error) std::rethrow_exception(first_error);
  }

  void SetState(SynchronizerSinkState state) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (!failed_.load()) state_.store(state);
  }

  void Fail(const std::string& error, bool fatal = false) noexcept {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (!failed_.exchange(true)) {
        error_ = error;
        state_.store(SynchronizerSinkState::kFailed);
        MW_LOG_ERROR("streamer", "SynchronizerSink失败: {}", error);
      }
    }
    queue_.Close();
    queue_.Clear();
    if (fatal) owner_.ReportFatalError(error);
  }

  SynchronizerSink& owner_;
  const SynchronizerSinkConfig config_;
  OperationRecorder performance_{PerformanceType::kSynchronizer,
                                 PerformanceUnit::kFrame,
                                 PerformanceUnit::kFrame};
  const std::vector<std::unique_ptr<Sink>>& outputs_;
  BlockingQueue<Work> queue_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<Thread> worker_;
  std::mutex input_mutex_;
  std::size_t dropped_audio_frames_ = 0;
  std::size_t dropped_video_frames_ = 0;
  std::mutex stop_mutex_;
  mutable std::mutex status_mutex_;
  std::string error_;
  std::atomic<SynchronizerSinkState> state_{SynchronizerSinkState::kIdle};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> failed_{false};
  std::atomic<std::size_t> cached_frames_{0};
  // Admission state is protected by input_mutex_; outputs are immutable after
  // setup. Scheduler/output fields below are worker-owned until Join.
  bool stopped_ = false;
  std::uint64_t input_generation_ = 0;
  bool source_ready_ = false;
  bool pending_reset_ = false;
  bool input_ended_ = false;
  bool final_input_end_ = false;
  bool has_audio_ = false;
  bool has_video_ = false;
  bool output_ready_ = false;
  std::size_t ready_outputs_ = 0;
  bool output_ended_ = false;
  bool finishing_ = false;
  bool poll_again_ = false;
  std::uint64_t output_generation_ = 0;
  std::optional<bool> last_standby_;
};

SynchronizerSink::SynchronizerSink(std::string id,
                                   SynchronizerSinkConfig config)
    : Sink(std::move(id), SinkMediaType::kFrame, SinkMediaType::kFrame),
      impl_(std::make_unique<Impl>(*this, std::move(config))) {}
SynchronizerSink::~SynchronizerSink() { Stop(); }
void SynchronizerSink::OnStreamsReady(const FrameStreamsReady& streams) {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}
void SynchronizerSink::OnAudioFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnFrame(frame, true);
}
void SynchronizerSink::OnVideoFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnFrame(frame, false);
}
void SynchronizerSink::OnTimelineReset(const TimelineReset& reset) {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}
void SynchronizerSink::OnInputEnded(const StreamEnded& end) {
  CloseRegistration();
  impl_->OnInputEnded(end);
}
void SynchronizerSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}
SynchronizerSinkState SynchronizerSink::state() const noexcept {
  return impl_->state();
}
std::string SynchronizerSink::error() const { return impl_->error(); }
std::size_t SynchronizerSink::queue_depth() const {
  return impl_->queue_depth();
}
NodeSnapshot SynchronizerSink::GetOwnPerformance() const {
  return impl_->GetOwnPerformance();
}

void SynchronizerSink::HandleFatalError(const std::string& error) noexcept {
  impl_->HandleFatalError(error);
}

}  // namespace mw::streamer
