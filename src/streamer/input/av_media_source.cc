#include "mw/streamer/input/av_media_source.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

const ffmpeg::StreamInfo* FindStream(const AvInputInfo& info, int index) {
  if (index < 0) return nullptr;
  for (const auto& stream : info.streams) {
    if (stream.stream_index == index) return &stream;
  }
  throw std::invalid_argument("AvMediaSource 所选轨道不存在");
}

void ValidateDecoderConfig(const AvDecoderConfig& config) {
  if (config.packet_watermark == 0 || config.frame_capacity == 0 ||
      config.thread_count < 0 ||
      config.decoder_name.find('\0') != std::string::npos) {
    throw std::invalid_argument("AvMediaSource 解码配置无效");
  }
}

void Acknowledge(std::optional<std::uint64_t>& position,
                 std::uint64_t generation) {
  if (!position || *position < generation) position = generation;
}

const char* StateName(AvMediaSourceState state) {
  switch (state) {
    case AvMediaSourceState::kStopped:
      return "stopped";
    case AvMediaSourceState::kConnecting:
      return "connecting";
    case AvMediaSourceState::kRunning:
      return "running";
    case AvMediaSourceState::kWaitingRetry:
      return "waiting_retry";
    case AvMediaSourceState::kDraining:
      return "draining";
    case AvMediaSourceState::kEnded:
      return "ended";
    case AvMediaSourceState::kFailed:
      return "failed";
  }
  return "unknown";
}

}  // namespace

class AvMediaSource::Impl final {
 public:
  Impl(AvMediaSourceConfig config,
       const ffmpeg::HwDeviceContext* device = nullptr)
      : config_(std::move(config)), proxy_(config_.proxy) {
    ValidateDecoderConfig(config_.video_decoder);
    ValidateDecoderConfig(config_.audio_decoder);
    if (device) device_ = device->Ref();
    proxy_.SetOnStateChanged([this](const AvInputProxyStatus& status) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status.state == AvInputProxyState::kFailed && !failure_) {
          failure_ = status.error;
        }
        proxy_events_.push_back(status);
      }
      changed_.notify_all();
    });
  }

  ~Impl() { Stop(); }

  void SetOnStateChanged(OnStateChanged callback) {
    if (worker_.joinable()) {
      throw std::logic_error("配置 AvMediaSource 回调前需要 Stop");
    }
    on_state_changed_ = std::move(callback);
  }

  bool Start(std::string_view url) noexcept {
    if (url.empty() || url.find('\0') != std::string_view::npos ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      MW_LOG_ERROR("streamer", "AvMediaSource[{}] 启动失败: 地址无效",
                   static_cast<const void*>(this));
      return false;
    }
    if (worker_.joinable()) {
      MW_LOG_ERROR("streamer",
                   "AvMediaSource[{}] 启动失败: 重新启动前需要 Stop",
                   static_cast<const void*>(this));
      return false;
    }
    url_ = url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = false;
      failure_ = {};
      status_ = {};
      status_.state = AvMediaSourceState::kConnecting;
      proxy_events_.clear();
      video_eof_.reset();
      audio_eof_.reset();
      final_generation_.reset();
    }
    MW_LOG_INFO("streamer", "AvMediaSource [{}] 启动: loop={}",
                static_cast<const void*>(this), config_.proxy.loop);
    if (worker_.Start([this]() noexcept { Run(); })) return true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      failure_ = {};
      status_ = {};
      proxy_events_.clear();
      video_eof_.reset();
      audio_eof_.reset();
      final_generation_.reset();
    }
    url_.clear();
    changed_.notify_all();
    return false;
  }

  AvDecoderReadResult ReadFrame(bool video) {
    const auto session = ReadSession();
    if (!session) return {};
    auto* decoder = video ? session->video.get() : session->audio.get();
    if (!decoder) return {};
    auto result = decoder->TryReadFrame();
    if (result.status == AvDecoderReadStatus::kEof) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Acknowledge(video ? video_eof_ : audio_eof_, result.generation);
      }
      changed_.notify_all();
      MW_LOG_DEBUG("streamer",
                   "AvMediaSource [{}] 消费 EOF: stream={} generation={}",
                   static_cast<const void*>(this), result.stream_index,
                   result.generation);
    } else if (result.status == AvDecoderReadStatus::kFrame &&
               mw::log::ShouldLog("streamer", mw::log::LogLevel::kTrace)) {
      MW_LOG_TRACE("streamer",
                   "AvMediaSource [{}] 消费帧: stream={} generation={} "
                   "pts={} time_base={}/{}",
                   static_cast<const void*>(this), result.stream_index,
                   result.generation, result.frame->get()->pts,
                   result.time_base.num, result.time_base.den);
    }
    return result;
  }

  AvMediaSourceStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }
  std::shared_ptr<const AvInputInfo> info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.info;
  }

  void RequestStop() noexcept {
    std::shared_ptr<Session> session;
    bool requested = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requested = !stopping_;
      stopping_ = true;
      session = session_;
    }
    changed_.notify_all();
    proxy_.RequestStop();
    if (session) session->RequestStop();
    if (requested) {
      MW_LOG_DEBUG("streamer", "AvMediaSource [{}] 请求停止",
                   static_cast<const void*>(this));
    }
  }
  void Stop() noexcept {
    RequestStop();
    worker_.Join();
  }

 private:
  struct Session {
    ~Session() { Stop(); }
    void RequestStop() noexcept {
      if (video) video->RequestStop();
      if (audio) audio->RequestStop();
    }
    void Stop() noexcept {
      if (video) video->Stop();
      if (audio) audio->Stop();
    }
    std::shared_ptr<const AvInputInfo> description;
    std::unique_ptr<AvDecoder> video;
    std::unique_ptr<AvDecoder> audio;
  };

  std::shared_ptr<Session> ReadSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || failure_ || status_.state == AvMediaSourceState::kEnded) {
      return {};
    }
    return session_;
  }

  void Failed(std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failure_) failure_ = std::move(error);
    }
    changed_.notify_all();
  }

  void CheckFailure() const {
    std::exception_ptr failure;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failure = failure_;
    }
    if (failure) std::rethrow_exception(failure);
  }

  bool Stopping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
  }

  void Publish(AvMediaSourceState state,
               const AvInputProxyStatus* connection = nullptr,
               bool force = false) {
    AvMediaSourceStatus snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if ((stopping_ || failure_) && state != AvMediaSourceState::kFailed &&
          state != AvMediaSourceState::kStopped &&
          state != AvMediaSourceState::kEnded) {
        return;
      }
      auto next = status_;
      next.state = state;
      if (connection) {
        next.generation = connection->generation;
        next.retry_count = connection->retry_count;
        next.retry_delay = connection->retry_delay;
        next.error = connection->error;
        next.info = connection->info;
      } else if (state == AvMediaSourceState::kFailed) {
        next.error = failure_;
      } else {
        next.error = {};
        next.retry_count = 0;
        next.retry_delay = {};
      }
      if (!force && next.state == status_.state &&
          next.generation == status_.generation &&
          next.retry_count == status_.retry_count &&
          next.retry_delay == status_.retry_delay &&
          next.error == status_.error && next.info == status_.info)
        return;
      status_ = next;
      snapshot = status_;
    }
    if (state == AvMediaSourceState::kFailed &&
        mw::log::ShouldLog("streamer", mw::log::LogLevel::kError)) {
      try {
        if (snapshot.error) std::rethrow_exception(snapshot.error);
      } catch (const ffmpeg::FfmpegException& error) {
        MW_LOG_ERROR(
            "streamer",
            "AvMediaSource [{}] 终止失败: generation={} code={} error={}",
            static_cast<const void*>(this), snapshot.generation,
            error.error_code(), error.what());
      } catch (const std::exception& error) {
        MW_LOG_ERROR(
            "streamer", "AvMediaSource [{}] 终止失败: generation={} error={}",
            static_cast<const void*>(this), snapshot.generation, error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer",
                     "AvMediaSource [{}] 未知终止异常: generation={}",
                     static_cast<const void*>(this), snapshot.generation);
      }
    } else if (state == AvMediaSourceState::kRunning ||
               state == AvMediaSourceState::kEnded ||
               state == AvMediaSourceState::kStopped) {
      MW_LOG_INFO("streamer", "AvMediaSource [{}] 状态={}: generation={}",
                  static_cast<const void*>(this), StateName(state),
                  snapshot.generation);
    } else {
      MW_LOG_DEBUG("streamer",
                   "AvMediaSource [{}] 状态={}: generation={} retries={} "
                   "retry_delay_s={}",
                   static_cast<const void*>(this), StateName(state),
                   snapshot.generation, snapshot.retry_count,
                   snapshot.retry_delay.count());
    }
    if (!on_state_changed_) return;
    try {
      on_state_changed_(snapshot);
    } catch (const std::exception& observer_error) {
      MW_LOG_ERROR("streamer", "AvMediaSource [{}] 状态通知异常: {}",
                   static_cast<const void*>(this), observer_error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "AvMediaSource [{}] 状态通知发生未知异常",
                   static_cast<const void*>(this));
    }
  }

  void ValidateTracks(const Session& session, const AvInputInfo& info) const {
    const auto& original = *session.description;
    if (original.video_stream_index != info.video_stream_index ||
        original.audio_stream_index != info.audio_stream_index) {
      throw std::invalid_argument("AvMediaSource 重连后的所选轨道发生改变");
    }
    for (const auto index :
         {info.video_stream_index, info.audio_stream_index}) {
      if (index < 0) continue;
      const auto& previous = *FindStream(original, index);
      const auto& current = *FindStream(info, index);
      const auto* previous_codec = previous.codec_parameters.get();
      const auto* current_codec = current.codec_parameters.get();
      // FlushBuffers does not replace the decoder's codec configuration.
      // StreamInfo equality compares media layout, not codec initialization
      // data, so validate that data before reusing the existing context.
      if (!(previous == current) ||
          previous_codec->profile != current_codec->profile ||
          previous_codec->level != current_codec->level ||
          previous_codec->extradata_size != current_codec->extradata_size ||
          (previous_codec->extradata_size > 0 &&
           std::memcmp(previous_codec->extradata, current_codec->extradata,
                       static_cast<std::size_t>(
                           previous_codec->extradata_size)) != 0)) {
        throw std::invalid_argument("AvMediaSource 重连后的轨道格式发生改变");
      }
    }
  }

  void Initialize(const std::shared_ptr<const AvInputInfo>& info,
                  std::shared_ptr<Session>& session) {
    if (!info) throw std::logic_error("AvMediaSource 缺少已打开的输入描述");
    if (session) {
      ValidateTracks(*session, *info);
      return;
    }
    auto created = std::make_shared<Session>();
    created->description = info;
    const auto on_error = [this](std::exception_ptr error) {
      Failed(std::move(error));
    };
    if (const auto* stream = FindStream(*info, info->video_stream_index)) {
      created->video =
          device_ ? std::make_unique<AvDecoder>(*stream, *device_,
                                                config_.video_decoder)
                  : std::make_unique<AvDecoder>(*stream, config_.video_decoder);
      created->video->SetOnError(on_error);
    }
    if (const auto* stream = FindStream(*info, info->audio_stream_index)) {
      created->audio =
          std::make_unique<AvDecoder>(*stream, config_.audio_decoder);
      created->audio->SetOnError(on_error);
    }
    if (!created->video && !created->audio) {
      throw std::invalid_argument("AvMediaSource 输入没有所选音视频轨道");
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session_ = created;
    }
    session = std::move(created);
    MW_LOG_DEBUG(
        "streamer",
        "AvMediaSource [{}] 解码链已建立: video_stream={} audio_stream={}",
        static_cast<const void*>(this), info->video_stream_index,
        info->audio_stream_index);
  }

  bool ProcessEvents(std::shared_ptr<Session>& session) {
    std::deque<AvInputProxyStatus> events;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events.swap(proxy_events_);
    }
    bool progress = !events.empty();
    for (const auto& event : events) {
      CheckFailure();
      if (Stopping()) break;
      switch (event.state) {
        case AvInputProxyState::kConnecting:
          Publish(AvMediaSourceState::kConnecting, &event);
          break;
        case AvInputProxyState::kConnected: {
          std::lock_guard<std::mutex> lock(mutex_);
          status_.info = event.info;
          status_.generation = event.generation;
        }
          Initialize(event.info, session);
          Publish(AvMediaSourceState::kRunning, &event);
          break;
        case AvInputProxyState::kWaitingRetry:
          Publish(AvMediaSourceState::kWaitingRetry, &event);
          break;
        case AvInputProxyState::kFailed:
          CheckFailure();
          break;
        case AvInputProxyState::kEnded:
        case AvInputProxyState::kStopped:
          // Input EOF is not decoded EOF. Reads acknowledge final drain.
          break;
      }
    }
    return progress;
  }

  bool WantsPacket(const std::shared_ptr<Session>& session) const {
    return !session || (session->video && session->video->wants_packet()) ||
           (session->audio && session->audio->wants_packet());
  }

  bool Dispatch(std::shared_ptr<Session>& session) {
    auto result = proxy_.TryReadPacket();
    if (result.status == AvInputReadStatus::kNull) return false;
    Initialize(result.info, session);
    if (Stopping()) return false;
    if (result.status == AvInputReadStatus::kEof) {
      MW_LOG_DEBUG("streamer",
                   "AvMediaSource [{}] 分发源 EOF: generation={} loop={}",
                   static_cast<const void*>(this), result.generation,
                   config_.proxy.loop);
      for (auto* decoder : {session->video.get(), session->audio.get()}) {
        if (decoder && !decoder->PushEof(result.generation)) {
          if (Stopping()) return false;
          if (const auto failure = decoder->error())
            std::rethrow_exception(failure);
          throw std::logic_error("AvMediaSource Decoder 拒绝 EOF");
        }
      }
      if (!config_.proxy.loop) {
        final_generation_ = result.generation;
        Publish(AvMediaSourceState::kDraining);
      }
    } else {
      if (mw::log::ShouldLog("streamer", mw::log::LogLevel::kTrace)) {
        MW_LOG_TRACE("streamer",
                     "AvMediaSource [{}] 分发包: stream={} generation={} "
                     "pts={} dts={} size={}",
                     static_cast<const void*>(this),
                     result.packet->get()->stream_index, result.generation,
                     result.packet->get()->pts, result.packet->get()->dts,
                     result.packet->get()->size);
      }
      auto* decoder = result.packet->get()->stream_index ==
                              session->description->video_stream_index
                          ? session->video.get()
                          : session->audio.get();
      if (!decoder)
        throw std::logic_error("AvMediaSource Packet 缺少对应 Decoder");
      if (!decoder->PushPacket(std::move(*result.packet), result.generation)) {
        if (Stopping()) return false;
        if (const auto failure = decoder->error())
          std::rethrow_exception(failure);
        throw std::logic_error("AvMediaSource Decoder 拒绝 Packet");
      }
    }
    return true;
  }

  bool Drained(const std::shared_ptr<Session>& session) const {
    if (!final_generation_ || !session) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return (!session->video || video_eof_ == final_generation_) &&
           (!session->audio || audio_eof_ == final_generation_);
  }

  void Cleanup(std::shared_ptr<Session>& session) noexcept {
    proxy_.RequestStop();
    if (session) session->Stop();
    proxy_.Stop();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session_.reset();
      proxy_events_.clear();
    }
    session.reset();
    MW_LOG_DEBUG("streamer", "AvMediaSource [{}] 媒体处理链已释放",
                 static_cast<const void*>(this));
  }

  void Run() noexcept {
    std::shared_ptr<Session> session;
    bool ended = false;
    try {
      // Start publishes an immediate snapshot; notify it on this worker too.
      Publish(AvMediaSourceState::kConnecting, nullptr, true);
      if (!Stopping() && !proxy_.Start(url_)) {
        throw std::runtime_error("AvMediaSource 无法启动输入代理");
      }
      while (!Stopping()) {
        CheckFailure();
        bool progress = ProcessEvents(session);
        CheckFailure();
        if (Stopping()) break;
        if (Drained(session)) {
          ended = true;
          break;
        }
        if (!final_generation_ && WantsPacket(session))
          progress |= Dispatch(session);
        if (!progress) {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait_for(lock, std::chrono::milliseconds(1), [this] {
            return stopping_ || failure_ || !proxy_events_.empty();
          });
        }
      }
    } catch (...) {
      Failed(std::current_exception());
    }
    std::exception_ptr failure;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failure = failure_;
    }
    if (failure) Publish(AvMediaSourceState::kFailed);
    Cleanup(session);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failure = failure_;
    }
    if (failure)
      Publish(AvMediaSourceState::kFailed);
    else
      Publish(ended ? AvMediaSourceState::kEnded
                    : AvMediaSourceState::kStopped);
  }

  AvMediaSourceConfig config_;
  std::optional<ffmpeg::HwDeviceContext> device_;
  AvInputProxy proxy_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::exception_ptr failure_;
  AvMediaSourceStatus status_;
  std::deque<AvInputProxyStatus> proxy_events_;
  std::shared_ptr<Session> session_;
  std::optional<std::uint64_t> video_eof_;
  std::optional<std::uint64_t> audio_eof_;
  // Written only by the dispatcher after successfully submitting packet EOF.
  std::optional<std::uint64_t> final_generation_;
  OnStateChanged on_state_changed_;
  std::string url_;
  internal::Thread worker_;
};

AvMediaSource::AvMediaSource(AvMediaSourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
AvMediaSource::AvMediaSource(const ffmpeg::HwDeviceContext& device,
                             AvMediaSourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config), &device)) {}
AvMediaSource::~AvMediaSource() = default;
void AvMediaSource::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
bool AvMediaSource::Start(std::string_view url) noexcept {
  return impl_->Start(url);
}
AvDecoderReadResult AvMediaSource::TryReadVideo() {
  return impl_->ReadFrame(true);
}
AvDecoderReadResult AvMediaSource::TryReadAudio() {
  return impl_->ReadFrame(false);
}
AvMediaSourceStatus AvMediaSource::status() const { return impl_->status(); }
std::shared_ptr<const AvInputInfo> AvMediaSource::info() const {
  return impl_->info();
}
void AvMediaSource::RequestStop() noexcept { impl_->RequestStop(); }
void AvMediaSource::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
