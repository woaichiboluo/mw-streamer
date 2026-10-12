#include "mw/streamer/remuxer/async_remuxer.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include "Poller/EventPoller.h"
#include "mw/log.h"
#include "mw/streamer/performance/performance.h"
#include "mw/streamer/remuxer/packet_converter.h"
#include "mw/streamer/remuxer/packet_interleaver.h"
#include "mw/streamer/remuxer/remux_output.h"

namespace mw::streamer {
class AsyncRemuxer::Impl final : public std::enable_shared_from_this<Impl> {
 public:
  void SetOnError(OnError callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(callback);
  }
  void SetOnEnded(OnEnded callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_ended_ = std::move(callback);
  }

  void Start(const std::vector<ffmpeg::StreamInfo>& streams) {
    std::lock_guard<std::mutex> control(control_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_) throw std::logic_error("AsyncRemuxer 已经启动");
    }
    auto poller =
        poller_ ? poller_ : toolkit::EventPollerPool::Instance().getPoller();
    auto info = registration_.info();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      output_info_ = std::move(info);
    }
    try {
      Sync(poller, [&] {
        auto interleaver =
            std::make_unique<internal::PacketInterleaver>(streams);
        auto converter = std::make_unique<internal::PacketConverter>(streams);
        auto timestamps =
            std::make_unique<internal::MediaTimestampConverter>(streams);
        std::weak_ptr<Impl> weak = shared_from_this();
        auto output = std::make_shared<internal::RemuxOutput>(
            poller, [weak](std::string_view target, int code,
                           std::string_view message) {
              if (auto self = weak.lock()) self->Error(target, code, message);
            });
        output->Start(converter->tracks());
        try {
          for (const auto& entry : registration_.entries())
            output->AddPrepared(entry);
        } catch (...) {
          const auto error = std::current_exception();
          try {
            output->Close();
          } catch (const std::exception& failure) {
            MW_LOG_ERROR("streamer", "AsyncRemuxer 启动回滚失败: {}",
                         failure.what());
          }
          std::rethrow_exception(error);
        }
        if (!poller_) poller_ = poller;
        output_ = std::move(output);
        converter_ = std::move(converter);
        std::lock_guard<std::mutex> lock(mutex_);
        interleaver_ = std::move(interleaver);
        performance_.Start(this, streams);
        timestamps_ = std::move(timestamps);
        handoff_.reset();
        running_ = accepting_ = true;
        draining_ = closing_ = scheduled_ = false;
      });
    } catch (...) {
      const auto error = std::current_exception();
      Sync(poller, [] {});
      std::rethrow_exception(error);
    }
  }

  bool SubmitPacket(const ffmpeg::Packet& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!accepting_) {
      performance_.Rejected();
      return false;
    }
    if (poller_->isCurrentThread())
      throw std::logic_error("AsyncRemuxer 不允许在输出 poller 上提交编码包");
    try {
      interleaver_->Validate(packet);
    } catch (...) {
      performance_.Error();
      throw;
    }
    writable_.wait(lock, [&] { return !accepting_ || !handoff_; });
    if (!accepting_) {
      performance_.Rejected();
      return false;
    }
    handoff_ =
        internal::PacketInterleaver::Item{packet.Ref(), performance_.Begin()};
    performance_.Accepted(*packet.get());
    Schedule();
    return true;
  }

  std::string AddPushUrl(const std::string& url) {
    std::lock_guard<std::mutex> control(control_);
    RequireRegisterable();
    if (const auto* existing = registration_.FindPush(url))
      return existing->actual;
    return Register(internal::OutputRegistration::PreparePush(url));
  }

  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls) {
    std::lock_guard<std::mutex> control(control_);
    Register(internal::OutputRegistration::PreparePublish(
        internal::OutputRegistration::Kind::kRtsp, app, stream, ip, port, tls));
  }

  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls) {
    std::lock_guard<std::mutex> control(control_);
    return Register(internal::OutputRegistration::PreparePublish(
        internal::OutputRegistration::Kind::kHls, app, stream, ip, port, tls));
  }

  RemuxerOutputInfo output_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_info_;
  }

  void Drain() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || closing_) return;
    accepting_ = false;
    draining_ = true;
    writable_.notify_all();
    Schedule();
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> control(control_);
    Drain();
    std::unique_lock<std::mutex> lock(mutex_);
    stopped_.wait(lock, [&] { return !running_; });
    registration_ = {};
    draining_ = closing_ = false;
  }

 private:
  template <typename Action>
  static void Sync(const toolkit::EventPoller::Ptr& poller, Action&& action) {
    std::exception_ptr error;
    poller->sync([&] {
      try {
        action();
      } catch (...) {
        error = std::current_exception();
      }
    });
    if (error) std::rethrow_exception(error);
  }

  bool RequireRegisterable() {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((running_ && !accepting_) || draining_ || closing_)
      throw std::logic_error("AsyncRemuxer 正在排空");
    return running_;
  }
  std::string Register(internal::OutputRegistration::Entry entry) {
    const bool active = RequireRegisterable();
    auto registration = registration_;
    registration.Add(entry);
    auto info = registration.info();
    if (active) {
      Sync(poller_, [&] {
        RequireAccepting();
        output_->AddPrepared(entry);
      });
    }
    registration_ = std::move(registration);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      output_info_ = std::move(info);
    }
    return entry.actual;
  }
  void RequireAccepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) throw std::logic_error("AsyncRemuxer 未启动或已经排空");
  }
  void Error(std::string_view target, int code, std::string_view message,
             bool closing_error = false) {
    OnError callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || (closing_ && !closing_error)) return;
      performance_.Error();
      callback = on_error_;
    }
    MW_LOG_ERROR("streamer", "AsyncRemuxer error: target={} code={} {}", target,
                 code, message);
    if (callback) callback(target, code, message);
  }

  // Admission has one packet slot; media interleaving is separate so waiting
  // for the other track can never block that track's next encoded packet.
  void Schedule() {
    if (scheduled_ || closing_) return;
    scheduled_ = true;
    auto self = shared_from_this();
    poller_->async([self] { self->Process(); }, false);
  }

  void Process() {
    bool finish = false;
    try {
      std::vector<internal::PacketInterleaver::Item> packets;
      std::vector<ffmpeg::Packet> first_packets;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || closing_) {
          scheduled_ = false;
          return;
        }
        const auto interleave_started = performance_.Begin();
        if (handoff_) {
          interleaver_->Push(handoff_->packet, handoff_->queued_at);
          handoff_.reset();
          writable_.notify_all();
        }
        if (!timestamps_->initialized())
          first_packets = interleaver_->FirstPackets();
        packets = interleaver_->PopReady(draining_);
        performance_.Interleaved(interleave_started, performance_.Begin());
        scheduled_ = false;
        finish = draining_;
      }
      if (!packets.empty() && !timestamps_->initialized())
        timestamps_->Initialize(first_packets);
      std::set<int> eof_tracks;
      if (finish && !output_->tracks_ready()) {
        for (const auto& packet : packets)
          eof_tracks.insert(packet.packet->stream_index);
      }
      for (const auto& packet : packets) {
        const auto convert_started = performance_.Begin();
        const auto timestamp = timestamps_->Convert(packet.packet);
        const auto dts_ms = timestamp.dts_ms;
        const auto pts_ms = timestamp.pts_ms;
        auto frame = converter_->Convert(packet.packet, dts_ms, pts_ms);
        const auto mux_started = performance_.Begin();
        output_->InputFrame(frame);
        const auto finished = performance_.Begin();
        if (performance_.enabled()) {
          std::lock_guard<std::mutex> lock(mutex_);
          performance_.HandedOff(*packet.packet.get(), pts_ms, dts_ms,
                                 packet.queued_at, convert_started, mux_started,
                                 finished);
        }
        if (!eof_tracks.empty()) {
          eof_tracks.erase(packet.packet->stream_index);
          // EOF identifies which declared tracks actually have packets.
          // Finalize them after their first frames, before ZLM's unready cache
          // fills.
          if (eof_tracks.empty() && !output_->tracks_ready()) output_->Flush();
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        performance_.Report(interleaver_->queued_packets() + (handoff_ ? 1 : 0),
                            interleaver_->queued_bytes() +
                                (handoff_ ? static_cast<std::size_t>(std::max(
                                                handoff_->packet->size, 0))
                                          : 0),
                            interleaver_->initialized(), draining_);
      }
    } catch (const std::exception& error) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        writable_.notify_all();
      }
      Error({}, AVERROR_EXTERNAL, error.what());
      finish = true;
    }
    if (finish) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        closing_ = true;
        writable_.notify_all();
      }
      try {
        output_->Flush();
      } catch (const std::exception& error) {
        Error({}, AVERROR_EXTERNAL, error.what(), true);
      }
      auto self = shared_from_this();
      // Ring dispatches produced by flush run before targets are released.
      poller_->async([self] { self->Finish(); }, false);
    }
  }

  void Finish() {
    try {
      output_->Close();
    } catch (const std::exception& error) {
      Error({}, AVERROR_EXTERNAL, error.what(), true);
    }
    output_.reset();
    converter_.reset();
    timestamps_.reset();
    auto self = shared_from_this();
    // Wait behind the pusher teardown tasks queued on this same poller.
    poller_->async(
        [self] {
          OnEnded callback;
          {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->handoff_.reset();
            self->interleaver_.reset();
            self->performance_.Report(0, 0, true, self->draining_, true);
            callback = self->on_ended_;
          }
          if (callback) callback();
          {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->running_ = self->scheduled_ = false;
          }
          self->stopped_.notify_all();
          self->writable_.notify_all();
        },
        false);
  }

  std::mutex control_;
  mutable std::mutex mutex_;
  internal::OutputRegistration registration_;
  RemuxerOutputInfo output_info_;
  std::condition_variable stopped_;
  std::condition_variable writable_;
  std::optional<internal::PacketInterleaver::Item> handoff_;
  bool running_ = false;
  bool accepting_ = false;
  bool draining_ = false;
  bool closing_ = false;
  bool scheduled_ = false;
  OnError on_error_;
  OnEnded on_ended_;
  toolkit::EventPoller::Ptr poller_;
  std::shared_ptr<internal::RemuxOutput> output_;
  std::unique_ptr<internal::PacketInterleaver> interleaver_;
  internal::RemuxPerformance performance_;
  std::unique_ptr<internal::PacketConverter> converter_;
  std::unique_ptr<internal::MediaTimestampConverter> timestamps_;
};

AsyncRemuxer::AsyncRemuxer() : impl_(std::make_shared<Impl>()) {}
AsyncRemuxer::~AsyncRemuxer() { Stop(); }
void AsyncRemuxer::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
void AsyncRemuxer::SetOnEnded(OnEnded callback) {
  impl_->SetOnEnded(std::move(callback));
}
void AsyncRemuxer::Start(const std::vector<ffmpeg::StreamInfo>& streams) {
  impl_->Start(streams);
}
bool AsyncRemuxer::SubmitPacket(const ffmpeg::Packet& packet) {
  return impl_->SubmitPacket(packet);
}
std::string AsyncRemuxer::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void AsyncRemuxer::AddRtspPublish(const std::string& app,
                                  const std::string& stream,
                                  const std::string& ip, std::uint16_t port) {
  AddRtspPublish(app, stream, ip, port, false);
}
void AsyncRemuxer::AddRtspPublish(const std::string& app,
                                  const std::string& stream,
                                  const std::string& ip, std::uint16_t port,
                                  bool tls) {
  impl_->AddRtspPublish(app, stream, ip, port, tls);
}
std::string AsyncRemuxer::AddHlsPublish(const std::string& app,
                                        const std::string& stream,
                                        const std::string& ip,
                                        std::uint16_t port) {
  return AddHlsPublish(app, stream, ip, port, false);
}
std::string AsyncRemuxer::AddHlsPublish(const std::string& app,
                                        const std::string& stream,
                                        const std::string& ip,
                                        std::uint16_t port, bool tls) {
  return impl_->AddHlsPublish(app, stream, ip, port, tls);
}
void AsyncRemuxer::Drain() noexcept { impl_->Drain(); }
void AsyncRemuxer::Stop() noexcept { impl_->Stop(); }
RemuxerOutputInfo AsyncRemuxer::output_info() const {
  return impl_->output_info();
}

}  // namespace mw::streamer
