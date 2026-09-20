#include <atomic>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "mw/streamer/performance/operation_recorder.h"
#include "mw/streamer/processor/internal/processor_sink_context.h"
#include "mw/streamer/sink/packet_custom_sink_node.h"

namespace mw::streamer {

class PacketCustomSink::Impl final {
 public:
  Impl(PacketCustomSink& owner, MwStreamerPacketCustomSinkCallbacks callbacks)
      : owner_(owner), callbacks_(callbacks) {}

  ~Impl() { Stop(); }

  void OnStreamsReady(const StreamsReady& streams) noexcept {
    Guard([&] {
      std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
      if (stopping_.load()) return;
      int audio_index = -1;
      int video_index = -1;
      for (const auto& stream : streams.streams) {
        stream.Validate();
        auto& index =
            stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO
                ? audio_index
                : video_index;
        if (index != -1) {
          throw std::invalid_argument("PacketCustomSink每种媒体仅支持一路轨道");
        }
        index = stream.stream_index;
      }
      if (audio_index != -1 && audio_index == video_index) {
        throw std::invalid_argument("PacketCustomSink音视频轨道索引重复");
      }
      const FrameStreamsReady source{streams.generation, streams.streams,
                                     nullptr};
      if (!context_) {
        auto context = std::make_unique<internal::ProcessorSinkContext>(source);
        const auto result = callbacks_.on_start
                                ? callbacks_.on_start(&context->source_info(),
                                                      callbacks_.user_context)
                                : kMwStreamerProcessorStartSuccess;
        if (result != kMwStreamerProcessorStartSuccess) {
          throw std::runtime_error("PacketCustomSink拒绝启动");
        }
        context->MarkStarted(callbacks_.user_context, callbacks_.on_boundary,
                             nullptr, callbacks_.on_stop);
        context_ = std::move(context);
      }
      if (context_->Open(source)) {
        audio_index_ = audio_index;
        video_index_ = video_index;
        owner_.StartMessages();
      }
    });
  }

  void OnPacket(const PacketReady& packet) noexcept {
    Guard([&] {
      std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
      if (stopping_.load()) return;
      Context().ValidatePacket(packet);
      const bool video =
          video_index_ >= 0 && packet.packet->stream_index == video_index_;
      if (!video &&
          (audio_index_ < 0 || packet.packet->stream_index != audio_index_)) {
        throw std::invalid_argument("PacketCustomSink收到未知轨道的Packet");
      }
      auto& recorder = video ? video_performance_ : audio_performance_;
      recorder.AddInput(1, packet.packet->size);
      const auto callback =
          video ? callbacks_.on_video_packet : callbacks_.on_audio_packet;
      if (callback) {
        OperationRecorder::Call call(recorder);
        callback(packet.packet.get(), callbacks_.user_context);
      }
    });
  }

  void OnTimelineReset(const TimelineReset& reset) noexcept {
    Guard([&] {
      std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
      if (!stopping_.load()) Context().Reset(reset);
    });
  }

  void OnInputEnded(const StreamEnded& end) noexcept {
    Guard([&] {
      std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
      if (!stopping_.load()) Context().End(end);
    });
  }

  void OnMessage(const MwStreamerMessage& message) {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (stopping_.load() || !context_ || !callbacks_.on_message) return;
    callbacks_.on_message(&message, callbacks_.user_context);
  }

  void Stop() noexcept {
    stopping_.store(true);
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (context_) {
      context_->Stop();
      context_.reset();
    }
  }

  NodeSnapshot GetPerformance() const {
    NodeSnapshot snapshot;
    snapshot.name = "PacketCustomSink";
    snapshot.operations = {audio_performance_.GetSnapshot(),
                           video_performance_.GetSnapshot()};
    return snapshot;
  }

 private:
  template <typename Callback>
  void Guard(Callback&& callback) noexcept {
    if (stopping_.load()) return;
    try {
      callback();
    } catch (const std::exception& error) {
      stopping_.store(true);
      owner_.ReportFatalError(error.what());
    } catch (...) {
      stopping_.store(true);
      owner_.ReportFatalError("PacketCustomSink回调发生未知错误");
    }
  }

  internal::ProcessorSinkContext& Context() const {
    if (!context_) throw std::logic_error("PacketCustomSink尚未启动");
    return *context_;
  }

  PacketCustomSink& owner_;
  const MwStreamerPacketCustomSinkCallbacks callbacks_;
  std::shared_mutex lifecycle_mutex_;
  std::unique_ptr<internal::ProcessorSinkContext> context_;
  int audio_index_ = -1;
  int video_index_ = -1;
  std::atomic<bool> stopping_{false};
  OperationRecorder audio_performance_{PerformanceType::kAudioProcessor,
                                       PerformanceUnit::kPacket,
                                       PerformanceUnit::kNone};
  OperationRecorder video_performance_{PerformanceType::kVideoProcessor,
                                       PerformanceUnit::kPacket,
                                       PerformanceUnit::kNone};
};

PacketCustomSink::PacketCustomSink(
    std::string id, MwStreamerPacketCustomSinkCallbacks callbacks)
    : Sink(std::move(id), SinkMediaType::kPacket),
      impl_(std::make_unique<Impl>(*this, callbacks)) {}

PacketCustomSink::~PacketCustomSink() { Stop(); }

void PacketCustomSink::OnStreamsReady(const StreamsReady& streams) noexcept {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void PacketCustomSink::OnPacket(const PacketReady& packet) noexcept {
  CloseRegistration();
  impl_->OnPacket(packet);
}

void PacketCustomSink::OnTimelineReset(const TimelineReset& reset) noexcept {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void PacketCustomSink::OnInputEnded(const StreamEnded& end) noexcept {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void PacketCustomSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

void PacketCustomSink::OnMessage(const MwStreamerMessage& message) {
  impl_->OnMessage(message);
}

NodeSnapshot PacketCustomSink::GetOwnPerformance() const {
  return impl_->GetPerformance();
}

}  // namespace mw::streamer
