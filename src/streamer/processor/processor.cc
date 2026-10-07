#include "mw/streamer/processor/processor.h"

#include <utility>

#include "mw/log.h"

namespace mw::streamer {

Processor::~Processor() { Stop(); }

void Processor::SetOnReady(OnReady callback) noexcept {
  on_ready_ = std::move(callback);
}

void Processor::SetOnVideo(OnVideo callback) noexcept {
  on_video_ = std::move(callback);
}

void Processor::SetOnAudio(OnAudio callback) noexcept {
  on_audio_ = std::move(callback);
}

void Processor::SetOnEnded(OnEnded callback) noexcept {
  on_ended_ = std::move(callback);
}

void Processor::SetOnStop(OnStop callback) noexcept {
  on_stop_ = std::move(callback);
}

bool Processor::Start(const std::vector<ffmpeg::StreamInfo>& streams,
                      const ffmpeg::HwDeviceContext& video_device) noexcept {
  if (on_ready_ && !on_ready_(streams, video_device)) {
    MW_LOG_WARNING("streamer", "Processor启动被OnReady拒绝");
    return false;
  }
  started_ = true;
  ended_ = false;
  MW_LOG_INFO("streamer", "Processor启动完成");
  return true;
}

ffmpeg::Frame Processor::ProcessVideo(const ffmpeg::Frame& frame) noexcept {
  return on_video_ ? on_video_(frame) : frame.Ref();
}

ffmpeg::Frame Processor::ProcessAudio(const ffmpeg::Frame& frame) noexcept {
  return on_audio_ ? on_audio_(frame) : frame.Ref();
}

void Processor::End() noexcept {
  if (!started_ || ended_) return;
  ended_ = true;
  MW_LOG_INFO("streamer", "Processor处理结束");
  if (on_ended_) on_ended_();
}

void Processor::Stop() noexcept {
  if (!started_) return;
  started_ = false;
  MW_LOG_INFO("streamer", "Processor停止完成");
  if (on_stop_) on_stop_();
}

}  // namespace mw::streamer
