#ifndef MW_STREAMER_REMUXER_REMUX_OUTPUT_H_
#define MW_STREAMER_REMUXER_REMUX_OUTPUT_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/MultiMediaSourceMuxer.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Pusher/PusherProxy.h"

namespace mw::streamer::internal {

// One shared output session. Own through shared_ptr and serialize every call on
// poller; Flush before Close, allowing queued Ring dispatches to run in
// between. Errors from asynchronous pushers run on that same poller.
class RemuxOutput final : public mediakit::MediaSourceEvent,
                          public std::enable_shared_from_this<RemuxOutput> {
 public:
  using ErrorCallback = std::function<void(std::string_view target, int code,
                                           std::string_view message)>;

  RemuxOutput(toolkit::EventPoller::Ptr poller, ErrorCallback callback);
  void Start(const std::vector<mediakit::Track::Ptr>& tracks);
  // zero_origin applies only before the session has received any frames;
  // later recordings retain the SDK's common origin for cached GOP history.
  std::string AddPushUrl(const std::string& url, bool zero_origin = false);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port);
  void InputFrame(const mediakit::Frame::Ptr& frame);
  bool tracks_ready() const;
  void Flush();
  // Releases every output even if recording finalization throws. Call once.
  void Close();

 private:
  struct Target {
    std::string url;
    std::string schema;
    mediakit::PusherProxy::Ptr pusher;
  };

  toolkit::EventPoller::Ptr getOwnerPoller(mediakit::MediaSource&) override;
  bool close(mediakit::MediaSource&) override;
  void onRegist(mediakit::MediaSource& source, bool registered) override;
  void StartPushers();
  void Error(std::string_view target, int code, std::string_view message);

  toolkit::EventPoller::Ptr poller_;
  ErrorCallback on_error_;
  mediakit::MultiMediaSourceMuxer::Ptr muxer_;
  bool received_frame_ = false;
  bool closed_ = false;
  std::vector<Target> targets_;
  std::map<std::string, bool> registered_;
  mediakit::MultiMediaSourceMuxer::Ptr hls_muxer_;
  toolkit::TcpServer::Ptr hls_server_;
  std::optional<std::pair<std::string, std::string>> hls_published_path_;
  toolkit::TcpServer::Ptr server_;
  std::optional<std::pair<std::string, std::string>> published_path_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_REMUXER_REMUX_OUTPUT_H_
