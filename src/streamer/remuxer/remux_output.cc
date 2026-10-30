#include "streamer/remuxer/remux_output.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

#include "Http/HttpSession.h"
#include "Record/Recorder.h"
#include "Rtsp/RtspSession.h"
#include "Util/SSLBox.h"
#include "Util/mini.h"
#include "Util/util.h"
#include "fmt/format.h"
#include "mw/log.h"

namespace mw::streamer::internal {
namespace {

std::exception_ptr PushError(const std::string& target,
                             const toolkit::SockException& error) {
  return std::make_exception_ptr(std::runtime_error(
      fmt::format("ZLM output error: target={} code={} {}", target,
                  static_cast<int>(error.getErrCode()), error.what())));
}

std::atomic<std::uint64_t> next_source_id{0};
std::mutex listeners_mutex;
std::atomic<std::uint64_t> next_recording_id{0};
std::set<std::pair<std::string, std::string>> published_paths;

enum class ListenerProtocol { kRtsp, kRtsps, kHttp, kHttps };
struct Listener {
  std::weak_ptr<toolkit::TcpServer> server;
  ListenerProtocol protocol;
};
std::map<std::pair<std::string, std::uint16_t>, Listener> listeners;

void RequireServerCertificate(bool tls) {
  if (tls && !toolkit::SSL_Initor::Instance().hasServerCertificate()) {
    throw std::invalid_argument(
        "TLS 发布需要在 InitConfig.tls 中配置服务器证书");
  }
}

void ReservePath(const std::string& app, const std::string& stream) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  if (!published_paths.emplace(app, stream).second) {
    throw std::invalid_argument("RTSP 发布路径已经被占用");
  }
}

void ReleasePath(const std::pair<std::string, std::string>& path) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  published_paths.erase(path);
}

template <typename Session>
toolkit::TcpServer::Ptr Listen(const std::string& ip, std::uint16_t port,
                               const toolkit::EventPoller::Ptr& poller,
                               ListenerProtocol protocol) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  for (auto iterator = listeners.begin(); iterator != listeners.end();) {
    if (iterator->second.server.expired())
      iterator = listeners.erase(iterator);
    else
      ++iterator;
  }
  auto& entry = listeners[{ip, port}];
  if (auto server = entry.server.lock()) {
    if (entry.protocol != protocol) {
      throw std::invalid_argument("监听地址和端口已经用于不同的发布协议");
    }
    return server;
  }
  auto server = std::make_shared<toolkit::TcpServer>(poller);
  server->start<Session>(port, ip);
  entry = {server, protocol};
  return server;
}

std::string Schema(const std::string& url) {
  if (url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0)
    return RTSP_SCHEMA;
  if (url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0)
    return RTMP_SCHEMA;
  if (url.rfind("srt://", 0) == 0) return TS_SCHEMA;
  if (url.find("://") == std::string::npos) {
    const auto extension = std::filesystem::u8path(url).extension().u8string();
    if (extension == ".mp4") return {};
  }
  throw std::invalid_argument(
      "Remuxer 目标需要 RTSP(S)、RTMP(S)、SRT 或本地 MP4 路径");
}

std::string RecordingPath(const std::string& path) {
  const auto source = std::filesystem::absolute(std::filesystem::u8path(path));
  std::filesystem::path candidate;
  do {
    const auto name = source.stem().u8string() + "_" +
                      toolkit::getTimeStr("%Y_%m_%d_%H_%M_%S") + "_" +
                      std::to_string(++next_recording_id) +
                      source.extension().u8string();
    candidate = source.parent_path() / std::filesystem::u8path(name);
  } while (std::filesystem::exists(candidate));
  return candidate.generic_u8string();
}

bool ValidSegment(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
         });
}

void ValidatePublish(OutputRegistration::Kind kind, const std::string& app,
                     const std::string& stream, const std::string& ip,
                     std::uint16_t port) {
  if (kind == OutputRegistration::Kind::kRtsp) {
    if (app.empty() || stream.empty() || ip.empty() || port == 0 ||
        app.find_first_of("/\\") != std::string::npos ||
        stream.find_first_of("/\\") != std::string::npos) {
      throw std::invalid_argument(
          "RTSP 发布需要有效 app、stream、监听地址和端口");
    }
    return;
  }
  if (!ValidSegment(app) || !ValidSegment(stream) || ip.empty() || port == 0)
    throw std::invalid_argument("HLS 发布需要有效 app、stream、监听地址和端口");
}

}  // namespace

RemuxerConfig OutputRegistration::PrepareConfig(RemuxerConfig config) {
  if (config.recording_queue_bytes == 0)
    throw std::invalid_argument("Remuxer 录像队列上限必须大于零");
  if (config.stream.empty())
    config.stream = fmt::format("stream_{}", ++next_source_id);
  if (!ValidSegment(config.app) || !ValidSegment(config.stream))
    throw std::invalid_argument("Remuxer app/stream 必须是有效的 ASCII URI 段");
  return config;
}

OutputRegistration::Entry OutputRegistration::PreparePush(
    const std::string& url) {
  Entry entry;
  entry.kind = Schema(url).empty() ? Kind::kRecording : Kind::kPush;
  entry.original = url;
  entry.actual = entry.kind == Kind::kRecording ? RecordingPath(url) : url;
  return entry;
}

OutputRegistration::Entry OutputRegistration::PreparePublish(
    Kind kind, const std::string& app, const std::string& stream,
    const std::string& ip, std::uint16_t port, bool tls) {
  ValidatePublish(kind, app, stream, ip, port);
  Entry entry;
  entry.kind = kind;
  entry.app = app;
  entry.stream = stream;
  entry.ip = ip;
  entry.port = port;
  entry.tls = tls;
  const auto host = ip.find(':') == std::string::npos ? ip : "[" + ip + "]";
  const auto scheme =
      kind == Kind::kRtsp ? (tls ? "rtsps" : "rtsp") : (tls ? "https" : "http");
  entry.actual = fmt::format("{}://{}:{}/{}/{}{}", scheme, host, port, app,
                             stream, kind == Kind::kHls ? "/hls.m3u8" : "");
  return entry;
}

const OutputRegistration::Entry* OutputRegistration::FindPush(
    const std::string& url) const {
  const auto found =
      std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return (entry.kind == Kind::kPush || entry.kind == Kind::kRecording) &&
               entry.original == url;
      });
  return found == entries_.end() ? nullptr : &*found;
}

void OutputRegistration::Add(Entry entry) {
  if (entry.kind != Kind::kPush && entry.kind != Kind::kRecording &&
      std::any_of(entries_.begin(), entries_.end(), [&](const auto& existing) {
        return existing.kind == entry.kind;
      })) {
    throw std::logic_error(entry.kind == Kind::kRtsp
                               ? "Remuxer 只允许一个 RTSP 发布点"
                               : "Remuxer 只允许一个 HLS 发布点");
  }
  entries_.push_back(std::move(entry));
}

Remuxer::OutputInfo OutputRegistration::info() const {
  Remuxer::OutputInfo result;
  for (const auto& entry : entries_) {
    switch (entry.kind) {
      case Kind::kPush:
      case Kind::kRecording:
        result.output_urls.push_back(entry.actual);
        break;
      case Kind::kRtsp:
        result.rtsp_url = entry.actual;
        break;
      case Kind::kHls:
        result.hls_url = entry.actual;
        break;
    }
  }
  return result;
}

RemuxOutput::RemuxOutput(toolkit::EventPoller::Ptr poller,
                         ErrorCallback callback, RemuxerConfig config)
    : config_(std::move(config)),
      poller_(std::move(poller)),
      on_error_(std::move(callback)) {}

void RemuxOutput::Start(const std::vector<mediakit::Track::Ptr>& tracks) {
  ReservePath(config_.app, config_.stream);
  published_path_ = {config_.app, config_.stream};
  mediakit::ProtocolOption option;
  option.enable_rtsp = option.enable_rtmp = option.enable_ts = true;
  option.rtsp_demand = option.rtmp_demand = option.ts_demand = true;
  option.enable_hls = option.enable_hls_fmp4 = option.enable_mp4 = false;
  option.enable_fmp4 = false;
  option.hls_demand = false;
  option.enable_audio =
      std::any_of(tracks.begin(), tracks.end(), [](const auto& track) {
        return track->getTrackType() == mediakit::TrackAudio;
      });
  option.add_mute_audio = false;
  option.auto_close = false;
  option.paced_sender_ms = 0;
  option.modify_stamp = mediakit::ProtocolOption::kModifyStampOff;
  option.rtsp_ntp_from_source_stamp = true;
  option.max_track = tracks.size();
  try {
    auto muxer = std::make_shared<mediakit::MultiMediaSourceMuxer>(
        mediakit::MediaTuple(DEFAULT_VHOST, config_.app, config_.stream), 0.0f,
        option);
    muxer->setMediaListener(shared_from_this());
    muxer->setTrackReadyTimeoutMS(0);
    for (const auto& track : tracks) {
      if (!muxer->addTrack(track))
        throw std::invalid_argument("ZLM 拒绝编码轨道");
    }
    muxer->addTrackCompleted();
    muxer_ = std::move(muxer);
  } catch (...) {
    ReleasePath(*published_path_);
    published_path_.reset();
    throw;
  }
}

void RemuxOutput::AddPrepared(const OutputRegistration::Entry& entry) {
  if (entry.kind == OutputRegistration::Kind::kRtsp) {
    AddRtspPublish(entry.ip, entry.port, entry.tls);
    return;
  }
  if (entry.kind == OutputRegistration::Kind::kHls) {
    AddHlsPublish(entry.ip, entry.port, entry.tls);
    return;
  }
  const auto schema = Schema(entry.actual);
  targets_.push_back({entry.actual, schema, nullptr});
  StartPushers();
}

void RemuxOutput::AddRtspPublish(const std::string& ip, std::uint16_t port,
                                 bool tls) {
  ValidatePublish(OutputRegistration::Kind::kRtsp, config_.app, config_.stream,
                  ip, port);
  if (server_) throw std::logic_error("Remuxer 只允许一个 RTSP 发布点");
  RequireServerCertificate(tls);
  server_ = tls ? Listen<mediakit::RtspSessionWithSSL>(ip, port, poller_,
                                                       ListenerProtocol::kRtsps)
                : Listen<mediakit::RtspSession>(ip, port, poller_,
                                                ListenerProtocol::kRtsp);
  MW_LOG_INFO("streamer", "Remuxer RTSP publish: {}://{}:{}/{}/{}",
              tls ? "rtsps" : "rtsp", ip, port, config_.app, config_.stream);
}

std::string RemuxOutput::AddHlsPublish(const std::string& ip,
                                       std::uint16_t port, bool tls) {
  ValidatePublish(OutputRegistration::Kind::kHls, config_.app, config_.stream,
                  ip, port);
  if (hls_server_) throw std::logic_error("Remuxer 只允许一个 HLS 发布点");
  RequireServerCertificate(tls);
  auto server = tls ? Listen<mediakit::HttpsSession>(ip, port, poller_,
                                                     ListenerProtocol::kHttps)
                    : Listen<mediakit::HttpSession>(ip, port, poller_,
                                                    ListenerProtocol::kHttp);
  const auto root =
      toolkit::mINI::Instance()[mediakit::Http::kRootPath].as<std::string>();
  if (!muxer_->setupRecord(mediakit::MediaSource::NullMediaSource(),
                           mediakit::Recorder::type_hls, true, root, 0))
    throw std::runtime_error("ZLM 启用 HLS 失败");
  hls_server_ = std::move(server);
  const auto host = ip.find(':') == std::string::npos ? ip : "[" + ip + "]";
  auto url = fmt::format("{}://{}:{}/{}/{}/hls.m3u8", tls ? "https" : "http",
                         host, port, config_.app, config_.stream);
  MW_LOG_INFO("streamer", "Remuxer HLS publish: {}", url);
  return url;
}

void RemuxOutput::InputFrame(const mediakit::Frame::Ptr& frame) {
  muxer_->inputFrame(frame);
}

bool RemuxOutput::tracks_ready() const { return muxer_->isAllTrackReady(); }

std::vector<Remuxer::PushStatus> RemuxOutput::push_statuses() const {
  std::vector<Remuxer::PushStatus> result;
  for (const auto& target : targets_) {
    if (target.pusher)
      result.push_back({target.url, target.pusher->getStatus()});
  }
  return result;
}

void RemuxOutput::Flush() { muxer_->flush(); }

void RemuxOutput::Close() {
  closed_ = true;
  targets_.clear();
  registered_.clear();
  muxer_.reset();
  hls_server_.reset();
  server_.reset();
  if (published_path_) {
    ReleasePath(*published_path_);
    published_path_.reset();
  }
}

void RemuxOutput::Error(std::exception_ptr error) {
  if (!closed_ && on_error_) on_error_(error);
}

toolkit::EventPoller::Ptr RemuxOutput::getOwnerPoller(mediakit::MediaSource&) {
  return poller_;
}
bool RemuxOutput::close(mediakit::MediaSource&) { return false; }
void RemuxOutput::onRegist(mediakit::MediaSource& source, bool registered) {
  if (registered && !closed_) {
    registered_[source.getSchema()] = true;
    StartPushers();
  }
}

void RemuxOutput::StartPushers() {
  for (auto& target : targets_) {
    if (target.pusher || !registered_[target.schema]) continue;
    try {
      auto pusher = std::make_shared<mediakit::PusherProxy>(
          mediakit::MediaSource::find(target.schema, DEFAULT_VHOST, config_.app,
                                      config_.stream, false),
          -1, poller_);
      std::weak_ptr<RemuxOutput> weak = shared_from_this();
      const auto url = target.url;
      pusher->setPushCallbackOnce(
          [weak, url](const toolkit::SockException& error) {
            if (auto self = weak.lock(); self && !self->closed_ && error) {
              MW_LOG_WARNING(
                  "streamer",
                  "Remuxer 推流连接失败，将自动重连: target={} code={} {}", url,
                  static_cast<int>(error.getErrCode()), error.what());
            }
          });
      pusher->setOnClose([weak, url](const toolkit::SockException& error) {
        if (auto self = weak.lock()) {
          self->Error(PushError(url, error));
        }
      });
      pusher->publish(url);
      target.pusher = std::move(pusher);
    } catch (...) {
      Error(std::current_exception());
    }
  }
}

}  // namespace mw::streamer::internal
