#include "mw/streamer/internal/processor.h"

#include <algorithm>
#include <array>
#include <string>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

void CheckCallback(
    StatusCode code, std::string_view fallback_message, int callback_result,
    std::array<char, MW_PROCESSOR_ERROR_MESSAGE_CAPACITY>* error_buffer) {
  error_buffer->back() = '\0';
  if (callback_result == 0) {
    return;
  }
  if (error_buffer->front() != '\0') {
    ThrowError(code, error_buffer->data());
  }
  ThrowError(code, fallback_message);
}

void InvokeStartCallback(MwProcessorOnStart callback,
                         const MwProcessorContext* context,
                         std::string_view config, void* user_data) {
  std::array<char, MW_PROCESSOR_ERROR_MESSAGE_CAPACITY> error_buffer{};
  const std::string config_text(config);
  const int result = callback(context, config_text.c_str(), error_buffer.data(),
                              error_buffer.size(), user_data);
  CheckCallback(StatusCode::kProcessorStartCallbackFailed,
                "Processor on_start callback failed", result, &error_buffer);
}

void InvokeConfigCallback(MwProcessorOnConfigUpdate callback,
                          std::string_view config, void* user_data) {
  std::array<char, MW_PROCESSOR_ERROR_MESSAGE_CAPACITY> error_buffer{};
  const std::string config_text(config);
  const int result = callback(config_text.c_str(), error_buffer.data(),
                              error_buffer.size(), user_data);
  CheckCallback(StatusCode::kProcessorConfigUpdateCallbackFailed,
                "Processor on_config_update callback failed", result,
                &error_buffer);
}

}  // namespace

Processor::Processor(const MwProcessorCallbacks& callbacks,
                     const MwProcessorContext& context)
    : callbacks_(callbacks), context_(context) {
  if (context.input_count != 0) {
    input_sources_.assign(context.inputs, context.inputs + context.input_count);
  }
  context_.inputs = input_sources_.empty() ? nullptr : input_sources_.data();
}

const MwProcessorContext& Processor::context() const noexcept {
  return context_;
}

bool Processor::has_video_callback() const noexcept {
  return callbacks_.on_video != nullptr;
}

bool Processor::has_audio_callback() const noexcept {
  return callbacks_.on_audio != nullptr;
}

void Processor::Start(std::string_view initial_config) {
  if (callbacks_.on_start == nullptr) {
    return;
  }
  InvokeStartCallback(callbacks_.on_start, &context_, initial_config,
                      callbacks_.user_data);
}

void Processor::UpdateConfig(std::string_view config) {
  if (callbacks_.on_config_update == nullptr) {
    return;
  }
  InvokeConfigCallback(callbacks_.on_config_update, config,
                       callbacks_.user_data);
}

void Processor::ProcessVideo(const MwGpuNv12FrameView* inputs,
                             std::size_t input_count,
                             MwGpuNv12FrameView* output) const {
  if (callbacks_.on_video != nullptr) {
    callbacks_.on_video(inputs, input_count, output, callbacks_.user_data);
  }
}

void Processor::ProcessAudio(const MwAudioFrameView* inputs,
                             std::size_t input_count,
                             MwAudioFrameView* output) const {
  if (callbacks_.on_audio != nullptr) {
    callbacks_.on_audio(inputs, input_count, output, callbacks_.user_data);
    return;
  }
  std::fill_n(output->samples, output->frame_count * output->channel_count,
              0.0F);
}

void Processor::Stop() const {
  if (callbacks_.on_stop != nullptr) {
    callbacks_.on_stop(callbacks_.user_data);
  }
}

}  // namespace mw::streamer::internal
