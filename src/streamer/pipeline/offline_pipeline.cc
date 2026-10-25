#include "mw/streamer/pipeline/offline_pipeline.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/platform/thread.h"
#include "mw/streamer/platform/wait_signal.h"

namespace mw::streamer {
namespace {

AvMediaSourceConfig MakeSourceConfig(OfflinePipelineConfig config) {
  AvMediaSourceConfig source;
  source.proxy.input = std::move(config.input);
  source.proxy.loop = false;
  source.proxy.max_retries = 0;
  source.video_decoder = std::move(config.video_decoder);
  source.audio_decoder = std::move(config.audio_decoder);
  return source;
}

std::string LocalPath(std::string_view path) {
  if (path.empty() || path.find('\0') != std::string_view::npos ||
      path.find("://") != std::string_view::npos)
    throw std::invalid_argument("OfflinePipeline 需要本地文件路径");
  // An absolute path also prevents a colon in a local filename from selecting
  // an FFmpeg input protocol.
  const auto absolute =
      std::filesystem::absolute(std::filesystem::u8path(path));
  if (!std::filesystem::is_regular_file(absolute))
    throw std::invalid_argument("OfflinePipeline 输入必须为存在的本地普通文件");
  return absolute.generic_u8string();
}

}  // namespace

class OfflinePipeline::Impl final {
 public:
  Impl(const ffmpeg::HwDeviceContext& device, OfflinePipelineConfig config)
      : source_(device, MakeSourceConfig(std::move(config))) {
    source_.SetReadSignal(signal_);
    source_.SetOnStateChanged([this](const AvMediaSourceStatus& status) {
      if (status.state == AvMediaSourceState::kFailed) {
        Fail(status.error);
        return;
      }
      {
        std::lock_guard lock(mutex_);
        if (started_ && !stopping_ && !error_) {
          if (status.state == AvMediaSourceState::kRunning)
            state_ = OfflinePipelineState::kRunning;
          else if (status.state == AvMediaSourceState::kDraining)
            state_ = OfflinePipelineState::kDraining;
        }
      }
      signal_->Notify();
    });
    source_.SetOnPacket([this](const ffmpeg::Packet& packet) {
      if (on_input_packet_) on_input_packet_(packet);
    });
  }

  ~Impl() { Stop(); }

  AvMediaFilter& media_filter() {
    CheckStopped();
    return filter_;
  }
  void SetOnInputPacket(OnPacket callback) {
    CheckStopped();
    on_input_packet_ = std::move(callback);
  }
  void SetOnError(OnError callback) {
    CheckStopped();
    on_error_ = std::move(callback);
  }

  bool Start(std::string_view path) {
    {
      std::lock_guard lock(mutex_);
      if (started_) return false;
      started_ = true;
      stopping_ = ended_ = false;
      error_ = {};
      state_ = OfflinePipelineState::kStarting;
    }
    try {
      const auto local = LocalPath(path);
      MW_LOG_INFO("streamer", "OfflinePipeline[{}] 开始启动", fmt::ptr(this));
      if (!worker_.Start([this, local] { Run(local); }))
        throw std::runtime_error("OfflinePipeline 无法创建消费线程");
      return true;
    } catch (...) {
      Fail(std::current_exception());
      Stop();
      return false;
    }
  }

  OfflinePipelineState state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }
  AvMediaSourceStatus input_status() const { return source_.status(); }
  OfflinePipelineStatus status() const {
    OfflinePipelineStatus result;
    {
      std::lock_guard lock(mutex_);
      result.state = state_;
      result.error = error_;
    }
    result.input = input_status();
    return result;
  }
  std::exception_ptr error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

  void RequestStop() noexcept {
    bool requested;
    {
      std::lock_guard lock(mutex_);
      requested = started_ && !stopping_;
      stopping_ = true;
      if (started_ && !error_ && !ended_)
        state_ = OfflinePipelineState::kStopping;
    }
    if (requested)
      MW_LOG_DEBUG("streamer", "OfflinePipeline[{}] 请求停止", fmt::ptr(this));
    source_.RequestStop();
    signal_->Notify();
  }

  void Stop() noexcept {
    bool was_started;
    {
      std::lock_guard lock(mutex_);
      was_started = started_;
    }
    RequestStop();
    worker_.Join();
    source_.Stop();
    std::lock_guard lock(mutex_);
    started_ = false;
    state_ = error_   ? OfflinePipelineState::kFailed
             : ended_ ? OfflinePipelineState::kEnded
                      : OfflinePipelineState::kStopped;
    if (was_started)
      MW_LOG_INFO("streamer", "OfflinePipeline[{}] 停止完成: state={}",
                  fmt::ptr(this), static_cast<int>(state_));
  }

 private:
  void CheckStopped() const {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("配置 OfflinePipeline 前需要 Stop");
  }
  bool Stopping() const {
    std::lock_guard lock(mutex_);
    return stopping_;
  }
  bool Consume() {
    std::array<bool, 2> eof{};
    while (!Stopping()) {
      const auto observed = signal_->GetRevision();
      const auto info = source_.info();
      bool progress = false;
      if (info) {
        const std::array<bool, 2> present{info->video_stream_index >= 0,
                                          info->audio_stream_index >= 0};
        for (std::size_t track = 0; track < present.size(); ++track) {
          if (!present[track] || eof[track] || Stopping()) continue;
          auto input =
              track == 0 ? source_.TryReadVideo() : source_.TryReadAudio();
          if (input.status == AvDecoderReadStatus::kNull) continue;
          progress = true;
          if (input.status == AvDecoderReadStatus::kEof) {
            eof[track] = true;
            MW_LOG_DEBUG("streamer",
                         "OfflinePipeline[{}] 轨道处理完成: track={} stream={} "
                         "end_pts={} time_base={}/{}",
                         fmt::ptr(this), track == 0 ? "video" : "audio",
                         input.stream_index, input.end_pts, input.time_base.num,
                         input.time_base.den);
          } else if (track == 0)
            filter_.ConsumeVideo(std::move(input));
          else
            filter_.ConsumeAudio(std::move(input));
        }
        if ((present[0] || present[1]) && (!present[0] || eof[0]) &&
            (!present[1] || eof[1]))
          return !Stopping();
      }
      if (!progress) {
        if (Stopping()) return false;
        signal_->Wait(observed);
      }
    }
    return false;
  }

  void Run(const std::string& path) noexcept {
    bool completed = false;
    try {
      MW_LOG_DEBUG("streamer", "OfflinePipeline[{}] 启动业务滤镜",
                   fmt::ptr(this));
      filter_.Start();
      if (!Stopping()) {
        if (!source_.Start(path))
          throw std::runtime_error("OfflinePipeline MediaSource 启动失败");
        // Source Start resets cancellation; preserve an earlier stop request.
        if (Stopping()) source_.RequestStop();
        MW_LOG_INFO("streamer", "OfflinePipeline[{}] 消费线程已启动",
                    fmt::ptr(this));
      }
      if (Consume()) {
        MW_LOG_DEBUG("streamer",
                     "OfflinePipeline[{}] 全部轨道 EOF，开始重置业务滤镜",
                     fmt::ptr(this));
        filter_.Reset();
        MW_LOG_DEBUG("streamer",
                     "OfflinePipeline[{}] 业务滤镜重置完成，等待输入清理",
                     fmt::ptr(this));
        // Consuming the last EOF acknowledges completion to MediaSource.
        // Wait for its final state before Stop can turn it into cancellation.
        for (;;) {
          const auto observed = signal_->GetRevision();
          if (Stopping()) break;
          if (source_.status().state == AvMediaSourceState::kEnded) {
            completed = true;
            break;
          }
          signal_->Wait(observed);
        }
      }
    } catch (...) {
      Fail(std::current_exception());
    }
    MW_LOG_DEBUG("streamer", "OfflinePipeline[{}] 清理输入和业务滤镜",
                 fmt::ptr(this));
    source_.Stop();
    try {
      filter_.Stop();
    } catch (...) {
      Fail(std::current_exception());
    }
    OfflinePipelineState final_state;
    {
      std::lock_guard lock(mutex_);
      ended_ = completed && !stopping_ && !error_;
      state_ = error_   ? OfflinePipelineState::kFailed
               : ended_ ? OfflinePipelineState::kEnded
                        : OfflinePipelineState::kStopped;
      final_state = state_;
    }
    MW_LOG_INFO("streamer",
                "OfflinePipeline[{}] 消费线程结束: completed={} state={}",
                fmt::ptr(this), completed, static_cast<int>(final_state));
  }

  void Fail(std::exception_ptr error) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (!started_ || error_) return;
      error_ = error;
      state_ = OfflinePipelineState::kFailed;
    }
    RequestStop();
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer", "OfflinePipeline 不可恢复错误: {}",
                   failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "OfflinePipeline 不可恢复错误: 未知异常");
    }
    if (on_error_) {
      try {
        on_error_(error);
      } catch (const std::exception& failure) {
        MW_LOG_WARNING("streamer", "OfflinePipeline OnError 回调异常: {}",
                       failure.what());
      } catch (...) {
        MW_LOG_WARNING("streamer", "OfflinePipeline OnError 回调发生未知异常");
      }
    }
  }

  mutable std::mutex mutex_;
  std::shared_ptr<internal::WaitSignal> signal_ =
      std::make_shared<internal::WaitSignal>();
  OfflinePipelineState state_ = OfflinePipelineState::kStopped;
  bool started_ = false;
  bool stopping_ = false;
  bool ended_ = false;
  std::exception_ptr error_;
  OnPacket on_input_packet_;
  OnError on_error_;
  AvMediaSource source_;
  AvMediaFilter filter_;
  internal::Thread worker_;
};

OfflinePipeline::OfflinePipeline(OfflinePipelineConfig config)
    : OfflinePipeline(ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCpu),
                      std::move(config)) {}
OfflinePipeline::OfflinePipeline(const ffmpeg::HwDeviceContext& device,
                                 OfflinePipelineConfig config)
    : impl_(std::make_unique<Impl>(device, std::move(config))) {}
OfflinePipeline::~OfflinePipeline() = default;
AvMediaFilter& OfflinePipeline::media_filter() { return impl_->media_filter(); }
void OfflinePipeline::SetOnInputPacket(OnPacket callback) {
  impl_->SetOnInputPacket(std::move(callback));
}
void OfflinePipeline::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
bool OfflinePipeline::Start(std::string_view path) {
  return impl_->Start(path);
}
OfflinePipelineState OfflinePipeline::state() const { return impl_->state(); }
AvMediaSourceStatus OfflinePipeline::input_status() const {
  return impl_->input_status();
}
OfflinePipelineStatus OfflinePipeline::status() const {
  return impl_->status();
}
std::exception_ptr OfflinePipeline::error() const { return impl_->error(); }
void OfflinePipeline::RequestStop() noexcept { impl_->RequestStop(); }
void OfflinePipeline::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
