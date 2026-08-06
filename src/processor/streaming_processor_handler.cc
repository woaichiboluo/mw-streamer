#include "mw/processor/streaming_processor_handler.h"

#include <optional>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/log/logging.h"
#include "mw/processor/internal/audio_frame_allocator.h"
#include "mw/processor/internal/enum_converter.h"
#include "mw/processor/internal/frame_adapter.h"
#include "mw/processor/internal/video_frame_allocator.h"

namespace mw::streamer::processor {
namespace {

using Log = log::Module<log::LogModule::kProcessor>;

}  // namespace

class StreamingProcessorHandler::Impl final {
 public:
  MwStreamerStreamingProcessorCallbacks callbacks{};
  std::optional<internal::VideoFrameAllocator> video_allocator;
  std::optional<internal::AudioFrameAllocator> audio_allocator;
};

StreamingProcessorHandler::StreamingProcessorHandler(
    const MwStreamerProcessorSourceInfo& source_info,
    const ffmpeg::HardwareContext* hardware_context)
    : ProcessorHandler(source_info, hardware_context),
      impl_(std::make_unique<Impl>()) {}

StreamingProcessorHandler::~StreamingProcessorHandler() { Stop(); }

MwStreamerProcessorStartResult StreamingProcessorHandler::Start(
    const MwStreamerStreamingProcessorConfig& config,
    const MwStreamerStreamingProcessorCallbacks& callbacks) {
  RequireReady("启动Streaming Processor");
  if (!config.config) {
    throw std::invalid_argument("Streaming Processor配置字符串不能为空指针");
  }
  if (source_info().has_video) {
    if (config.output_width == 0 || config.output_height == 0) {
      throw std::invalid_argument("Streaming Processor视频输出缺少有效的宽高");
    }
    impl_->video_allocator.emplace(config.output_width, config.output_height);
  } else if (config.output_width != 0 || config.output_height != 0) {
    throw std::invalid_argument(
        "纯音频Streaming Processor不能配置视频输出宽高");
  }
  if (source_info().has_audio) {
    impl_->audio_allocator.emplace();
  }

  const MwStreamerStreamingProcessorStartRequest request = {
      &source_info(),
      &config,
      &execution(),
  };
  MwStreamerProcessorStartResult result;
  try {
    result = callbacks.on_start
                 ? callbacks.on_start(&request, callbacks.user_context)
                 : kMwStreamerProcessorStartSuccess;
  } catch (...) {
    impl_->video_allocator.reset();
    impl_->audio_allocator.reset();
    throw;
  }
  if (result != kMwStreamerProcessorStartSuccess) {
    impl_->video_allocator.reset();
    impl_->audio_allocator.reset();
    Log::Warning("Streaming Processor启动失败：result={}",
                 static_cast<int>(result));
    return result;
  }

  impl_->callbacks = callbacks;
  MarkStarted(callbacks.user_context, callbacks.on_boundary,
              callbacks.update_config, callbacks.on_stop);
  Log::Info(
      "Streaming Processor启动成功：video={}, audio={}, execution={}, "
      "output={}x{}",
      source_info().has_video != 0, source_info().has_audio != 0,
      internal::ToName(execution().type), config.output_width,
      config.output_height);
  return result;
}

ffmpeg::Frame StreamingProcessorHandler::ProcessVideo(
    const ffmpeg::Frame& input) {
  RequireStarted("处理Streaming视频帧");
  if (!source_info().has_video) {
    throw std::logic_error("Streaming Processor没有视频流");
  }

  const internal::VideoFrameAdapter input_adapter(input);
  ValidateVideoInput(*input.get(), input_adapter.view());

  ffmpeg::Frame output =
      impl_->callbacks.process_video
          ? impl_->video_allocator->Allocate(input)
          : impl_->video_allocator->GetBlackFrame(input, hardware_context());
  if (impl_->callbacks.process_video) {
    internal::VideoBufferAdapter output_adapter(output);
    auto output_view = output_adapter.view();
    const MwStreamerStreamingVideoProcessRequest request = {
        &input_adapter.view(),
        &output_view,
    };
    impl_->callbacks.process_video(&request, impl_->callbacks.user_context);
  }

  output.CopyPropertiesFrom(input);
  output.ClearCrop();
  return output;
}

ffmpeg::Frame StreamingProcessorHandler::ProcessAudio(
    const ffmpeg::Frame& input) {
  RequireStarted("处理Streaming音频帧");
  if (!source_info().has_audio) {
    throw std::logic_error("Streaming Processor没有音频流");
  }

  const internal::AudioFrameAdapter input_adapter(input);
  auto output = impl_->audio_allocator->Allocate(input);
  if (impl_->callbacks.process_audio) {
    internal::AudioBufferAdapter output_adapter(output);
    auto output_view = output_adapter.view();
    const MwStreamerStreamingAudioProcessRequest request = {
        &input_adapter.view(),
        &output_view,
    };
    impl_->callbacks.process_audio(&request, impl_->callbacks.user_context);
  } else {
    ffmpeg::ThrowIfError(av_frame_copy(output.get(), input.get()),
                         "复制Streaming Processor默认音频输出");
  }

  output.CopyPropertiesFrom(input);
  return output;
}

}  // namespace mw::streamer::processor
