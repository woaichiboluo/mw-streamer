#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "mw/streamer/performance/operation_recorder.h"
#include "mw/streamer/processor/internal/frame_adapter.h"
#include "mw/streamer/processor/internal/processor_sink_context.h"
#include "mw/streamer/sink/frame_custom_sink_node.h"

namespace mw::streamer {

class FrameCustomSink::Impl final {
 public:
  Impl(FrameCustomSink& owner, MwStreamerFrameCustomSinkCallbacks callbacks)
      : owner_(owner), callbacks_(callbacks) {}

  ~Impl() { Stop(); }

  NodeSnapshot GetPerformance() const {
    NodeSnapshot snapshot;
    snapshot.name = "FrameCustomSink";
    snapshot.operations = {audio_performance_.GetSnapshot(),
                           video_performance_.GetSnapshot()};
    return snapshot;
  }

  void OnStreamsReady(const FrameStreamsReady& streams) {
    std::exception_ptr failure;
    {
      std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
      try {
        if (stopped_ || stopping_.load()) {
          throw std::logic_error("FrameCustomSink已停止");
        }
        if (!context_) {
          Start(streams);
        }
        if (context_->Open(streams)) {
          owner_.StartMessages();
        }
      } catch (...) {
        stopping_.store(true);
        failure = std::current_exception();
      }
    }
    if (failure) {
      owner_.Stop();
      std::rethrow_exception(failure);
    }
  }

  void OnAudioFrame(const FrameReady& frame) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().ValidateFrame(frame, false);
    audio_performance_.AddInput(std::max(frame.frame->nb_samples, 0));
    if (callbacks_.on_audio) {
      OperationRecorder::Call call(audio_performance_);
      const internal::AudioFrameAdapter input(frame.frame);
      callbacks_.on_audio(&input.view(), callbacks_.user_context);
    }
  }

  void OnVideoFrame(const FrameReady& frame) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    auto& context = Context();
    context.ValidateFrame(frame, true);
    video_performance_.AddInput(1);
    if (callbacks_.on_frame) {
      OperationRecorder::Call call(video_performance_);
      const internal::VideoFrameAdapter input(frame.frame);
      context.ValidateVideoInput(*frame.frame.get(), input.view());
      callbacks_.on_frame(&input.view(), callbacks_.user_context);
    }
  }

  void OnTimelineReset(const TimelineReset& reset) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().Reset(reset);
  }

  void OnInputEnded(const StreamEnded& end) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().End(end);
  }

  void OnMessage(const MwStreamerMessage& message) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (stopping_.load() || !context_ || !callbacks_.on_message) {
      return;
    }
    callbacks_.on_message(&message, callbacks_.user_context);
  }

  void Stop() noexcept {
    stopping_.store(true);
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    if (context_) {
      context_->Stop();
      context_.reset();
    }
  }

 private:
  void Start(const FrameStreamsReady& streams) {
    auto context = std::make_unique<internal::ProcessorSinkContext>(streams);
    const auto result = callbacks_.on_start
                            ? callbacks_.on_start(&context->source_info(),
                                                  callbacks_.user_context)
                            : kMwStreamerProcessorStartSuccess;
    if (result != kMwStreamerProcessorStartSuccess) {
      throw std::runtime_error("FrameCustomSink拒绝启动");
    }
    context->MarkStarted(callbacks_.user_context, callbacks_.on_boundary,
                         nullptr, callbacks_.on_stop);
    context_ = std::move(context);
  }

  internal::ProcessorSinkContext& Context() const {
    if (stopped_ || stopping_.load() || !context_) {
      throw std::logic_error("FrameCustomSink尚未启动或已停止");
    }
    return *context_;
  }

  OperationRecorder audio_performance_{PerformanceType::kAudioProcessor,
                                       PerformanceUnit::kSample,
                                       PerformanceUnit::kNone};
  OperationRecorder video_performance_{PerformanceType::kVideoProcessor,
                                       PerformanceUnit::kFrame,
                                       PerformanceUnit::kNone};
  FrameCustomSink& owner_;
  const MwStreamerFrameCustomSinkCallbacks callbacks_;
  std::shared_mutex lifecycle_mutex_;
  std::unique_ptr<internal::ProcessorSinkContext> context_;
  std::atomic<bool> stopping_{false};
  bool stopped_ = false;
};

FrameCustomSink::FrameCustomSink(std::string id,
                                 MwStreamerFrameCustomSinkCallbacks callbacks)
    : Sink(std::move(id), SinkMediaType::kFrame),
      impl_(std::make_unique<Impl>(*this, callbacks)) {}

FrameCustomSink::~FrameCustomSink() { Stop(); }

void FrameCustomSink::OnStreamsReady(const FrameStreamsReady& streams) {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void FrameCustomSink::OnAudioFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnAudioFrame(frame);
}

void FrameCustomSink::OnVideoFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnVideoFrame(frame);
}

void FrameCustomSink::OnTimelineReset(const TimelineReset& reset) {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void FrameCustomSink::OnInputEnded(const StreamEnded& end) {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void FrameCustomSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

void FrameCustomSink::OnMessage(const MwStreamerMessage& message) {
  impl_->OnMessage(message);
}

NodeSnapshot FrameCustomSink::GetOwnPerformance() const {
  return impl_->GetPerformance();
}

}  // namespace mw::streamer
