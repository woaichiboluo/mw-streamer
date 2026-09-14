#include "mw/streamer/sink/custom_sink_node.h"

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

namespace mw::streamer {

class CustomSink::Impl final {
 public:
  Impl(CustomSink& owner, MwStreamerCustomSinkCallbacks callbacks)
      : owner_(owner), callbacks_(callbacks) {
    message_sender_.context = &owner_;
    message_sender_.send = &CustomSink::SendFromCallback;
  }

  ~Impl() { Stop(); }

  NodeSnapshot GetPerformance() const {
    NodeSnapshot snapshot;
    snapshot.name = "CustomSink";
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
          throw std::logic_error("CustomSink已停止");
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
    const auto result =
        callbacks_.on_start
            ? callbacks_.on_start(&context->source_info(), &message_sender_,
                                  callbacks_.user_context)
            : kMwStreamerProcessorStartSuccess;
    if (result != kMwStreamerProcessorStartSuccess) {
      throw std::runtime_error("CustomSink拒绝启动");
    }
    context->MarkStarted(callbacks_.user_context, nullptr, nullptr, nullptr);
    context_ = std::move(context);
  }

  internal::ProcessorSinkContext& Context() const {
    if (stopped_ || stopping_.load() || !context_) {
      throw std::logic_error("CustomSink尚未启动或已停止");
    }
    return *context_;
  }

  OperationRecorder audio_performance_{PerformanceType::kAudioProcessor,
                                       PerformanceUnit::kSample,
                                       PerformanceUnit::kNone};
  OperationRecorder video_performance_{PerformanceType::kVideoProcessor,
                                       PerformanceUnit::kFrame,
                                       PerformanceUnit::kNone};
  CustomSink& owner_;
  const MwStreamerCustomSinkCallbacks callbacks_;
  MwStreamerMessageSender message_sender_{};
  std::shared_mutex lifecycle_mutex_;
  std::unique_ptr<internal::ProcessorSinkContext> context_;
  std::atomic<bool> stopping_{false};
  bool stopped_ = false;
};

CustomSink::CustomSink(std::string id,
                       MwStreamerCustomSinkCallbacks callbacks)
    : Sink(std::move(id), SinkMediaType::kFrame),
      impl_(std::make_unique<Impl>(*this, callbacks)) {}

CustomSink::~CustomSink() { Stop(); }

void CustomSink::OnStreamsReady(const FrameStreamsReady& streams) {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void CustomSink::OnAudioFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnAudioFrame(frame);
}

void CustomSink::OnVideoFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnVideoFrame(frame);
}

void CustomSink::OnTimelineReset(const TimelineReset& reset) {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void CustomSink::OnInputEnded(const StreamEnded& end) {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void CustomSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

NodeSnapshot CustomSink::GetOwnPerformance() const {
  return impl_->GetPerformance();
}

void CustomSink::SendFromCallback(
    void* context, const char* type, const void* payload, size_t payload_size,
    const MwStreamerMediaTimestamp* timestamp) noexcept {
  auto* sink = static_cast<CustomSink*>(context);
  if (!sink) {
    return;
  }
  try {
    if (!type || (payload_size != 0 && !payload)) {
      throw std::invalid_argument("CustomSink消息类型或负载无效");
    }
    SinkMessage message{sink->id(), type, payload, payload_size, std::nullopt};
    if (timestamp) {
      message.timestamp = *timestamp;
    }
    sink->SendMessage(message);
  } catch (const std::exception& error) {
    sink->ReportFatalError(error.what());
  } catch (...) {
    sink->ReportFatalError("CustomSink发送消息时发生未知错误");
  }
}

}  // namespace mw::streamer
