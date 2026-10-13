#include "mw/streamer/input/av_media_input.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer {
namespace {

const ffmpeg::StreamInfo* FindStream(const AvInputInfo& info, int index) {
  if (index < 0) return nullptr;
  for (const auto& stream : info.streams) {
    if (stream.stream_index == index) return &stream;
  }
  throw std::invalid_argument("AvMediaInput 所选轨道不存在");
}

void ValidateDecoderConfig(const AvDecoderConfig& config) {
  if (config.packet_watermark == 0 || config.frame_capacity == 0 ||
      config.thread_count < 0 ||
      config.decoder_name.find('\0') != std::string::npos) {
    throw std::invalid_argument("AvMediaInput 解码配置无效");
  }
}

void Acknowledge(std::optional<std::uint64_t>& position,
                 std::uint64_t generation) {
  if (!position || *position < generation) position = generation;
}

}  // namespace

class AvMediaInput::Impl final {
 public:
  Impl(AvMediaInputConfig config,
       const ffmpeg::HwDeviceContext* device = nullptr)
      : config_(std::move(config)), proxy_(config_.proxy) {
    if (config_.mode != AvMediaInputMode::kBatch &&
        config_.mode != AvMediaInputMode::kLive) {
      throw std::invalid_argument("AvMediaInput 模式无效");
    }
    ValidateDecoderConfig(config_.video_decoder);
    ValidateDecoderConfig(config_.audio_decoder);
    if (config_.mode == AvMediaInputMode::kLive) {
      if (config_.output.fps.num <= 0 || config_.output.fps.den <= 0 ||
          config_.output.audio_sample_rate <= 0 ||
          !av_get_sample_fmt_name(config_.output.audio_sample_format) ||
          !av_channel_layout_check(&config_.output.audio_channel_layout)) {
        throw std::invalid_argument("AvMediaInput Live 输出配置无效");
      }
      ffmpeg::FfmpegException::throwIfError(
          av_channel_layout_copy(&output_audio_.get()->ch_layout,
                                 &config_.output.audio_channel_layout),
          "复制 AvMediaInput 输出声道布局");
      config_.output.audio_channel_layout = output_audio_.get()->ch_layout;
    }
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
      throw std::logic_error("配置 AvMediaInput 回调前需要 Stop");
    }
    on_state_changed_ = std::move(callback);
  }

  void Start(std::string_view url) {
    if (url.empty() || url.find('\0') != std::string_view::npos ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      throw std::invalid_argument("AvMediaInput 地址无效");
    }
    if (worker_.joinable()) {
      throw std::logic_error("重新启动 AvMediaInput 前需要 Stop");
    }
    url_ = url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = false;
      failure_ = {};
      status_ = {};
      status_.state = AvMediaInputState::kConnecting;
      proxy_events_.clear();
      video_eof_.reset();
      audio_eof_.reset();
      live_eof_.reset();
      final_generation_.reset();
    }
    try {
      worker_ = std::thread(&Impl::Run, this);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      failure_ = std::current_exception();
      status_.state = AvMediaInputState::kFailed;
      status_.error = failure_;
      throw;
    }
  }

  AvDecoderReadResult ReadBatch(bool video) {
    if (config_.mode != AvMediaInputMode::kBatch) {
      throw std::logic_error("AvMediaInput Live 模式只能读取配组后的帧");
    }
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
    }
    return result;
  }

  AvFrameSourceReadResult ReadLive() {
    if (config_.mode != AvMediaInputMode::kLive) {
      throw std::logic_error("AvMediaInput Batch 模式需要分别读取音视频");
    }
    const auto session = ReadSession();
    if (!session || !session->source) return {};
    auto result = session->source->TryReadFrame();
    if (result.status == AvFrameSourceReadStatus::kEof) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        Acknowledge(live_eof_, result.generation);
      }
      changed_.notify_all();
    }
    return result;
  }

  AvMediaInputMode mode() const { return config_.mode; }
  AvMediaInputStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }
  std::shared_ptr<const AvInputInfo> info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.info;
  }

  void RequestStop() noexcept {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      session = session_;
    }
    changed_.notify_all();
    proxy_.RequestStop();
    if (session) session->RequestStop();
  }
  void Stop() noexcept {
    RequestStop();
    if (worker_.joinable()) worker_.join();
  }

 private:
  struct Session {
    ~Session() { Stop(); }
    void RequestStop() noexcept {
      if (source) source->RequestStop();
      if (video) video->RequestStop();
      if (audio) audio->RequestStop();
    }
    void Stop() noexcept {
      if (source) source->Stop();
      if (video) video->Stop();
      if (audio) audio->Stop();
    }
    std::shared_ptr<const AvInputInfo> description;
    std::unique_ptr<AvDecoder> video;
    std::unique_ptr<AvDecoder> audio;
    std::unique_ptr<AvFrameSource> source;
  };

  std::shared_ptr<Session> ReadSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || failure_ || status_.state == AvMediaInputState::kEnded) {
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

  void Publish(AvMediaInputState state,
               const AvInputProxyStatus* connection = nullptr,
               bool force = false) {
    AvMediaInputStatus snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if ((stopping_ || failure_) && state != AvMediaInputState::kFailed &&
          state != AvMediaInputState::kStopped &&
          state != AvMediaInputState::kEnded) {
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
      } else if (state == AvMediaInputState::kFailed) {
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
    if (!on_state_changed_) return;
    try {
      on_state_changed_(snapshot);
    } catch (const std::exception& observer_error) {
      MW_LOG_ERROR("streamer", "AvMediaInput 状态通知异常: {}",
                   observer_error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "AvMediaInput 状态通知发生未知异常");
    }
  }

  void ValidateTracks(const Session& session, const AvInputInfo& info) const {
    const auto& original = *session.description;
    if (original.video_stream_index != info.video_stream_index ||
        original.audio_stream_index != info.audio_stream_index) {
      throw std::invalid_argument("AvMediaInput 重连后的所选轨道发生改变");
    }
    for (const auto index :
         {info.video_stream_index, info.audio_stream_index}) {
      if (index >= 0 &&
          !(*FindStream(original, index) == *FindStream(info, index))) {
        throw std::invalid_argument("AvMediaInput 重连后的轨道格式发生改变");
      }
    }
  }

  void Initialize(const std::shared_ptr<const AvInputInfo>& info,
                  std::shared_ptr<Session>& session) {
    if (!info) throw std::logic_error("AvMediaInput 缺少已打开的输入描述");
    if (session) {
      ValidateTracks(*session, *info);
      return;
    }
    auto created = std::make_shared<Session>();
    created->description = info;
    const auto on_failed = [this](std::exception_ptr error) {
      Failed(std::move(error));
    };
    if (const auto* stream = FindStream(*info, info->video_stream_index)) {
      created->video =
          device_ ? std::make_unique<AvDecoder>(
                        *stream, *device_, config_.video_decoder, on_failed)
                  : std::make_unique<AvDecoder>(*stream, config_.video_decoder,
                                                on_failed);
    }
    if (const auto* stream = FindStream(*info, info->audio_stream_index)) {
      created->audio = std::make_unique<AvDecoder>(
          *stream, config_.audio_decoder, on_failed);
    }
    if (!created->video && !created->audio) {
      throw std::invalid_argument("AvMediaInput 输入没有所选音视频轨道");
    }
    if (config_.mode == AvMediaInputMode::kLive) {
      created->source = std::make_unique<AvFrameSource>(
          created->video.get(), created->audio.get(), config_.output,
          info->start_time_us, on_failed);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session_ = created;
    }
    session = std::move(created);
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
          Publish(AvMediaInputState::kConnecting, &event);
          break;
        case AvInputProxyState::kConnected: {
          std::lock_guard<std::mutex> lock(mutex_);
          status_.info = event.info;
          status_.generation = event.generation;
        }
          Initialize(event.info, session);
          Publish(AvMediaInputState::kRunning, &event);
          break;
        case AvInputProxyState::kWaitingRetry:
          Publish(AvMediaInputState::kWaitingRetry, &event);
          break;
        case AvInputProxyState::kFailed:
          CheckFailure();
          break;
        case AvInputProxyState::kEnded:
        case AvInputProxyState::kStopped:
          // Input EOF is not decoded/source EOF. Reads acknowledge final drain.
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
      for (auto* decoder : {session->video.get(), session->audio.get()}) {
        if (decoder && !decoder->PushEof(result.generation)) {
          if (Stopping()) return false;
          if (const auto failure = decoder->error())
            std::rethrow_exception(failure);
          throw std::logic_error("AvMediaInput Decoder 拒绝 EOF");
        }
      }
      if (!config_.proxy.loop) {
        final_generation_ = result.generation;
        Publish(AvMediaInputState::kDraining);
      }
    } else {
      auto* decoder = result.packet->get()->stream_index ==
                              session->description->video_stream_index
                          ? session->video.get()
                          : session->audio.get();
      if (!decoder)
        throw std::logic_error("AvMediaInput Packet 缺少对应 Decoder");
      if (!decoder->PushPacket(std::move(*result.packet), result.generation)) {
        if (Stopping()) return false;
        if (const auto failure = decoder->error())
          std::rethrow_exception(failure);
        throw std::logic_error("AvMediaInput Decoder 拒绝 Packet");
      }
    }
    return true;
  }

  bool Drained(const std::shared_ptr<Session>& session) const {
    if (!final_generation_ || !session) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.mode == AvMediaInputMode::kLive) {
      return live_eof_ == final_generation_;
    }
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
  }

  void Run() noexcept {
    std::shared_ptr<Session> session;
    bool ended = false;
    try {
      // Start publishes an immediate snapshot; notify it on this worker too.
      Publish(AvMediaInputState::kConnecting, nullptr, true);
      if (!Stopping()) proxy_.Start(url_);
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
    if (failure) Publish(AvMediaInputState::kFailed);
    Cleanup(session);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failure = failure_;
    }
    if (failure)
      Publish(AvMediaInputState::kFailed);
    else
      Publish(ended ? AvMediaInputState::kEnded : AvMediaInputState::kStopped);
  }

  AvMediaInputConfig config_;
  std::optional<ffmpeg::HwDeviceContext> device_;
  ffmpeg::CodecParameters output_audio_;
  AvInputProxy proxy_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::exception_ptr failure_;
  AvMediaInputStatus status_;
  std::deque<AvInputProxyStatus> proxy_events_;
  std::shared_ptr<Session> session_;
  std::optional<std::uint64_t> video_eof_;
  std::optional<std::uint64_t> audio_eof_;
  std::optional<std::uint64_t> live_eof_;
  // Written only by the dispatcher after successfully submitting packet EOF.
  std::optional<std::uint64_t> final_generation_;
  OnStateChanged on_state_changed_;
  std::string url_;
  std::thread worker_;
};

AvMediaInput::AvMediaInput(AvMediaInputConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
AvMediaInput::AvMediaInput(const ffmpeg::HwDeviceContext& device,
                           AvMediaInputConfig config)
    : impl_(std::make_unique<Impl>(std::move(config), &device)) {}
AvMediaInput::~AvMediaInput() = default;
void AvMediaInput::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
void AvMediaInput::Start(std::string_view url) { impl_->Start(url); }
AvDecoderReadResult AvMediaInput::TryReadVideo() {
  return impl_->ReadBatch(true);
}
AvDecoderReadResult AvMediaInput::TryReadAudio() {
  return impl_->ReadBatch(false);
}
AvFrameSourceReadResult AvMediaInput::TryReadFrame() {
  return impl_->ReadLive();
}
AvMediaInputMode AvMediaInput::mode() const { return impl_->mode(); }
AvMediaInputStatus AvMediaInput::status() const { return impl_->status(); }
std::shared_ptr<const AvInputInfo> AvMediaInput::info() const {
  return impl_->info();
}
void AvMediaInput::RequestStop() noexcept { impl_->RequestStop(); }
void AvMediaInput::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
