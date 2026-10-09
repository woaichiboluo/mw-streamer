#include "mw/streamer/remuxer/remuxer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include "Common/MultiMediaSourceMuxer.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Pusher/PusherProxy.h"
#include "Record/RemuxRecorder.h"
#include "Rtsp/RtspSession.h"
#include "Util/util.h"
#include "mw/log.h"
#include "mw/streamer/performance/performance.h"
#include "mw/streamer/remuxer/packet_converter.h"
#include "mw/streamer/remuxer/packet_interleaver.h"

namespace mw::streamer {
namespace {

constexpr AVRational kNanoseconds{1, 1000000000};
constexpr AVRational kMilliseconds{1, 1000};
std::atomic<std::uint64_t> next_source_id{0};
std::mutex listeners_mutex;
std::map<std::pair<std::string, std::uint16_t>,
         std::weak_ptr<toolkit::TcpServer>>
    listeners;
std::set<std::pair<std::string, std::string>> published_paths;

void ReservePath(const std::string& app, const std::string& stream) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  if (!published_paths.emplace(app, stream).second) {
    throw std::invalid_argument("RTSP发布路径已经被占用");
  }
}

void ReleasePath(const std::pair<std::string, std::string>& path) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  published_paths.erase(path);
}

toolkit::TcpServer::Ptr Listen(const std::string& ip, std::uint16_t port,
                               const toolkit::EventPoller::Ptr& poller) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  for (auto iterator = listeners.begin(); iterator != listeners.end();) {
    if (iterator->second.expired())
      iterator = listeners.erase(iterator);
    else
      ++iterator;
  }
  auto& entry = listeners[{ip, port}];
  if (auto server = entry.lock()) return server;
  auto server = std::make_shared<toolkit::TcpServer>(poller);
  server->start<mediakit::RtspSession>(port, ip);
  entry = server;
  return server;
}

std::string Schema(const std::string& url) {
  if (url.rfind("rtsp://", 0) == 0) return RTSP_SCHEMA;
  if (url.rfind("rtmp://", 0) == 0) return RTMP_SCHEMA;
  if (url.rfind("srt://", 0) == 0) return TS_SCHEMA;
  if (url.find("://") == std::string::npos) {
    const auto extension = std::filesystem::u8path(url).extension().u8string();
    if (extension == ".mp4" || extension == ".m3u8") return {};
  }
  throw std::invalid_argument(
      "Remuxer目标需要RTSP、RTMP、SRT或本地MP4/M3U8路径");
}

std::string RecordingPath(const std::string& path) {
  const auto source = std::filesystem::absolute(std::filesystem::u8path(path));
  const auto name = source.stem().u8string() + "_" +
                    toolkit::getTimeStr("%Y_%m_%d_%H_%M_%S") +
                    source.extension().u8string();
  return (source.parent_path() / std::filesystem::u8path(name))
      .generic_u8string();
}

}  // namespace

class Remuxer::Impl final : public mediakit::MediaSourceEvent,
                            public std::enable_shared_from_this<Impl> {
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
      if (running_) throw std::logic_error("Remuxer已经启动");
    }
    auto poller =
        poller_ ? poller_ : toolkit::EventPollerPool::Instance().getPoller();
    Sync(poller, [&] {
      auto interleaver = std::make_unique<internal::PacketInterleaver>(streams);
      auto converter = std::make_unique<internal::PacketConverter>(streams);
      mediakit::ProtocolOption option;
      option.enable_rtsp = option.enable_rtmp = option.enable_ts = true;
      option.rtsp_demand = option.rtmp_demand = option.ts_demand = true;
      option.enable_hls = option.enable_hls_fmp4 = option.enable_mp4 = false;
      option.enable_fmp4 = false;
      option.enable_audio =
          std::any_of(streams.begin(), streams.end(), [](const auto& stream) {
            return stream.codec_parameters.get()->codec_type ==
                   AVMEDIA_TYPE_AUDIO;
          });
      option.add_mute_audio = false;
      option.auto_close = false;
      option.paced_sender_ms = 0;
      option.modify_stamp = mediakit::ProtocolOption::kModifyStampOff;
      option.preserve_startup_packets = true;
      option.rtsp_ntp_from_source_stamp = true;
      option.max_track = streams.size();
      auto muxer = std::make_shared<mediakit::MultiMediaSourceMuxer>(
          mediakit::MediaTuple(DEFAULT_VHOST, "mw_remux",
                               std::to_string(++next_source_id)),
          0.0f, option);
      if (!poller_) poller_ = poller;
      muxer->setMediaListener(shared_from_this());
      muxer->setTrackReadyTimeoutMS(0);
      for (const auto& track : converter->tracks()) {
        if (!muxer->addTrack(track))
          throw std::invalid_argument("ZLM拒绝编码轨道");
      }
      muxer->addTrackCompleted();
      muxer_ = std::move(muxer);
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
    const auto schema = Schema(url);
    std::lock_guard<std::mutex> control(control_);
    auto poller = ActivePoller();
    std::string result;
    Sync(poller, [&] {
      RequireAccepting();
      if (!schema.empty()) {
        const auto existing =
            std::find_if(targets_.begin(), targets_.end(),
                         [&](const auto& target) { return target.url == url; });
        if (existing != targets_.end()) {
          result = url;
          return;
        }
        targets_.push_back({url, schema, nullptr});
        result = url;
        StartPushers();
      } else {
        result = RecordingPath(url);
        auto recorder = mediakit::createRemuxRecorder(
            result, std::filesystem::u8path(url).extension() == ".m3u8");
        muxer_->addRecorder(recorder);
        MW_LOG_INFO("streamer", "Remuxer recording: {}", result);
      }
    });
    return result;
  }

  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port) {
    if (app.empty() || stream.empty() || ip.empty() || port == 0 ||
        app.find_first_of("/\\") != std::string::npos ||
        stream.find_first_of("/\\") != std::string::npos) {
      throw std::invalid_argument(
          "RTSP发布需要有效app、stream、监听地址和端口");
    }
    std::lock_guard<std::mutex> control(control_);
    auto poller = ActivePoller();
    Sync(poller, [&] {
      RequireAccepting();
      if (server_) throw std::logic_error("Remuxer只允许一个RTSP发布点");
      ReservePath(app, stream);
      try {
        auto server = Listen(ip, port, poller);
        auto source = muxer_->getMediaSource(RTSP_SCHEMA);
        source->setMediaTuple(mediakit::MediaTuple(DEFAULT_VHOST, app, stream));
        server_ = std::move(server);
        published_path_ = {app, stream};
      } catch (...) {
        ReleasePath({app, stream});
        throw;
      }
      MW_LOG_INFO("streamer", "Remuxer RTSP publish: rtsp://{}:{}/{}/{}", ip,
                  port, app, stream);
    });
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
  struct Target {
    std::string url;
    std::string schema;
    mediakit::PusherProxy::Ptr pusher;
  };

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
    if (!accepting_) throw std::logic_error("Remuxer未启动或已经排空");
    return poller_;
  }
  void RequireAccepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) throw std::logic_error("Remuxer未启动或已经排空");
  }
  bool Active() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !closing_;
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
    MW_LOG_ERROR("streamer", "Remuxer error: target={} code={} {}", target,
                 code, message);
    if (callback) callback(target, code, message);
  }

  toolkit::EventPoller::Ptr getOwnerPoller(mediakit::MediaSource&) override {
    return poller_;
  }
  bool close(mediakit::MediaSource&) override { return false; }
  void onRegist(mediakit::MediaSource& source, bool registered) override {
    if (registered && Active()) {
      registered_[source.getSchema()] = true;
      StartPushers();
    }
  }

  void StartPushers() {
    for (auto& target : targets_) {
      if (target.pusher || !registered_[target.schema]) continue;
      try {
        auto pusher = std::make_shared<mediakit::PusherProxy>(
            muxer_->getMediaSource(target.schema), -1, poller_);
        std::weak_ptr<Impl> weak = shared_from_this();
        const auto url = target.url;
        pusher->setPushCallbackOnce(
            [weak, url](const toolkit::SockException& error) {
              if (auto self = weak.lock(); self && error) {
                self->Error(url, static_cast<int>(error.getErrCode()),
                            error.what());
              }
            });
        pusher->setOnClose([weak, url](const toolkit::SockException& error) {
          if (auto self = weak.lock()) {
            self->Error(url, static_cast<int>(error.getErrCode()),
                        error.what());
          }
        });
        pusher->publish(url);
        target.pusher = std::move(pusher);
      } catch (const std::exception& error) {
        Error(target.url, AVERROR_EXTERNAL, error.what());
      }
    }
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
      if (finish && !muxer_->isAllTrackReady()) {
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
        muxer_->inputFrame(frame);
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
          if (eof_tracks.empty() && !muxer_->isAllTrackReady()) muxer_->flush();
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
        muxer_->flush();
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
      muxer_->closeRecorders();
    } catch (const std::exception& error) {
      Error({}, AVERROR_EXTERNAL, error.what(), true);
    }
    targets_.clear();
    registered_.clear();
    muxer_.reset();
    converter_.reset();
    server_.reset();
    if (published_path_) {
      ReleasePath(*published_path_);
      published_path_.reset();
    }
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
  mediakit::MultiMediaSourceMuxer::Ptr muxer_;
  std::unique_ptr<internal::PacketInterleaver> interleaver_;
  internal::RemuxPerformance performance_;
  std::unique_ptr<internal::PacketConverter> converter_;
  std::optional<std::int64_t> shift_ns_;
  std::vector<Target> targets_;
  std::map<std::string, bool> registered_;
  toolkit::TcpServer::Ptr server_;
  std::optional<std::pair<std::string, std::string>> published_path_;
};

Remuxer::Remuxer() : impl_(std::make_shared<Impl>()) {}
Remuxer::~Remuxer() { Stop(); }
void Remuxer::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
void Remuxer::SetOnEnded(OnEnded callback) {
  impl_->SetOnEnded(std::move(callback));
}
void Remuxer::Start(const std::vector<ffmpeg::StreamInfo>& streams) {
  impl_->Start(streams);
}
bool Remuxer::SubmitPacket(const ffmpeg::Packet& packet, std::int64_t dts_ns) {
  return impl_->SubmitPacket(packet, dts_ns);
}
std::string Remuxer::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void Remuxer::AddRtspPublish(const std::string& app, const std::string& stream,
                             const std::string& ip, std::uint16_t port) {
  impl_->AddRtspPublish(app, stream, ip, port);
}
void Remuxer::Drain() noexcept { impl_->Drain(); }
void Remuxer::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
