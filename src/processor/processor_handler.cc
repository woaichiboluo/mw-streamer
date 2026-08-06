#include "mw/processor/processor_handler.h"

#include <fmt/format.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "mw/ffmpeg/hardware_context.h"
#include "mw/log/logging.h"
#include "mw/processor/internal/enum_converter.h"
#include "mw/processor/internal/execution_context_adapter.h"

namespace mw::streamer::processor {
namespace {

using Log = log::Module<log::LogModule::kProcessor>;

enum class HandlerState {
  kReady,
  kStarted,
  kStopped,
};

}  // namespace

class ProcessorHandler::Impl final {
 public:
  Impl(const MwStreamerProcessorSourceInfo& source_info,
       const ffmpeg::HardwareContext* hardware_context)
      : source_info_(source_info) {
    if (hardware_context && !source_info_.has_video) {
      throw std::invalid_argument("纯音频Processor不能包含硬件执行上下文");
    }
    if (hardware_context) {
      hardware_context_.emplace(*hardware_context);
    }
    execution_ = internal::MakeProcessorExecutionContext(
        hardware_context_ ? &*hardware_context_ : nullptr);
  }

  void RequireReady(const char* operation) const {
    if (state_ != HandlerState::kReady) {
      throw std::logic_error(fmt::format("{}只能在启动前执行", operation));
    }
  }

  void RequireStarted(const char* operation) const {
    if (state_ != HandlerState::kStarted) {
      throw std::logic_error(fmt::format("{}只能在启动后执行", operation));
    }
  }

  void MarkStarted(void* user_context,
                   MwStreamerProcessorBoundaryCallback on_boundary,
                   MwStreamerProcessorUpdateConfigCallback update_config,
                   MwStreamerProcessorStopCallback on_stop) {
    RequireReady("完成Processor启动");
    user_context_ = user_context;
    on_boundary_ = on_boundary;
    update_config_ = update_config;
    on_stop_ = on_stop;
    state_ = HandlerState::kStarted;
  }

  void NotifyBoundary(MwStreamerProcessorBoundaryReason reason) {
    RequireStarted("通知Processor输入边界");
    if (on_boundary_) {
      on_boundary_(reason, user_context_);
    }
    Log::Debug("Processor处理输入边界：reason={}", internal::ToName(reason));
  }

  void UpdateConfig(std::string config) {
    RequireStarted("更新Processor配置");
    if (update_config_) {
      update_config_(config.c_str(), user_context_);
    }
    Log::Debug("Processor运行配置更新完成：bytes={}", config.size());
  }

  void Stop() noexcept {
    if (state_ != HandlerState::kStarted) {
      return;
    }
    state_ = HandlerState::kStopped;
    try {
      if (on_stop_) {
        on_stop_(user_context_);
      }
      Log::Info("Processor停止完成");
    } catch (const std::exception& error) {
      Log::Error("Processor停止回调异常，已隔离：{}", error.what());
    } catch (...) {
      Log::Error("Processor停止回调异常，已隔离：未知异常");
    }
  }

  void ValidateVideoInput(const AVFrame& input,
                          const MwStreamerVideoFrameView& view) const {
    if (!hardware_context_) {
      if (view.buffer.memory_type != kMwStreamerMemoryHost) {
        throw std::invalid_argument("CPU Processor不能接受硬件视频帧");
      }
      return;
    }
    if (!hardware_context_->IsCompatible(input)) {
      throw std::invalid_argument("Processor输入硬件帧与执行上下文不兼容");
    }
  }

  const MwStreamerProcessorSourceInfo& source_info() const noexcept {
    return source_info_;
  }

  const MwStreamerExecutionContext& execution() const noexcept {
    return execution_;
  }

  const ffmpeg::HardwareContext* hardware_context() const noexcept {
    return hardware_context_ ? &*hardware_context_ : nullptr;
  }

 private:
  MwStreamerProcessorSourceInfo source_info_{};
  std::optional<ffmpeg::HardwareContext> hardware_context_;
  MwStreamerExecutionContext execution_{};
  HandlerState state_ = HandlerState::kReady;
  void* user_context_ = nullptr;
  MwStreamerProcessorBoundaryCallback on_boundary_ = nullptr;
  MwStreamerProcessorUpdateConfigCallback update_config_ = nullptr;
  MwStreamerProcessorStopCallback on_stop_ = nullptr;
};

ProcessorHandler::ProcessorHandler(
    const MwStreamerProcessorSourceInfo& source_info,
    const ffmpeg::HardwareContext* hardware_context)
    : impl_(std::make_unique<Impl>(source_info, hardware_context)) {}

ProcessorHandler::~ProcessorHandler() { Stop(); }

void ProcessorHandler::NotifyBoundary(
    MwStreamerProcessorBoundaryReason reason) {
  impl_->NotifyBoundary(reason);
}

void ProcessorHandler::UpdateConfig(std::string config) {
  impl_->UpdateConfig(std::move(config));
}

void ProcessorHandler::Stop() noexcept { impl_->Stop(); }

void ProcessorHandler::RequireReady(const char* operation) const {
  impl_->RequireReady(operation);
}

void ProcessorHandler::RequireStarted(const char* operation) const {
  impl_->RequireStarted(operation);
}

void ProcessorHandler::MarkStarted(
    void* user_context, MwStreamerProcessorBoundaryCallback on_boundary,
    MwStreamerProcessorUpdateConfigCallback update_config,
    MwStreamerProcessorStopCallback on_stop) {
  impl_->MarkStarted(user_context, on_boundary, update_config, on_stop);
}

void ProcessorHandler::ValidateVideoInput(
    const AVFrame& input, const MwStreamerVideoFrameView& view) const {
  impl_->ValidateVideoInput(input, view);
}

const MwStreamerProcessorSourceInfo& ProcessorHandler::source_info()
    const noexcept {
  return impl_->source_info();
}

const MwStreamerExecutionContext& ProcessorHandler::execution() const noexcept {
  return impl_->execution();
}

const ffmpeg::HardwareContext* ProcessorHandler::hardware_context()
    const noexcept {
  return impl_->hardware_context();
}

}  // namespace mw::streamer::processor
