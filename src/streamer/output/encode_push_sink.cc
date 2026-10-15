#include "mw/streamer/output/encode_push_sink.h"

#include <mutex>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/encoder/av_encoder.h"

namespace mw::streamer {

class EncodePushSink::Impl final {
 public:
  explicit Impl(const ffmpeg::HwDeviceContext& device)
      : remuxer_(std::make_unique<Remuxer>()),
        encoder_(std::make_unique<AvEncoder>(device)) {
    encoder_->SetOnPacket([this](const ffmpeg::Packet& packet) {
      // A terminal Remuxer failure already reaches Fail. Packets from accepted
      // Encoder work may be rejected while that session finishes closing.
      static_cast<void>(remuxer_->SubmitPacket(packet));
    });
    encoder_->SetOnError([this](std::exception_ptr error) { Fail(error); });
    remuxer_->SetOnError([this](std::exception_ptr error) { Fail(error); });
  }

  ~Impl() {
    encoder_->Stop();
    remuxer_->Stop();
  }

  void SetOnError(OnError callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(callback);
  }

  bool Initialize(const AvEncoderConfig& format) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (initialized_) {
        MW_LOG_ERROR("streamer", "EncodePushSink 已经初始化");
        return false;
      }
    }
    if (!encoder_->Start(format)) return false;
    try {
      if (remuxer_->Start(encoder_->streams())) {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = true;
        return true;
      }
    } catch (const std::exception& error) {
      MW_LOG_ERROR("streamer", "EncodePushSink 初始化失败: {}", error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "EncodePushSink 初始化失败: 未知异常");
    }
    // Remuxer::Start has rolled back its resources and retained registrations.
    // Stop would erase those registrations, so only join the started Encoder.
    encoder_->Stop();
    return false;
  }

  std::future<bool> Send(OutputFrame frame) {
    std::promise<bool> admitted;
    auto result = admitted.get_future();
    if (!Accepting()) {
      admitted.set_value(false);
      return result;
    }
    try {
      admitted.set_value(encoder_->SubmitFrame(frame.frame));
    } catch (...) {
      admitted.set_exception(std::current_exception());
    }
    return result;
  }

  Remuxer& remuxer() { return *remuxer_; }

 private:
  bool Accepting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_ && !error_;
  }

  void Fail(std::exception_ptr error) {
    OnError callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!initialized_ || error_) return;
      error_ = error;
      callback = std::move(on_error_);
    }
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer", "EncodePushSink 终止错误: {}", failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "EncodePushSink 发生未知终止错误");
    }
    if (callback) {
      try {
        callback(error);
      } catch (const std::exception& observer_error) {
        MW_LOG_ERROR("streamer", "EncodePushSink OnError 回调异常: {}",
                     observer_error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "EncodePushSink OnError 回调发生未知异常");
      }
    }
  }

  mutable std::mutex mutex_;
  bool initialized_ = false;
  std::exception_ptr error_;
  OnError on_error_;
  std::unique_ptr<Remuxer> remuxer_;
  std::unique_ptr<AvEncoder> encoder_;
};

EncodePushSink::EncodePushSink(const ffmpeg::HwDeviceContext& device)
    : impl_(std::make_unique<Impl>(device)) {}
EncodePushSink::~EncodePushSink() = default;
void EncodePushSink::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
bool EncodePushSink::Initialize(const AvEncoderConfig& format) noexcept {
  return impl_->Initialize(format);
}
std::future<bool> EncodePushSink::Send(OutputFrame frame) {
  return impl_->Send(std::move(frame));
}
std::string EncodePushSink::AddPushUrl(const std::string& url) {
  return impl_->remuxer().AddPushUrl(url);
}
void EncodePushSink::AddRtspPublish(const std::string& app,
                                    const std::string& stream,
                                    const std::string& ip, std::uint16_t port) {
  impl_->remuxer().AddRtspPublish(app, stream, ip, port);
}
void EncodePushSink::AddRtspPublish(const std::string& app,
                                    const std::string& stream,
                                    const std::string& ip, std::uint16_t port,
                                    bool tls) {
  impl_->remuxer().AddRtspPublish(app, stream, ip, port, tls);
}
std::string EncodePushSink::AddHlsPublish(const std::string& app,
                                          const std::string& stream,
                                          const std::string& ip,
                                          std::uint16_t port) {
  return impl_->remuxer().AddHlsPublish(app, stream, ip, port);
}
std::string EncodePushSink::AddHlsPublish(const std::string& app,
                                          const std::string& stream,
                                          const std::string& ip,
                                          std::uint16_t port, bool tls) {
  return impl_->remuxer().AddHlsPublish(app, stream, ip, port, tls);
}
Remuxer::OutputInfo EncodePushSink::output_info() const {
  return impl_->remuxer().output_info();
}

std::vector<Remuxer::PushStatus> EncodePushSink::push_statuses() const {
  return impl_->remuxer().push_statuses();
}

}  // namespace mw::streamer
