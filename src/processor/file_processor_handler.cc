#include "mw/processor/file_processor_handler.h"

#include <stdexcept>

#include "mw/log/logging.h"
#include "mw/processor/internal/enum_converter.h"
#include "mw/processor/internal/frame_adapter.h"

namespace mw::streamer::processor {
namespace {

using Log = log::Module<log::LogModule::kProcessor>;

}  // namespace

class FileProcessorHandler::Impl final {
 public:
  MwStreamerFileProcessorCallbacks callbacks{};
};

FileProcessorHandler::FileProcessorHandler(
    const MwStreamerProcessorSourceInfo& source_info,
    const ffmpeg::HardwareContext* hardware_context)
    : ProcessorHandler(source_info, hardware_context),
      impl_(std::make_unique<Impl>()) {}

FileProcessorHandler::~FileProcessorHandler() { Stop(); }

MwStreamerProcessorStartResult FileProcessorHandler::Start(
    const MwStreamerFileProcessorConfig& config,
    const MwStreamerFileProcessorCallbacks& callbacks) {
  RequireReady("启动File Processor");
  if (!config.config) {
    throw std::invalid_argument("File Processor配置字符串不能为空指针");
  }

  const MwStreamerFileProcessorStartRequest request = {
      &source_info(),
      &config,
      &execution(),
  };
  const auto result = callbacks.on_start
                          ? callbacks.on_start(&request, callbacks.user_context)
                          : kMwStreamerProcessorStartSuccess;
  if (result != kMwStreamerProcessorStartSuccess) {
    Log::Warning("File Processor启动失败：result={}", static_cast<int>(result));
    return result;
  }

  impl_->callbacks = callbacks;
  MarkStarted(callbacks.user_context, callbacks.on_boundary,
              callbacks.update_config, callbacks.on_stop);
  Log::Info("File Processor启动成功：video={}, audio={}, execution={}",
            source_info().has_video != 0, source_info().has_audio != 0,
            internal::ToName(execution().type));
  return result;
}

void FileProcessorHandler::ProcessVideo(const ffmpeg::Frame& input) {
  RequireStarted("处理File视频帧");
  if (!source_info().has_video) {
    throw std::logic_error("File Processor没有视频流");
  }

  const internal::VideoFrameAdapter input_adapter(input);
  ValidateVideoInput(*input.get(), input_adapter.view());
  if (impl_->callbacks.process_video) {
    impl_->callbacks.process_video(&input_adapter.view(),
                                   impl_->callbacks.user_context);
  }
}

void FileProcessorHandler::ProcessAudio(const ffmpeg::Frame& input) {
  RequireStarted("处理File音频帧");
  if (!source_info().has_audio) {
    throw std::logic_error("File Processor没有音频流");
  }

  const internal::AudioFrameAdapter input_adapter(input);
  if (impl_->callbacks.process_audio) {
    impl_->callbacks.process_audio(&input_adapter.view(),
                                   impl_->callbacks.user_context);
  }
}

}  // namespace mw::streamer::processor
