#include "mw/streamer/recorder/recorder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/platform/thread.h"
#include "streamer/recorder/mp4_file_writer.h"

namespace mw::streamer {
namespace {

void Validate(const std::vector<ffmpeg::StreamInfo>& streams,
              const RecorderConfig& config) {
  if (config.path.empty() || config.path.find('\0') != std::string::npos ||
      config.path.find("://") != std::string::npos ||
      std::filesystem::u8path(config.path).extension() != ".mp4" ||
      config.max_queue_bytes == 0) {
    throw std::invalid_argument("Recorder 需要本地 .mp4 路径和非零队列上限");
  }
  if (streams.empty() || streams.size() > 2)
    throw std::invalid_argument("Recorder 需要一至两条音视频轨道");
  bool video = false;
  bool audio = false;
  for (std::size_t i = 0; i < streams.size(); ++i) {
    streams[i].Validate();
    const auto* codec = streams[i].codec_parameters.get();
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO && !video &&
        (codec->codec_id == AV_CODEC_ID_H264 ||
         codec->codec_id == AV_CODEC_ID_HEVC)) {
      video = true;
    } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO && !audio &&
               codec->codec_id == AV_CODEC_ID_AAC) {
      audio = true;
    } else {
      throw std::invalid_argument(
          "Recorder 仅支持单路 H264/H265 视频和 AAC 音频");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (streams[i].stream_index == streams[j].stream_index)
        throw std::invalid_argument("Recorder 轨道索引重复");
    }
  }
}

std::size_t PacketBytes(const AVPacket& packet) {
  std::size_t bytes = sizeof(AVPacket) + static_cast<std::size_t>(packet.size);
  for (int i = 0; i < packet.side_data_elems; ++i)
    bytes += packet.side_data[i].size;
  return bytes;
}

}  // namespace

class Recorder::Impl final {
 public:
  void SetOnError(OnError callback) {
    auto stored =
        callback ? std::make_shared<OnError>(std::move(callback)) : nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(stored);
  }

  bool Start(const std::vector<ffmpeg::StreamInfo>& streams,
             RecorderConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) return false;
    Validate(streams, config);
    streams_ = streams;
    config_ = std::move(config);
    status_ = {};
    status_.state = RecorderState::kRunning;
    last_warning_ = {};
    MW_LOG_DEBUG(
        "streamer",
        "Recorder 启动配置: instance={} path={} tracks={} max_queue_bytes={}",
        static_cast<const void*>(this), config_.path, streams_.size(),
        config_.max_queue_bytes);
    if (!worker_.Start([this] { Run(); })) {
      status_.state = RecorderState::kFailed;
      status_.error = "Recorder 无法创建写入线程";
      return false;
    }
    return true;
  }

  bool TrySubmitPacket(const ffmpeg::Packet& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (status_.state != RecorderState::kRunning) return false;
    const auto* raw = packet.get();
    if (!raw || raw->size <= 0 || !raw->data ||
        (raw->dts == AV_NOPTS_VALUE && raw->pts == AV_NOPTS_VALUE) ||
        std::none_of(streams_.begin(), streams_.end(), [&](const auto& info) {
          return info.stream_index == raw->stream_index;
        })) {
      throw std::invalid_argument("Recorder 编码包、时间戳或轨道索引无效");
    }
    const auto bytes = PacketBytes(*raw);
    if (bytes > config_.max_queue_bytes - status_.queue_bytes) {
      const auto dropped = ++status_.dropped_packets;
      const auto now = std::chrono::steady_clock::now();
      const bool warn =
          dropped == 1 || now - last_warning_ >= std::chrono::seconds(5);
      if (warn) last_warning_ = now;
      lock.unlock();
      if (warn)
        MW_LOG_WARNING("streamer",
                       "Recorder 队列已满，丢弃当前包: instance={} dropped={}",
                       static_cast<const void*>(this), dropped);
      return false;
    }
    queue_.push_back(packet.Ref());
    status_.queue_bytes += bytes;
    MW_LOG_TRACE("streamer",
                 "Recorder 投递包: instance={} stream={} pts={} dts={} "
                 "time_base={}/{} size={} queue={} queue_bytes={}",
                 static_cast<const void*>(this), raw->stream_index, raw->pts,
                 raw->dts, raw->time_base.num, raw->time_base.den, raw->size,
                 queue_.size(), status_.queue_bytes);
    lock.unlock();
    ready_.notify_one();
    return true;
  }

  RecorderStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  void Drain() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state == RecorderState::kRunning) {
        status_.state = RecorderState::kStopping;
        MW_LOG_DEBUG("streamer",
                     "Recorder 开始排空: instance={} queue={} queue_bytes={}",
                     static_cast<const void*>(this), queue_.size(),
                     status_.queue_bytes);
      }
    }
    ready_.notify_one();
  }

  void Stop() noexcept {
    Drain();
    worker_.Join();
  }

 private:
  void Run() noexcept {
    try {
      internal::Mp4FileWriter writer(streams_, config_.path);
      MW_LOG_INFO("streamer", "Recorder 开始录像: instance={} path={}",
                  static_cast<const void*>(this), config_.path);
      for (;;) {
        std::optional<ffmpeg::Packet> packet;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          ready_.wait(lock, [this] {
            return !queue_.empty() || status_.state != RecorderState::kRunning;
          });
          if (queue_.empty()) break;
          status_.queue_bytes -= PacketBytes(*queue_.front().get());
          packet.emplace(std::move(queue_.front()));
          queue_.pop_front();
          MW_LOG_TRACE("streamer",
                       "Recorder 开始写包: instance={} stream={} pts={} dts={} "
                       "time_base={}/{} size={} queue={} queue_bytes={}",
                       static_cast<const void*>(this), (*packet)->stream_index,
                       (*packet)->pts, (*packet)->dts, (*packet)->time_base.num,
                       (*packet)->time_base.den, (*packet)->size, queue_.size(),
                       status_.queue_bytes);
        }
        writer.Write(std::move(*packet));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++status_.written_packets;
          MW_LOG_TRACE("streamer",
                       "Recorder 完成写包: instance={} written={} queue={} "
                       "queue_bytes={}",
                       static_cast<const void*>(this), status_.written_packets,
                       queue_.size(), status_.queue_bytes);
        }
      }
      writer.Finish();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = RecorderState::kStopped;
        MW_LOG_DEBUG("streamer",
                     "Recorder 排空完成: instance={} written={} dropped={} "
                     "queue_bytes={}",
                     static_cast<const void*>(this), status_.written_packets,
                     status_.dropped_packets, status_.queue_bytes);
      }
      MW_LOG_INFO("streamer", "Recorder 结束录像: instance={} path={}",
                  static_cast<const void*>(this), config_.path);
    } catch (const std::exception& error) {
      Fail(std::current_exception(), error.what());
    } catch (...) {
      Fail(std::current_exception(), "Recorder 未知写入错误");
    }
  }

  void Fail(std::exception_ptr exception, const char* error) noexcept {
    std::deque<ffmpeg::Packet> discarded;
    std::shared_ptr<OnError> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = RecorderState::kFailed;
      status_.error = error;
      status_.queue_bytes = 0;
      queue_.swap(discarded);
      MW_LOG_DEBUG(
          "streamer",
          "Recorder 失败清理: instance={} discarded={} written={} dropped={}",
          static_cast<const void*>(this), discarded.size(),
          status_.written_packets, status_.dropped_packets);
      callback = on_error_;
    }
    discarded.clear();
    MW_LOG_ERROR("streamer", "Recorder 录像失败: instance={} path={} error={}",
                 static_cast<const void*>(this), config_.path, error);
    if (callback) {
      try {
        (*callback)(std::move(exception));
      } catch (const std::exception& callback_error) {
        MW_LOG_WARNING("streamer", "Recorder OnError 回调异常: {}",
                       callback_error.what());
      } catch (...) {
        MW_LOG_WARNING("streamer", "Recorder OnError 回调发生未知异常");
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<ffmpeg::Packet> queue_;
  std::vector<ffmpeg::StreamInfo> streams_;
  RecorderConfig config_;
  RecorderStatus status_;
  std::shared_ptr<OnError> on_error_;
  std::chrono::steady_clock::time_point last_warning_{};
  Thread worker_;
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { Stop(); }

void Recorder::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}

bool Recorder::Start(const std::vector<ffmpeg::StreamInfo>& streams,
                     RecorderConfig config) {
  return impl_->Start(streams, std::move(config));
}

bool Recorder::TrySubmitPacket(const ffmpeg::Packet& packet) {
  return impl_->TrySubmitPacket(packet);
}

RecorderStatus Recorder::status() const { return impl_->status(); }

void Recorder::Drain() noexcept { impl_->Drain(); }

void Recorder::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
