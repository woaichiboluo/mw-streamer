#include "mw/streamer/input/av_input.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/av_dictionary.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;

const char* StateName(AvInputState state) {
  switch (state) {
    case AvInputState::kClosed:
      return "closed";
    case AvInputState::kOpening:
      return "opening";
    case AvInputState::kOpened:
      return "opened";
    case AvInputState::kEnded:
      return "ended";
    case AvInputState::kFailed:
      return "failed";
    case AvInputState::kCancelled:
      return "cancelled";
  }
  return "unknown";
}

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

}  // namespace

class AvInput::Impl final {
 public:
  explicit Impl(AvInputConfig config) : config_(std::move(config)) {
    if (config_.packet_capacity == 0 || config_.open_timeout.count() <= 0 ||
        config_.read_timeout.count() <= 0) {
      throw std::invalid_argument("AvInput 缓存包数量和超时必须为正数");
    }
  }

  ~Impl() { Close(); }

  void SetOnStateChanged(OnStateChanged callback) {
    if (status().state != AvInputState::kClosed) {
      throw std::logic_error("配置 AvInput 回调前需要 Close");
    }
    on_state_changed_ = std::move(callback);
  }

  void Open(std::string_view url) {
    if (url.empty() || url.find('\0') != std::string_view::npos ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      throw std::invalid_argument("AvInput 地址不能为空或包含空字符");
    }
    if (status().state != AvInputState::kClosed) {
      throw std::logic_error("重新打开 AvInput 前需要 Close");
    }
    cancelled_.store(false);
    MW_LOG_DEBUG("streamer",
                 "AvInput[{}] 打开配置: 包容量={} 打开超时={}ms 读取超时={}ms "
                 "格式={} 选项数={}",
                 static_cast<const void*>(this), config_.packet_capacity,
                 config_.open_timeout.count(), config_.read_timeout.count(),
                 config_.input_format_name, config_.options.size());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      info_ = {};
    }
    Publish(AvInputState::kOpening);
    try {
      format_ = avformat_alloc_context();
      if (!format_) throw std::bad_alloc();
      format_->interrupt_callback = {&Impl::Interrupt, this};
      const AVInputFormat* input_format = nullptr;
      if (!config_.input_format_name.empty()) {
        input_format = av_find_input_format(config_.input_format_name.c_str());
        if (!input_format) {
          throw ffmpeg::FfmpegException(AVERROR_DEMUXER_NOT_FOUND,
                                        "av_find_input_format");
        }
      }
      ffmpeg::AvDictionary options(config_.options);
      const std::string address(url);
      BeginIo(config_.open_timeout);
      CheckIo(avformat_open_input(&format_, address.c_str(), input_format,
                                  options.address()),
              "avformat_open_input");
      if (log::ShouldLog("streamer", log::LogLevel::kWarning)) {
        for (const auto& [key, value] : options.ToMap()) {
          MW_LOG_WARNING("streamer", "AvInput[{}] 未使用输入选项: {}",
                         static_cast<const void*>(this), key);
        }
      }
      CheckIo(avformat_find_stream_info(format_, nullptr),
              "avformat_find_stream_info");
      deadline_ns_.store(0);
      AvInputInfo info;
      info.start_time_us = format_->start_time;
      info.duration_us = format_->duration;
      info.metadata = ffmpeg::AvDictionary::ToMap(format_->metadata);
      for (unsigned int index = 0; index < format_->nb_streams; ++index) {
        auto* stream = format_->streams[index];
        ffmpeg::StreamInfo description{
            stream->index, ffmpeg::CodecParameters(*stream->codecpar),
            stream->time_base};
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
          description.frame_rate =
              av_guess_frame_rate(format_, stream, nullptr);
          description.sample_aspect_ratio =
              av_guess_sample_aspect_ratio(format_, stream, nullptr);
        }
        description.disposition = stream->disposition;
        description.metadata = ffmpeg::AvDictionary::ToMap(stream->metadata);
        info.streams.push_back(std::move(description));
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            info.video_stream_index < 0) {
          info.video_stream_index = stream->index;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                   info.audio_stream_index < 0) {
          info.audio_stream_index = stream->index;
        }
      }
      MW_LOG_DEBUG(
          "streamer",
          "AvInput[{}] 轨道: 总数={} 视频={} 音频={} 起点={}us 时长={}us",
          static_cast<const void*>(this), info.streams.size(),
          info.video_stream_index, info.audio_stream_index, info.start_time_us,
          info.duration_us);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        info_ = std::move(info);
      }
      Publish(AvInputState::kOpened);
      if (!worker_.Start([this]() noexcept { Run(); })) {
        throw std::runtime_error("AvInput 工作线程启动失败");
      }
    } catch (...) {
      OpenFailed(std::current_exception());
      throw;
    }
  }

  AvInputReadResult Read(bool wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (wait) {
      changed_.wait(lock, [this] {
        return cancelled_.load() || !packets_.empty() ||
               (status_.state != AvInputState::kOpening &&
                status_.state != AvInputState::kOpened);
      });
    }
    if (cancelled_.load() || status_.state == AvInputState::kClosed ||
        status_.state == AvInputState::kCancelled) {
      return {AvInputReadStatus::kNull, {}};
    }
    if (!packets_.empty()) {
      auto packet = std::move(packets_.front());
      packets_.pop_front();
      const auto buffered = packets_.size();
      lock.unlock();
      changed_.notify_all();
      MW_LOG_TRACE("streamer",
                   "AvInput[{}] 消费包: 流={} PTS={} DTS={} 字节={} 剩余={}",
                   static_cast<const void*>(this), packet->stream_index,
                   packet->pts, packet->dts, packet->size, buffered);
      return {AvInputReadStatus::kPacket, std::move(packet)};
    }
    if (status_.state == AvInputState::kEnded) {
      return {AvInputReadStatus::kEof, {}};
    }
    return {AvInputReadStatus::kNull, {}};
  }

  void RequestCancel() noexcept {
    bool requested;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requested = !cancelled_.exchange(true);
    }
    changed_.notify_all();
    if (requested) {
      MW_LOG_DEBUG("streamer", "AvInput[{}] 请求取消",
                   static_cast<const void*>(this));
    }
  }

  void Close() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state == AvInputState::kClosed) return;
    }
    RequestCancel();
    worker_.Join();
    avformat_close_input(&format_);
    deadline_ns_.store(0);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      packets_.clear();
    }
    Publish(AvInputState::kClosed);
  }

  AvInputStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  AvInputInfo info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
  }

  std::size_t buffered_packets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_.size();
  }

 private:
  static int Interrupt(void* opaque) {
    const auto* self = static_cast<const Impl*>(opaque);
    const auto deadline = self->deadline_ns_.load();
    return self->cancelled_.load() || (deadline != 0 && NowNs() >= deadline);
  }

  void BeginIo(std::chrono::milliseconds timeout) {
    deadline_ns_.store(
        NowNs() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
  }

  void CheckIo(int code, const char* operation) const {
    if (cancelled_.load()) {
      throw ffmpeg::FfmpegException(AVERROR_EXIT, operation);
    }
    const auto deadline = deadline_ns_.load();
    if (deadline != 0 && NowNs() >= deadline) code = AVERROR(ETIMEDOUT);
    ffmpeg::FfmpegException::throwIfError(code, operation);
  }

  void Publish(AvInputState state, std::exception_ptr error = {}) {
    AvInputStatus snapshot{state, std::move(error)};
    AvInputState previous;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      previous = status_.state;
      status_ = snapshot;
    }
    changed_.notify_all();
    if (state == AvInputState::kOpening || state == AvInputState::kOpened ||
        state == AvInputState::kClosed) {
      MW_LOG_INFO("streamer", "AvInput[{}] 状态: {} -> {}",
                  static_cast<const void*>(this), StateName(previous),
                  StateName(state));
    } else {
      MW_LOG_DEBUG("streamer", "AvInput[{}] 状态: {} -> {}",
                   static_cast<const void*>(this), StateName(previous),
                   StateName(state));
    }
    if (snapshot.error && log::ShouldLog("streamer", log::LogLevel::kError)) {
      try {
        std::rethrow_exception(snapshot.error);
      } catch (const ffmpeg::FfmpegException& failure) {
        MW_LOG_ERROR("streamer", "AvInput[{}] 输入失败: 错误码={} {}",
                     static_cast<const void*>(this), failure.error_code(),
                     failure.what());
      } catch (const std::exception& failure) {
        MW_LOG_ERROR("streamer", "AvInput[{}] 输入失败: {}",
                     static_cast<const void*>(this), failure.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "AvInput[{}] 输入失败: 未知异常",
                     static_cast<const void*>(this));
      }
    }
    if (!on_state_changed_) return;
    try {
      on_state_changed_(snapshot);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer", "AvInput[{}] 状态通知异常: {}",
                   static_cast<const void*>(this), failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "AvInput[{}] 状态通知发生未知异常",
                   static_cast<const void*>(this));
    }
  }

  void OpenFailed(std::exception_ptr error) {
    deadline_ns_.store(0);
    avformat_close_input(&format_);
    if (cancelled_.load()) {
      Publish(AvInputState::kCancelled);
    } else {
      Publish(AvInputState::kFailed, std::move(error));
    }
  }

  void Run() noexcept {
    try {
      for (;;) {
        const bool trace = log::ShouldLog("streamer", log::LogLevel::kTrace);
        {
          std::unique_lock<std::mutex> lock(mutex_);
          if (trace && !cancelled_.load() &&
              packets_.size() >= config_.packet_capacity) {
            const auto buffered = packets_.size();
            lock.unlock();
            MW_LOG_TRACE("streamer", "AvInput[{}] 包队列已满: {}/{}",
                         static_cast<const void*>(this), buffered,
                         config_.packet_capacity);
            lock.lock();
          }
          changed_.wait(lock, [this] {
            return cancelled_.load() ||
                   packets_.size() < config_.packet_capacity;
          });
          if (cancelled_.load()) break;
        }
        ffmpeg::Packet packet;
        if (deadline_ns_.load() == 0) BeginIo(config_.read_timeout);
        const int code = av_read_frame(format_, packet.get());
        if (cancelled_.load()) break;
        CheckIo(code == AVERROR(EAGAIN) || code == AVERROR_EOF ? 0 : code,
                "av_read_frame");
        if (code == AVERROR_EOF) {
          deadline_ns_.store(0);
          MW_LOG_DEBUG("streamer", "AvInput[{}] 源 EOF，等待包队列排空",
                       static_cast<const void*>(this));
          {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(
                lock, [this] { return cancelled_.load() || packets_.empty(); });
            if (cancelled_.load()) break;
          }
          Publish(AvInputState::kEnded);
          return;
        }
        if (code == AVERROR(EAGAIN)) {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait_for(lock, std::chrono::milliseconds(5),
                            [this] { return cancelled_.load(); });
          continue;
        }
        deadline_ns_.store(0);
        if (packet->stream_index != info_.video_stream_index &&
            packet->stream_index != info_.audio_stream_index) {
          continue;
        }
        const int stream_index = packet->stream_index;
        const auto pts = packet->pts;
        const auto dts = packet->dts;
        const int bytes = packet->size;
        std::size_t buffered;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (cancelled_.load()) break;
          packets_.push_back(std::move(packet));
          buffered = packets_.size();
        }
        changed_.notify_all();
        MW_LOG_TRACE(
            "streamer",
            "AvInput[{}] 投递包: 流={} PTS={} DTS={} 字节={} 缓存={}/{}",
            static_cast<const void*>(this), stream_index, pts, dts, bytes,
            buffered, config_.packet_capacity);
      }
    } catch (...) {
      FinishFailure(std::current_exception());
      return;
    }
    deadline_ns_.store(0);
    Publish(AvInputState::kCancelled);
  }

  void FinishFailure(std::exception_ptr error) {
    deadline_ns_.store(0);
    if (cancelled_.load()) {
      Publish(AvInputState::kCancelled);
    } else {
      Publish(AvInputState::kFailed, std::move(error));
    }
  }

  const AvInputConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::atomic<bool> cancelled_{false};
  std::atomic<std::int64_t> deadline_ns_{0};
  AvInputStatus status_;
  AvInputInfo info_;
  std::deque<ffmpeg::Packet> packets_;
  AVFormatContext* format_ = nullptr;
  internal::Thread worker_;
  OnStateChanged on_state_changed_;
};

AvInput::AvInput(AvInputConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
AvInput::~AvInput() = default;
void AvInput::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
void AvInput::Open(std::string_view url) { impl_->Open(url); }
AvInputReadResult AvInput::ReadPacket() { return impl_->Read(true); }
AvInputReadResult AvInput::TryReadPacket() { return impl_->Read(false); }
void AvInput::RequestCancel() noexcept { impl_->RequestCancel(); }
void AvInput::Close() noexcept { impl_->Close(); }
AvInputStatus AvInput::status() const { return impl_->status(); }
AvInputInfo AvInput::info() const { return impl_->info(); }
std::size_t AvInput::buffered_packets() const {
  return impl_->buffered_packets();
}

}  // namespace mw::streamer
