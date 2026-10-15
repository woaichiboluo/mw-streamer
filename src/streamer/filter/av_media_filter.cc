#include "mw/streamer/filter/av_media_filter.h"

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "mw/log.h"

namespace mw::streamer {

class AvMediaFilter::Impl final {
 public:
  ~Impl() {
    try {
      Stop();
    } catch (const std::exception& error) {
      MW_LOG_ERROR("streamer", "AvMediaFilter 停止回调失败: {}", error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "AvMediaFilter 停止回调发生未知异常");
    }
  }

  template <typename Callback>
  void Configure(Callback& target, Callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      throw std::logic_error("配置 AvMediaFilter 回调前需要 Stop");
    }
    target = std::move(callback);
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      throw std::logic_error("AvMediaFilter 重新启动前需要 Stop");
    }
    if (on_start_) on_start_();
    failed_ = false;
    started_ = true;
  }

  void Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return;
    if (stopping_) {
      idle_.wait(lock, [this] { return !stopping_; });
      return;
    }
    stopping_ = true;
    idle_.wait(lock, [this] { return active_calls_ == 0; });
    started_ = false;
    stopping_ = false;
    idle_.notify_all();
    if (on_stop_) on_stop_();
  }

  AvDecoderReadResult Process(AvDecoderReadResult input, bool video) {
    const auto& callback = video ? on_video_ : on_audio_;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_ || stopping_ || failed_) {
        throw std::logic_error("AvMediaFilter 处理帧前需要成功 Start");
      }
      if (input.status != AvDecoderReadStatus::kFrame) return input;
      if (!input.frame || !input.frame->get()) {
        throw std::invalid_argument("AvMediaFilter 缺少输入帧");
      }
      if (!callback) return input;
      ++active_calls_;
    }
    // Save timing before invoking business code. A callback may return fresh
    // storage without copying properties; execution time must not become PTS.
    const auto* source = input.frame->get();
    const auto pts = source->pts;
    const auto dts = source->pkt_dts;
    const auto best_effort = source->best_effort_timestamp;
    const auto time_base = source->time_base;
    const auto duration = source->duration;
    try {
      auto output = callback(*input.frame);
      if (!output.get()) {
        throw std::invalid_argument("AvMediaFilter 回调返回了无效帧");
      }
      output->pts = pts;
      output->pkt_dts = dts;
      output->best_effort_timestamp = best_effort;
      output->time_base = time_base;
      output->duration = duration;
      input.frame = std::move(output);
    } catch (...) {
      // Do not resume using partially-updated business
      // history. Stop still releases the successfully-started session.
      FinishProcessing(true);
      throw;
    }
    FinishProcessing(false);
    return input;
  }

  OnStart on_start_;
  OnStop on_stop_;
  OnAudio on_audio_;
  OnVideo on_video_;

 private:
  void FinishProcessing(bool failed) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = failed_ || failed;
    if (--active_calls_ == 0) idle_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable idle_;
  std::size_t active_calls_ = 0;
  bool started_ = false;
  bool stopping_ = false;
  bool failed_ = false;
};

AvMediaFilter::AvMediaFilter() : impl_(std::make_unique<Impl>()) {}
AvMediaFilter::~AvMediaFilter() = default;
void AvMediaFilter::SetOnStart(OnStart callback) {
  impl_->Configure(impl_->on_start_, std::move(callback));
}
void AvMediaFilter::SetOnStop(OnStop callback) {
  impl_->Configure(impl_->on_stop_, std::move(callback));
}
void AvMediaFilter::SetOnAudio(OnAudio callback) {
  impl_->Configure(impl_->on_audio_, std::move(callback));
}
void AvMediaFilter::SetOnVideo(OnVideo callback) {
  impl_->Configure(impl_->on_video_, std::move(callback));
}
void AvMediaFilter::Start() { impl_->Start(); }
void AvMediaFilter::Stop() { impl_->Stop(); }
AvDecoderReadResult AvMediaFilter::ProcessVideo(AvDecoderReadResult input) {
  return impl_->Process(std::move(input), true);
}
AvDecoderReadResult AvMediaFilter::ProcessAudio(AvDecoderReadResult input) {
  return impl_->Process(std::move(input), false);
}

}  // namespace mw::streamer
