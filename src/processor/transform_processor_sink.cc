#include "mw/processor/transform_processor_sink.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "internal/processor_sink_context.h"
#include "mw/performance/operation_recorder.h"
#include "mw/processor/internal/audio_frame_allocator.h"
#include "mw/processor/internal/frame_adapter.h"
#include "mw/processor/internal/video_frame_allocator.h"
#include "mw/sink/fatal_error.h"

namespace mw::streamer::processor {
class TransformProcessorSink::Impl final {
 public:
  Impl(TransformProcessorSink& owner,
       processor::StreamingProcessorConfig config,
       MwStreamerStreamingProcessorCallbacks callbacks)
      : owner_(owner),
        config_(std::move(config)),
        callbacks_(callbacks),
        outputs_(owner.downstream()) {}

  ~Impl() { Stop(); }

  performance::NodeSnapshot GetPerformance() const {
    performance::NodeSnapshot snapshot;
    snapshot.name = "TransformProcessorSink";
    snapshot.operations = {audio_performance_.GetSnapshot(),
                           video_performance_.GetSnapshot()};
    return snapshot;
  }

  void OnStreamsReady(const media::FrameStreamsReady& streams) {
    std::exception_ptr failure;
    {
      std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
      try {
        if (stopping_.load() || stopped_) {
          throw std::logic_error("TransformProcessorSink已停止");
        }
        if (!context_) {
          Start(streams);
        }
        if (stopping_.load()) {
          return;
        }
        if (context_->Open(streams)) {
          owner_.StartMessages();
          for (auto& output : outputs_) {
            output->OnStreamsReady(streams);
          }
        }
      } catch (...) {
        // A message sent by a partially initialized child must not slip through
        // when this exclusive boundary releases the waiting message callback.
        stopping_.store(true, std::memory_order_release);
        failure = std::current_exception();
      }
    }
    if (failure) {
      // Message callbacks may be waiting for lifecycle_mutex_. Wait only
      // after releasing the boundary lock, including partial startup failures.
      owner_.Stop();
      std::rethrow_exception(failure);
    }
  }

  void OnAudioFrame(const media::FrameReady& frame) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    Context().ValidateFrame(frame, false);
    audio_performance_.AddInput(std::max(frame.frame->nb_samples, 0));
    if (!callbacks_.process_audio) {
      audio_performance_.AddOutput(std::max(frame.frame->nb_samples, 0));
      for (auto& output : outputs_) {
        output->OnAudioFrame(frame);
      }
      return;
    }
    const media::FrameReady result{frame.generation, ProcessAudio(frame.frame)};
    audio_performance_.AddOutput(std::max(result.frame->nb_samples, 0));
    for (auto& output : outputs_) {
      output->OnAudioFrame(result);
    }
  }

  void OnVideoFrame(const media::FrameReady& frame) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    auto& context = Context();
    context.ValidateFrame(frame, true);
    video_performance_.AddInput(1);
    if (!callbacks_.process_video) {
      ValidatePassthroughVideo(frame.frame, context);
      video_performance_.AddOutput(1);
      for (auto& output : outputs_) {
        output->OnVideoFrame(frame);
      }
      return;
    }
    const media::FrameReady result{frame.generation,
                                   ProcessVideo(frame.frame, context)};
    video_performance_.AddOutput(1);
    for (auto& output : outputs_) {
      output->OnVideoFrame(result);
    }
  }

  void OnTimelineReset(const media::TimelineReset& reset) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (Context().Reset(reset)) {
      for (auto& output : outputs_) {
        output->OnTimelineReset(reset);
      }
    }
  }

  void OnInputEnded(const media::StreamEnded& end) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (Context().End(end)) {
      for (auto& output : outputs_) {
        output->OnInputEnded(end);
      }
    }
  }

  void UpdateConfig(std::string config) {
    std::shared_lock<std::shared_mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    Context().UpdateConfig(config);
    config_.config = std::move(config);
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
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    stopping_.store(true, std::memory_order_release);
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (stopped_) {
      return;
    }
    owner_.StopDownstream();
    FinishStop();
  }

 private:
  void Start(const media::FrameStreamsReady& streams) {
    if (outputs_.empty()) {
      throw std::logic_error("TransformProcessorSink启动前至少需要一个下游");
    }
    auto context = std::make_unique<internal::ProcessorSinkContext>(streams);
    PrepareAllocators(context->source_info());
    const MwStreamerStreamingProcessorConfig config{
        config_.output_width, config_.output_height, config_.config.c_str()};
    const MwStreamerStreamingProcessorStartRequest request{
        &context->source_info(), &config, &context->execution()};
    const auto result =
        callbacks_.on_start
            ? callbacks_.on_start(&request, callbacks_.user_context)
            : kMwStreamerProcessorStartSuccess;
    if (result != kMwStreamerProcessorStartSuccess) {
      throw std::runtime_error("TransformProcessorSink拒绝启动");
    }
    context->MarkStarted(callbacks_.user_context, callbacks_.on_boundary,
                         callbacks_.update_config, callbacks_.on_stop);
    context_ = std::move(context);
  }

  void PrepareAllocators(const MwStreamerProcessorSourceInfo& source) {
    if (source.has_video) {
      if (config_.output_width == 0 || config_.output_height == 0) {
        throw std::invalid_argument(
            "TransformProcessorSink视频输出宽高必须有效");
      }
      if (callbacks_.process_video) {
        video_allocator_.emplace(config_.output_width, config_.output_height);
      }
    } else if (config_.output_width != 0 || config_.output_height != 0) {
      throw std::invalid_argument(
          "纯音频TransformProcessorSink输出宽高必须为0");
    }
    if (source.has_audio && callbacks_.process_audio) {
      audio_allocator_.emplace();
    }
  }

  ffmpeg::Frame ProcessAudio(const ffmpeg::Frame& frame) {
    performance::OperationRecorder::Call call(audio_performance_);
    const processor::internal::AudioFrameAdapter input(frame);
    auto result = audio_allocator_->Allocate(frame);
    processor::internal::AudioBufferAdapter output(result);
    auto output_view = output.view();
    const MwStreamerStreamingAudioProcessRequest request{&input.view(),
                                                         &output_view};
    callbacks_.process_audio(&request, callbacks_.user_context);
    result.CopyPropertiesFrom(frame);
    return result;
  }

  ffmpeg::Frame ProcessVideo(const ffmpeg::Frame& frame,
                             internal::ProcessorSinkContext& context) {
    performance::OperationRecorder::Call call(video_performance_);
    const processor::internal::VideoFrameAdapter input(frame);
    context.ValidateVideoInput(*frame.get(), input.view());
    auto result = video_allocator_->Allocate(frame);
    processor::internal::VideoBufferAdapter output(result);
    auto output_view = output.view();
    const MwStreamerStreamingVideoProcessRequest request{&input.view(),
                                                         &output_view};
    callbacks_.process_video(&request, callbacks_.user_context);
    result.CopyPropertiesFrom(frame);
    result.ClearCrop();
    return result;
  }

  void ValidatePassthroughVideo(
      const ffmpeg::Frame& frame,
      const internal::ProcessorSinkContext& context) const {
    context.ValidateVideoDevice(frame);
    if (frame->width <= 0 || frame->height <= 0 ||
        static_cast<std::uint32_t>(frame->width) != config_.output_width ||
        static_cast<std::uint32_t>(frame->height) != config_.output_height) {
      throw sink::FatalError(fmt::format(
          "TransformProcessorSink视频透传尺寸不匹配：实际{}x{}，期望{}x{}",
          frame->width, frame->height, config_.output_width,
          config_.output_height));
    }
  }

  internal::ProcessorSinkContext& Context() const {
    if (stopping_.load() || stopped_ || !context_) {
      throw std::logic_error("TransformProcessorSink尚未启动或已停止");
    }
    return *context_;
  }

  void FinishStop() noexcept {
    stopped_ = true;
    if (context_) {
      context_->Stop();
    }
    audio_allocator_.reset();
    video_allocator_.reset();
    context_.reset();
  }

  performance::OperationRecorder audio_performance_{
      performance::PerformanceType::kAudioProcessor,
      performance::PerformanceUnit::kSample,
      performance::PerformanceUnit::kSample};
  performance::OperationRecorder video_performance_{
      performance::PerformanceType::kVideoProcessor,
      performance::PerformanceUnit::kFrame,
      performance::PerformanceUnit::kFrame};
  TransformProcessorSink& owner_;
  processor::StreamingProcessorConfig config_;
  const MwStreamerStreamingProcessorCallbacks callbacks_;
  std::shared_mutex lifecycle_mutex_;
  std::mutex update_mutex_;
  std::mutex stop_mutex_;
  std::unique_ptr<internal::ProcessorSinkContext> context_;
  std::optional<processor::internal::AudioFrameAllocator> audio_allocator_;
  std::optional<processor::internal::VideoFrameAllocator> video_allocator_;
  const std::vector<std::unique_ptr<sink::Sink>>& outputs_;
  std::atomic<bool> stopping_{false};
  bool stopped_ = false;
};

TransformProcessorSink::TransformProcessorSink(
    std::string id, processor::StreamingProcessorConfig config,
    MwStreamerStreamingProcessorCallbacks callbacks)
    : sink::Sink(std::move(id), sink::SinkMediaType::kFrame,
                 sink::SinkMediaType::kFrame),
      impl_(std::make_unique<Impl>(*this, std::move(config), callbacks)) {}

TransformProcessorSink::~TransformProcessorSink() { Stop(); }

performance::NodeSnapshot TransformProcessorSink::GetOwnPerformance() const {
  return impl_->GetPerformance();
}

void TransformProcessorSink::OnStreamsReady(
    const media::FrameStreamsReady& streams) {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void TransformProcessorSink::OnAudioFrame(const media::FrameReady& frame) {
  CloseRegistration();
  impl_->OnAudioFrame(frame);
}

void TransformProcessorSink::OnVideoFrame(const media::FrameReady& frame) {
  CloseRegistration();
  impl_->OnVideoFrame(frame);
}

void TransformProcessorSink::OnTimelineReset(
    const media::TimelineReset& reset) {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void TransformProcessorSink::OnInputEnded(const media::StreamEnded& end) {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void TransformProcessorSink::UpdateConfig(std::string config) {
  impl_->UpdateConfig(std::move(config));
}

void TransformProcessorSink::OnMessage(const sink::SinkMessage& message) {
  if (!impl_->OnMessage(message)) {
    sink::Sink::OnMessage(message);
  }
}

void TransformProcessorSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

}  // namespace mw::streamer::processor
