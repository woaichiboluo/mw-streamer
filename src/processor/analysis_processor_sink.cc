#include "mw/processor/analysis_processor_sink.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "internal/processor_sink_context.h"
#include "mw/performance/operation_recorder.h"
#include "mw/processor/internal/frame_adapter.h"

namespace mw::streamer::processor {

class AnalysisProcessorSink::Impl final {
 public:
  Impl(AnalysisProcessorSink& owner, MwStreamerFileProcessorCallbacks callbacks)
      : owner_(owner), callbacks_(callbacks) {}

  ~Impl() { Stop(); }

  performance::NodeSnapshot GetPerformance() const {
    performance::NodeSnapshot snapshot;
    snapshot.name = "AnalysisProcessorSink";
    snapshot.operations = {audio_performance_.GetSnapshot(),
                           video_performance_.GetSnapshot()};
    return snapshot;
  }

  void OnStreamsReady(const media::FrameStreamsReady& streams) {
    std::exception_ptr failure;
    {
      std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
      try {
        if (stopped_ || stopping_.load()) {
          throw std::logic_error("AnalysisProcessorSink已停止");
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

  void OnAudioFrame(const media::FrameReady& frame) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().ValidateFrame(frame, false);
    audio_performance_.AddInput(std::max(frame.frame->nb_samples, 0));
    if (callbacks_.process_audio) {
      performance::OperationRecorder::Call call(audio_performance_);
      const processor::internal::AudioFrameAdapter input(frame.frame);
      callbacks_.process_audio(&input.view(), callbacks_.user_context);
    }
  }

  void OnVideoFrame(const media::FrameReady& frame) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    auto& context = Context();
    context.ValidateFrame(frame, true);
    video_performance_.AddInput(1);
    if (callbacks_.process_video) {
      performance::OperationRecorder::Call call(video_performance_);
      const processor::internal::VideoFrameAdapter input(frame.frame);
      context.ValidateVideoInput(*frame.frame.get(), input.view());
      callbacks_.process_video(&input.view(), callbacks_.user_context);
    }
  }

  void OnTimelineReset(const media::TimelineReset& reset) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().Reset(reset);
  }

  void OnInputEnded(const media::StreamEnded& end) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().End(end);
  }

  void SetConfig(std::string config) {
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    processor_config_ = std::move(config);
    if (context_) {
      Context().UpdateConfig(processor_config_);
    }
  }

  bool OnMessage(const sink::SinkMessage& message) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (stopping_.load() || !context_) {
      return true;
    }
    if (!callbacks_.on_message) {
      return false;
    }
    const std::string sink_id(message.sink_id);
    const std::string type(message.type);
    const MwStreamerMessage view{
        sink_id.c_str(),
        type.c_str(),
        message.payload,
        message.payload_size,
        static_cast<std::uint8_t>(message.timestamp.has_value()),
        message.timestamp.value_or(MwStreamerMediaTimestamp{})};
    callbacks_.on_message(&view, callbacks_.user_context);
    return true;
  }

  void Stop() noexcept {
    stopping_.store(true);
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    StopLocked();
  }

 private:
  void Start(const media::FrameStreamsReady& streams) {
    auto context = std::make_unique<internal::ProcessorSinkContext>(streams);
    const MwStreamerFileProcessorConfig config{processor_config_.c_str()};
    const MwStreamerFileProcessorStartRequest request{
        &context->source_info(), &config, &context->execution()};
    const auto result =
        callbacks_.on_start
            ? callbacks_.on_start(&request, callbacks_.user_context)
            : kMwStreamerProcessorStartSuccess;
    if (result != kMwStreamerProcessorStartSuccess) {
      throw std::runtime_error("AnalysisProcessorSink拒绝启动");
    }
    context->MarkStarted(callbacks_.user_context, callbacks_.on_boundary,
                         callbacks_.on_config_update, callbacks_.on_stop);
    context_ = std::move(context);
  }

  internal::ProcessorSinkContext& Context() const {
    if (stopped_ || stopping_.load() || !context_) {
      throw std::logic_error("AnalysisProcessorSink尚未启动或已停止");
    }
    return *context_;
  }

  void StopLocked() noexcept {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    if (context_) {
      context_->Stop();
      context_.reset();
    }
  }

  performance::OperationRecorder audio_performance_{
      performance::PerformanceType::kAudioProcessor,
      performance::PerformanceUnit::kSample,
      performance::PerformanceUnit::kNone};
  performance::OperationRecorder video_performance_{
      performance::PerformanceType::kVideoProcessor,
      performance::PerformanceUnit::kFrame,
      performance::PerformanceUnit::kNone};
  AnalysisProcessorSink& owner_;
  std::string processor_config_;
  const MwStreamerFileProcessorCallbacks callbacks_;
  std::shared_mutex lifecycle_mutex_;
  std::mutex update_mutex_;
  std::unique_ptr<internal::ProcessorSinkContext> context_;
  std::atomic<bool> stopping_{false};
  bool stopped_ = false;
};

AnalysisProcessorSink::AnalysisProcessorSink(
    std::string id, MwStreamerFileProcessorCallbacks callbacks)
    : sink::Sink(std::move(id), sink::SinkMediaType::kFrame),
      impl_(std::make_unique<Impl>(*this, callbacks)) {}

AnalysisProcessorSink::~AnalysisProcessorSink() { Stop(); }

performance::NodeSnapshot AnalysisProcessorSink::GetOwnPerformance() const {
  return impl_->GetPerformance();
}

void AnalysisProcessorSink::OnStreamsReady(
    const media::FrameStreamsReady& streams) {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void AnalysisProcessorSink::OnAudioFrame(const media::FrameReady& frame) {
  CloseRegistration();
  impl_->OnAudioFrame(frame);
}

void AnalysisProcessorSink::OnVideoFrame(const media::FrameReady& frame) {
  CloseRegistration();
  impl_->OnVideoFrame(frame);
}

void AnalysisProcessorSink::OnTimelineReset(const media::TimelineReset& reset) {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void AnalysisProcessorSink::OnInputEnded(const media::StreamEnded& end) {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void AnalysisProcessorSink::UpdateConfig(std::string config) {
  impl_->SetConfig(std::move(config));
}

void AnalysisProcessorSink::OnMessage(const sink::SinkMessage& message) {
  if (!impl_->OnMessage(message)) {
    sink::Sink::OnMessage(message);
  }
}

void AnalysisProcessorSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

}  // namespace mw::streamer::processor
