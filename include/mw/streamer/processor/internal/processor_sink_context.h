#ifndef MW_STREAMER_PROCESSOR_INTERNAL_PROCESSOR_SINK_CONTEXT_H_
#define MW_STREAMER_PROCESSOR_INTERNAL_PROCESSOR_SINK_CONTEXT_H_

#include <optional>
#include <stdexcept>

#include "mw/streamer/ffmpeg/pixel_format.h"
#include "mw/streamer/media/stream_event.h"
#include "mw/streamer/processor/internal/source_info_adapter.h"
#include "mw/streamer/processor/processor_handler.h"

namespace mw::streamer::internal {

// Reuses execution-context ownership and C lifecycle callbacks without taking
// media processing out of the concrete Sink. Access is protected by its
// owning sink's lifecycle lock; audio/video only read this state.
class ProcessorSinkContext final : public ProcessorHandler {
 public:
  explicit ProcessorSinkContext(const FrameStreamsReady& streams)
      : ProcessorHandler(SourceInfo(streams), streams.hardware_context) {}

  using ProcessorHandler::execution;
  using ProcessorHandler::MarkStarted;
  using ProcessorHandler::source_info;
  using ProcessorHandler::ValidateVideoInput;

  bool Open(const FrameStreamsReady& streams) {
    RequireStarted("接收Processor轨道");
    if (streams.generation == generation_ && !pending_generation_) {
      return false;
    }
    if (streams.generation == 0 ||
        (generation_ != 0 && pending_generation_ != streams.generation)) {
      throw std::logic_error("Processor新代次轨道必须先收到时间线重置");
    }
    if (!SameSourceInfo(source_info(), SourceInfo(streams))) {
      throw std::invalid_argument("Processor不能跨代次改变源流信息");
    }
    const auto* previous = hardware_context();
    const auto* next = streams.hardware_context;
    if ((previous == nullptr) != (next == nullptr) ||
        (previous && previous->get()->data != next->get()->data)) {
      throw std::invalid_argument("Processor不能跨代次改变硬件执行上下文");
    }
    generation_ = streams.generation;
    pending_generation_.reset();
    ended_ = false;
    return true;
  }

  void ValidateFrame(const FrameReady& frame, bool video) const {
    RequireStarted("处理Processor帧");
    if (pending_generation_ || ended_ || frame.generation != generation_) {
      throw std::logic_error("Processor帧不属于当前就绪的输入代次");
    }
    if (!frame.frame.get() ||
        (video ? !source_info().has_video : !source_info().has_audio)) {
      throw std::invalid_argument("Processor帧为空或没有对应媒体轨道");
    }
  }

  void ValidateVideoDevice(const Frame& frame) const {
    if (hardware_context()) {
      if (!hardware_context()->IsCompatible(*frame.get())) {
        throw std::invalid_argument("Processor视频帧与硬件执行上下文不兼容");
      }
    } else if (IsHardwarePixelFormat(
                   static_cast<AVPixelFormat>(frame->format))) {
      throw std::invalid_argument("CPU Processor不能接受硬件视频帧");
    }
  }

  bool Reset(const TimelineReset& reset) {
    RequireStarted("重置Processor时间线");
    if (reset.generation <= generation_ ||
        (pending_generation_ && reset.generation <= *pending_generation_)) {
      return false;
    }
    pending_generation_ = reset.generation;
    NotifyBoundary(kMwStreamerProcessorTimelineReset);
    return true;
  }

  bool End(const StreamEnded& end) {
    RequireStarted("结束Processor输入");
    if (pending_generation_ || ended_ || end.generation != generation_) {
      return false;
    }
    ended_ = true;
    if (end.reason == StreamEndReason::kEof) {
      NotifyBoundary(kMwStreamerProcessorEndOfInput);
    }
    return true;
  }

 private:
  static bool SameRational(const MwStreamerRational& lhs,
                           const MwStreamerRational& rhs) {
    return lhs.num == rhs.num && lhs.den == rhs.den;
  }

  static bool SameSourceInfo(const MwStreamerProcessorSourceInfo& lhs,
                             const MwStreamerProcessorSourceInfo& rhs) {
    return lhs.has_video == rhs.has_video && lhs.has_audio == rhs.has_audio &&
           (!lhs.has_video ||
            (lhs.video.codec == rhs.video.codec &&
             lhs.video.width == rhs.video.width &&
             lhs.video.height == rhs.video.height &&
             SameRational(lhs.video.frame_rate, rhs.video.frame_rate) &&
             SameRational(lhs.video.time_base, rhs.video.time_base))) &&
           (!lhs.has_audio ||
            (lhs.audio.codec == rhs.audio.codec &&
             lhs.audio.sample_rate == rhs.audio.sample_rate &&
             lhs.audio.channel_count == rhs.audio.channel_count &&
             SameRational(lhs.audio.time_base, rhs.audio.time_base)));
  }

  static MwStreamerProcessorSourceInfo SourceInfo(
      const FrameStreamsReady& streams) {
    if (streams.generation == 0) {
      throw std::invalid_argument("Processor输入代次必须大于0");
    }
    std::optional<StreamInfo> audio;
    std::optional<StreamInfo> video;
    for (const auto& stream : streams.source_streams) {
      stream.Validate();
      const auto type = stream.codec_parameters.get()->codec_type;
      if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) {
        continue;
      }
      auto& target = type == AVMEDIA_TYPE_AUDIO ? audio : video;
      if (target) {
        throw std::invalid_argument("Processor每种媒体类型仅支持一路轨道");
      }
      target = stream;
    }
    if (!audio && !video) {
      throw std::invalid_argument("Processor输入不包含音频或视频");
    }
    return internal::MakeProcessorSourceInfo(audio, video);
  }

  std::uint64_t generation_ = 0;
  std::optional<std::uint64_t> pending_generation_;
  bool ended_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PROCESSOR_INTERNAL_PROCESSOR_SINK_CONTEXT_H_
