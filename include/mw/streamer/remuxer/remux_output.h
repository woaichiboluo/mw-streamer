#ifndef MW_STREAMER_REMUXER_REMUX_OUTPUT_H_
#define MW_STREAMER_REMUXER_REMUX_OUTPUT_H_

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/MultiMediaSourceMuxer.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Pusher/PusherProxy.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace mw::streamer::internal {

// Configuration for one session; all mutations are serialized by the
// owning Remuxer's control mutex. Preparation needs no initialized runtime.
class OutputRegistration final {
 public:
  enum class Kind { kPush, kRtsp, kHls };
  struct Entry {
    Kind kind;
    std::string original;
    std::string actual;
    std::string app;
    std::string stream;
    std::string ip;
    std::uint16_t port = 0;
    bool tls = false;
  };

  static Entry PreparePush(const std::string& url);
  static Entry PreparePublish(Kind kind, const std::string& app,
                              const std::string& stream, const std::string& ip,
                              std::uint16_t port, bool tls);
  const Entry* FindPush(const std::string& url) const;
  void Add(Entry entry);
  Remuxer::OutputInfo info() const;
  const std::vector<Entry>& entries() const { return entries_; }

 private:
  std::vector<Entry> entries_;
};

// One shared output session. Own through shared_ptr and serialize every call on
// poller; Flush before Close, allowing queued Ring dispatches to run in
// between. Terminal pusher errors run on that same poller; recoverable network
// failures are logged while the SDK retries them internally.
class RemuxOutput final : public mediakit::MediaSourceEvent,
                          public std::enable_shared_from_this<RemuxOutput> {
 public:
  // Only unrecoverable errors are forwarded. Original exceptions are retained;
  // terminal SDK error results are wrapped at entry.
  using ErrorCallback = Remuxer::OnError;

  RemuxOutput(toolkit::EventPoller::Ptr poller, ErrorCallback callback);
  void Start(const std::vector<mediakit::Track::Ptr>& tracks);
  // zero_origin applies only before the session has received any frames;
  // later recordings retain the SDK's common origin for cached GOP history.
  // Activates the prepared exact path/URL without adding a recording suffix.
  void AddPrepared(const OutputRegistration::Entry& entry,
                   bool zero_origin = false);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls);
  void InputFrame(const mediakit::Frame::Ptr& frame);
  bool tracks_ready() const;
  std::vector<Remuxer::PushStatus> push_statuses() const;
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
  void Error(std::exception_ptr error);

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
