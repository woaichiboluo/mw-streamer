#include "mw/streamer/remuxer/remux_output.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

#include "Http/HttpSession.h"
#include "Record/RemuxRecorder.h"
#include "Rtsp/RtspSession.h"
#include "Util/mini.h"
#include "Util/util.h"
#include "fmt/format.h"
#include "mw/log.h"

namespace mw::streamer::internal {
namespace {

std::atomic<std::uint64_t> next_source_id{0};
std::mutex listeners_mutex;
std::set<std::pair<std::string, std::string>> published_paths;
std::set<std::pair<std::string, std::string>> hls_published_paths;

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

template <typename Session>
toolkit::TcpServer::Ptr Listen(const std::string& ip, std::uint16_t port,
                               const toolkit::EventPoller::Ptr& poller) {
  std::lock_guard<std::mutex> lock(listeners_mutex);
  static std::map<std::pair<std::string, std::uint16_t>,
                  std::weak_ptr<toolkit::TcpServer>>
      listeners;
  for (auto iterator = listeners.begin(); iterator != listeners.end();) {
    if (iterator->second.expired())
      iterator = listeners.erase(iterator);
    else
      ++iterator;
  }
  auto& entry = listeners[{ip, port}];
  if (auto server = entry.lock()) return server;
  auto server = std::make_shared<toolkit::TcpServer>(poller);
  server->start<Session>(port, ip);
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

RemuxOutput::RemuxOutput(toolkit::EventPoller::Ptr poller,
                         ErrorCallback callback)
    : poller_(std::move(poller)), on_error_(std::move(callback)) {}

void RemuxOutput::Start(const std::vector<mediakit::Track::Ptr>& tracks) {
  mediakit::ProtocolOption option;
  option.enable_rtsp = option.enable_rtmp = option.enable_ts = true;
  option.rtsp_demand = option.rtmp_demand = option.ts_demand = true;
  option.enable_hls = option.enable_hls_fmp4 = option.enable_mp4 = false;
  option.enable_fmp4 = false;
  option.enable_audio =
      std::any_of(tracks.begin(), tracks.end(), [](const auto& track) {
        return track->getTrackType() == mediakit::TrackAudio;
      });
  option.add_mute_audio = false;
  option.auto_close = false;
  option.paced_sender_ms = 0;
  option.modify_stamp = mediakit::ProtocolOption::kModifyStampOff;
  option.preserve_startup_packets = true;
  option.rtsp_ntp_from_source_stamp = true;
  option.max_track = tracks.size();
  auto muxer = std::make_shared<mediakit::MultiMediaSourceMuxer>(
      mediakit::MediaTuple(DEFAULT_VHOST, "mw_remux",
                           std::to_string(++next_source_id)),
      0.0f, option);
  muxer->setMediaListener(shared_from_this());
  muxer->setTrackReadyTimeoutMS(0);
  for (const auto& track : tracks) {
    if (!muxer->addTrack(track)) throw std::invalid_argument("ZLM拒绝编码轨道");
  }
  muxer->addTrackCompleted();
  muxer_ = std::move(muxer);
}

std::string RemuxOutput::AddPushUrl(const std::string& url, bool zero_origin) {
  const auto schema = Schema(url);
  if (!schema.empty()) {
    const auto existing =
        std::find_if(targets_.begin(), targets_.end(),
                     [&](const auto& target) { return target.url == url; });
    if (existing != targets_.end()) return url;
    targets_.push_back({url, schema, nullptr});
    StartPushers();
    return url;
  }
  const auto result = RecordingPath(url);
  auto recorder = mediakit::createRemuxRecorder(
      result, std::filesystem::u8path(url).extension() == ".m3u8");
  if (zero_origin && !received_frame_) recorder->setTimestampOrigin(0);
  muxer_->addRecorder(recorder);
  MW_LOG_INFO("streamer", "Remuxer recording: {}", result);
  return result;
}

void RemuxOutput::AddRtspPublish(const std::string& app,
                                 const std::string& stream,
                                 const std::string& ip, std::uint16_t port) {
  if (app.empty() || stream.empty() || ip.empty() || port == 0 ||
      app.find_first_of("/\\") != std::string::npos ||
      stream.find_first_of("/\\") != std::string::npos) {
    throw std::invalid_argument("RTSP发布需要有效app、stream、监听地址和端口");
  }
  if (server_) throw std::logic_error("Remuxer只允许一个RTSP发布点");
  ReservePath(app, stream);
  try {
    auto server = Listen<mediakit::RtspSession>(ip, port, poller_);
    auto source = muxer_->getMediaSource(RTSP_SCHEMA);
    source->setMediaTuple(mediakit::MediaTuple(DEFAULT_VHOST, app, stream));
    server_ = std::move(server);
    published_path_ = {app, stream};
  } catch (...) {
    ReleasePath({app, stream});
    throw;
  }
  MW_LOG_INFO("streamer", "Remuxer RTSP publish: rtsp://{}:{}/{}/{}", ip, port,
              app, stream);
}

std::string RemuxOutput::AddHlsPublish(const std::string& app,
                                       const std::string& stream,
                                       const std::string& ip,
                                       std::uint16_t port) {
  const auto valid_segment = [](const std::string& value) {
    return !value.empty() && value != "." && value != ".." &&
           std::all_of(value.begin(), value.end(), [](char c) {
             return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
           });
  };
  if (!valid_segment(app) || !valid_segment(stream) || ip.empty() || port == 0)
    throw std::invalid_argument("HLS发布需要有效app、stream、监听地址和端口");
  if (hls_server_) throw std::logic_error("Remuxer只允许一个HLS发布点");
  {
    std::lock_guard<std::mutex> lock(listeners_mutex);
    if (!hls_published_paths.emplace(app, stream).second)
      throw std::invalid_argument("HLS发布路径已经被占用");
  }
  try {
    auto server = Listen<mediakit::HttpSession>(ip, port, poller_);
    auto option = muxer_->getOption();
    option.enable_rtsp = option.enable_rtmp = option.enable_ts = false;
    option.enable_hls = true;
    option.hls_demand = false;
    option.hls_save_path =
        toolkit::mINI::Instance()[mediakit::Http::kRootPath].as<std::string>();
    // A separate native muxer keeps HLS's public tuple and recorder root
    // independent of RTSP. ZLM owns registration, slices and HTTP playback.
    auto hls_muxer = std::make_shared<mediakit::MultiMediaSourceMuxer>(
        mediakit::MediaTuple(DEFAULT_VHOST, app, stream), 0.0f, option);
    hls_muxer->setMediaListener(shared_from_this());
    hls_muxer->setTrackReadyTimeoutMS(0);
    muxer_->addRecorder(hls_muxer);
    hls_muxer_ = std::move(hls_muxer);
    hls_server_ = std::move(server);
    hls_published_path_ = {app, stream};
  } catch (...) {
    std::lock_guard<std::mutex> lock(listeners_mutex);
    hls_published_paths.erase({app, stream});
    throw;
  }
  const auto host = ip.find(':') == std::string::npos ? ip : "[" + ip + "]";
  auto url =
      fmt::format("http://{}:{}/{}/{}/hls.m3u8", host, port, app, stream);
  MW_LOG_INFO("streamer", "Remuxer HLS publish: {}", url);
  return url;
}

void RemuxOutput::InputFrame(const mediakit::Frame::Ptr& frame) {
  received_frame_ = true;
  muxer_->inputFrame(frame);
}

bool RemuxOutput::tracks_ready() const { return muxer_->isAllTrackReady(); }

void RemuxOutput::Flush() { muxer_->flush(); }

void RemuxOutput::Close() {
  closed_ = true;
  std::exception_ptr error;
  try {
    muxer_->closeRecorders();
  } catch (...) {
    error = std::current_exception();
  }
  targets_.clear();
  registered_.clear();
  muxer_.reset();
  hls_muxer_.reset();
  hls_server_.reset();
  if (hls_published_path_) {
    std::lock_guard<std::mutex> lock(listeners_mutex);
    hls_published_paths.erase(*hls_published_path_);
    hls_published_path_.reset();
  }
  server_.reset();
  if (published_path_) {
    ReleasePath(*published_path_);
    published_path_.reset();
  }
  if (error) std::rethrow_exception(error);
}

void RemuxOutput::Error(std::string_view target, int code,
                        std::string_view message) {
  if (!closed_ && on_error_) on_error_(target, code, message);
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
          muxer_->getMediaSource(target.schema), -1, poller_);
      std::weak_ptr<RemuxOutput> weak = shared_from_this();
      const auto url = target.url;
      pusher->setPushCallbackOnce([weak,
                                   url](const toolkit::SockException& error) {
        if (auto self = weak.lock(); self && error) {
          self->Error(url, static_cast<int>(error.getErrCode()), error.what());
        }
      });
      pusher->setOnClose([weak, url](const toolkit::SockException& error) {
        if (auto self = weak.lock()) {
          self->Error(url, static_cast<int>(error.getErrCode()), error.what());
        }
      });
      pusher->publish(url);
      target.pusher = std::move(pusher);
    } catch (const std::exception& error) {
      Error(target.url, AVERROR_EXTERNAL, error.what());
    }
  }
}

}  // namespace mw::streamer::internal
