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
namespace {

constexpr AVRational kNanoseconds{1, 1000000000};
constexpr AVRational kMilliseconds{1, 1000};
}  // namespace

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
      if (running_) throw std::logic_error("AsyncRemuxer已经启动");
    }
    auto poller =
        poller_ ? poller_ : toolkit::EventPollerPool::Instance().getPoller();
    Sync(poller, [&] {
      auto interleaver = std::make_unique<internal::PacketInterleaver>(streams);
      auto converter = std::make_unique<internal::PacketConverter>(streams);
      std::weak_ptr<Impl> weak = shared_from_this();
      auto output = std::make_shared<internal::RemuxOutput>(
          poller,
          [weak](std::string_view target, int code, std::string_view message) {
            if (auto self = weak.lock()) self->Error(target, code, message);
          });
      output->Start(converter->tracks());
      if (!poller_) poller_ = poller;
      output_ = std::move(output);
      converter_ = std::move(converter);
      std::lock_guard<std::mutex> lock(mutex_);
      interleaver_ = std::move(interleaver);
      performance_.Start(this, streams);
      shift_ns_.reset();
      running_ = accepting_ = true;
      draining_ = closing_ = scheduled_ = false;
    });
  }

  bool SubmitPacket(const ffmpeg::Packet& packet, std::int64_t ordering_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
      performance_.Rejected();
      return false;
    }
    const auto interleave_started = performance_.Begin();
    try {
      interleaver_->Push(packet, ordering_ns, interleave_started);
    } catch (...) {
      performance_.Interleaved(interleave_started, performance_.Begin());
      performance_.Error();
      throw;
    }
    performance_.Interleaved(interleave_started, performance_.Begin());
    performance_.Accepted(*packet.get());
    UpdateDiscarded();
    Schedule();
    return true;
  }

  std::string AddPushUrl(const std::string& url) {
    std::lock_guard<std::mutex> control(control_);
    auto poller = ActivePoller();
    std::string result;
    Sync(poller, [&] {
      RequireAccepting();
      result = output_->AddPushUrl(url);
    });
    return result;
  }

  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port) {
    std::lock_guard<std::mutex> control(control_);
    auto poller = ActivePoller();
    Sync(poller, [&] {
      RequireAccepting();
      output_->AddRtspPublish(app, stream, ip, port);
    });
  }

  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port) {
    std::lock_guard<std::mutex> control(control_);
    auto poller = ActivePoller();
    std::string result;
    Sync(poller, [&] {
      RequireAccepting();
      result = output_->AddHlsPublish(app, stream, ip, port);
    });
    return result;
  }

  void Drain() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || closing_) return;
    accepting_ = false;
    draining_ = true;
    Schedule();
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> control(control_);
    Drain();
    std::unique_lock<std::mutex> lock(mutex_);
    stopped_.wait(lock, [&] { return !running_; });
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

  toolkit::EventPoller::Ptr ActivePoller() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) throw std::logic_error("AsyncRemuxer未启动或已经排空");
    return poller_;
  }
  void RequireAccepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) throw std::logic_error("AsyncRemuxer未启动或已经排空");
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

  // Called with the admission mutex held; the same queue also serves startup
  // selection and media interleaving, without a second packet handoff queue.
  void Schedule() {
    if (scheduled_) return;
    scheduled_ = true;
    auto self = shared_from_this();
    poller_->async([self] { self->Process(); }, false);
  }

  // Called under the admission mutex. EOF can discard another startup prefix.
  void UpdateDiscarded() {
    if (!performance_.enabled()) return;
    performance_.Discarded(interleaver_->discarded_packets(),
                           interleaver_->discarded_bytes());
  }

  void Process() {
    bool finish = false;
    try {
      std::vector<internal::PacketInterleaver::Item> packets;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || closing_) {
          scheduled_ = false;
          return;
        }
        const auto interleave_started = performance_.Begin();
        packets = interleaver_->PopReady(draining_);
        performance_.Interleaved(interleave_started, performance_.Begin());
        UpdateDiscarded();
        scheduled_ = false;
        finish = draining_;
      }
      if (!packets.empty() && !shift_ns_) {
        std::int64_t minimum = 0;
        for (const auto& packet : packets) {
          minimum = std::min({minimum, packet.pts_ns, packet.dts_ns});
        }
        shift_ns_ = -minimum;
      }
      std::set<int> eof_tracks;
      if (finish && !output_->tracks_ready()) {
        for (const auto& packet : packets)
          eof_tracks.insert(packet.packet->stream_index);
      }
      for (const auto& packet : packets) {
        const auto convert_started = performance_.Begin();
        const auto dts = packet.dts_ns + *shift_ns_;
        const auto pts = packet.pts_ns + *shift_ns_;
        if (dts < 0 || pts < 0)
          throw std::invalid_argument("媒体时间戳早于共同输出起点");
        const auto dts_ms = static_cast<std::uint64_t>(
            av_rescale_q(dts, kNanoseconds, kMilliseconds));
        const auto pts_ms = static_cast<std::uint64_t>(
            av_rescale_q(pts, kNanoseconds, kMilliseconds));
        auto frame = converter_->Convert(packet.packet, dts_ms, pts_ms);
        const auto mux_started = performance_.Begin();
        output_->InputFrame(frame);
        const auto finished = performance_.Begin();
        if (performance_.enabled()) {
          std::lock_guard<std::mutex> lock(mutex_);
          performance_.HandedOff(
              *packet.packet.get(), packet.pts_ns, packet.dts_ns, pts_ms,
              dts_ms, packet.queued_at, convert_started, mux_started, finished);
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
        performance_.Report(interleaver_->queued_packets(),
                            interleaver_->queued_bytes(),
                            interleaver_->initialized(), draining_);
      }
    } catch (const std::exception& error) {
      Error({}, AVERROR_EXTERNAL, error.what());
      finish = true;
    }
    if (finish) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        closing_ = true;
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
    auto self = shared_from_this();
    // Wait behind the pusher teardown tasks queued on this same poller.
    poller_->async(
        [self] {
          OnEnded callback;
          {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->UpdateDiscarded();
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
        },
        false);
  }

  std::mutex control_;
  std::mutex mutex_;
  std::condition_variable stopped_;
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
  std::optional<std::int64_t> shift_ns_;
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
bool AsyncRemuxer::SubmitPacket(const ffmpeg::Packet& packet,
                                std::int64_t dts_ns) {
  return impl_->SubmitPacket(packet, dts_ns);
}
std::string AsyncRemuxer::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void AsyncRemuxer::AddRtspPublish(const std::string& app,
                                  const std::string& stream,
                                  const std::string& ip, std::uint16_t port) {
  impl_->AddRtspPublish(app, stream, ip, port);
}
std::string AsyncRemuxer::AddHlsPublish(const std::string& app,
                                        const std::string& stream,
                                        const std::string& ip,
                                        std::uint16_t port) {
  return impl_->AddHlsPublish(app, stream, ip, port);
}
void AsyncRemuxer::Drain() noexcept { impl_->Drain(); }
void AsyncRemuxer::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
