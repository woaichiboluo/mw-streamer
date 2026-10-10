#include "mw/streamer/processor/batch_processor.h"

#include <utility>

#include "mw/log.h"

namespace mw::streamer {

BatchProcessor::~BatchProcessor() noexcept { Stop(); }

void BatchProcessor::SetOnReady(OnReady callback) noexcept {
  on_ready_ = std::move(callback);
}

void BatchProcessor::SetOnVideo(OnVideo callback) noexcept {
  on_video_ = std::move(callback);
}

void BatchProcessor::SetOnAudio(OnAudio callback) noexcept {
  on_audio_ = std::move(callback);
}

void BatchProcessor::SetOnEnded(OnEnded callback) noexcept {
  on_ended_ = std::move(callback);
}

void BatchProcessor::SetOnStop(OnStop callback) noexcept {
  on_stop_ = std::move(callback);
}

bool BatchProcessor::Start(
    const std::vector<ffmpeg::StreamInfo>& streams,
    const ffmpeg::HwDeviceContext& video_device) noexcept {
  if (on_ready_ && !on_ready_(streams, video_device)) {
    MW_LOG_WARNING("streamer", "BatchProcessor 启动被 OnReady 拒绝");
    return false;
  }
  started_ = true;
  ended_ = false;
  MW_LOG_INFO("streamer", "BatchProcessor 启动完成");
  return true;
}

void BatchProcessor::ProcessVideo(const ffmpeg::Frame& frame) noexcept {
  if (on_video_) on_video_(frame);
}

void BatchProcessor::ProcessAudio(const ffmpeg::Frame& frame) noexcept {
  if (on_audio_) on_audio_(frame);
}

void BatchProcessor::End() noexcept {
  if (!started_ || ended_) return;
  ended_ = true;
  MW_LOG_INFO("streamer", "BatchProcessor 处理结束");
  if (on_ended_) on_ended_();
}

void BatchProcessor::Stop() noexcept {
  if (!started_) return;
  started_ = false;
  MW_LOG_INFO("streamer", "BatchProcessor 停止完成");
  if (on_stop_) on_stop_();
}

}  // namespace mw::streamer
