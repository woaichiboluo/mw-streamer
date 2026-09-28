#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Common/MediaSource.h"
#include "Network/TcpServer.h"
#include "Rtsp/RtspMediaSourceMuxer.h"
#include "Rtsp/RtspMuxer.h"
#include "Rtsp/RtspSession.h"
#include "ext-codec/H264.h"

namespace {

class ClientSocket {
 public:
  explicit ClientSocket(std::uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    REQUIRE(fd_ != INVALID_SOCKET);
    DWORD timeout = 3000;
#else
    REQUIRE(fd_ >= 0);
    timeval timeout{};
    timeout.tv_sec = 3;
#endif
    REQUIRE(setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(fd_, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0);
  }

  ~ClientSocket() {
#ifdef _WIN32
    closesocket(fd_);
#else
    close(fd_);
#endif
  }

  void Send(const std::string& request) {
    REQUIRE(send(fd_, request.data(), static_cast<int>(request.size()), 0) ==
            static_cast<int>(request.size()));
  }

  std::string ReadResponse() {
    std::string response;
    char byte;
    while (response.find("\r\n\r\n") == std::string::npos) {
      REQUIRE(recv(fd_, &byte, 1, 0) == 1);
      response.push_back(byte);
    }
    const auto end = response.find("\r\n\r\n") + 4;
    const auto length_pos = response.find("Content-Length:");
    if (length_pos != std::string::npos && length_pos < end) {
      const auto length = std::stoul(response.substr(length_pos + 15));
      while (response.size() < end + length) {
        REQUIRE(recv(fd_, &byte, 1, 0) == 1);
        response.push_back(byte);
      }
    }
    return response;
  }

  bool ReadInterleavedPacket() {
    char header[4];
    for (char& byte : header) {
      if (recv(fd_, &byte, 1, 0) != 1) return false;
    }
    if (header[0] != '$' || header[1] != 0) return false;
    const auto length = (static_cast<unsigned char>(header[2]) << 8) |
                        static_cast<unsigned char>(header[3]);
    std::string packet(length, '\0');
    std::size_t received = 0;
    while (received < packet.size()) {
      const auto count = recv(fd_, packet.data() + received,
                              static_cast<int>(packet.size() - received), 0);
      if (count <= 0) return false;
      received += static_cast<std::size_t>(count);
    }
    return packet.size() >= 12 &&
           (static_cast<unsigned char>(packet[0]) >> 6) == 2;
  }

 private:
#ifdef _WIN32
  SOCKET fd_ = INVALID_SOCKET;
#else
  int fd_;
#endif
};

class MediaListener final : public mediakit::MediaSourceEvent {
 public:
  MediaListener() : poller_(toolkit::EventPollerPool::Instance().getPoller()) {}

  int totalReaderCount(mediakit::MediaSource& source) override {
    return source.readerCount();
  }

  toolkit::EventPoller::Ptr getOwnerPoller(mediakit::MediaSource&) override {
    return poller_;
  }

 private:
  const toolkit::EventPoller::Ptr poller_;
};

class RtpCapture final
    : public toolkit::RingDelegate<mediakit::RtpPacket::Ptr> {
 public:
  void onWrite(mediakit::RtpPacket::Ptr packet, bool) override {
    packets.push_back(std::move(packet));
  }

  std::vector<mediakit::RtpPacket::Ptr> packets;
};

mediakit::H264Frame::Ptr KeyFrame(std::uint64_t dts) {
  auto frame = mediakit::FrameImp::create<mediakit::H264Frame>();
  frame->_buffer.assign("\x00\x00\x00\x01\x65\x88\x84", 7);
  frame->_prefix_size = 4;
  frame->_dts = dts;
  return frame;
}

}  // namespace

TEST_CASE(
    "ZLM RTSP server serves a published H264 stream and releases its port") {
  auto server = std::make_shared<toolkit::TcpServer>();
  server->start<mediakit::RtspSession>(0, "127.0.0.1");
  const auto port = server->getPort();
  REQUIRE(port != 0);

  mediakit::ProtocolOption option;
  option.rtsp_demand = false;
  auto muxer = std::make_shared<mediakit::RtspMediaSourceMuxer>(
      mediakit::MediaTuple("__defaultVhost__", "live", "rtsp-test"), option);
  auto listener = std::make_shared<MediaListener>();
  muxer->setListener(listener);
  const std::string sps(
      "\x00\x00\x00\x01\x67\x42\xc0\x1e\xda\x02\x80\x2d\xc8\x08\x80", 15);
  const std::string pps("\x00\x00\x00\x01\x68\xce\x06\xe2", 8);
  REQUIRE(muxer->addTrack(std::make_shared<mediakit::H264Track>(sps, pps)));
  muxer->addTrackCompleted();
  REQUIRE(muxer->inputFrame(KeyFrame(0)));
  REQUIRE(muxer->inputFrame(KeyFrame(40)));
  muxer->flush();
  auto source = std::static_pointer_cast<mediakit::RtspMediaSource>(
      muxer->getMediaSource());
  REQUIRE_FALSE(source->getSdp().empty());
  REQUIRE(source->getRing());
  REQUIRE(mediakit::MediaSource::find("rtsp", "__defaultVhost__", "live",
                                      "rtsp-test", false));

  const std::string url =
      "rtsp://127.0.0.1:" + std::to_string(port) + "/live/rtsp-test";
  {
    ClientSocket client(port);
    client.Send("OPTIONS " + url + " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    CHECK(client.ReadResponse().find("RTSP/1.0 200 OK") == 0);

    client.Send("DESCRIBE " + url +
                " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
    const auto describe = client.ReadResponse();
    REQUIRE(describe.find("RTSP/1.0 200 OK") == 0);
    CHECK(describe.find("m=video") != std::string::npos);
    CHECK(describe.find("H264") != std::string::npos);

    client.Send("SETUP " + url +
                "/trackID=0 RTSP/1.0\r\nCSeq: 3\r\nTransport: "
                "RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    const auto setup = client.ReadResponse();
    REQUIRE(setup.find("RTSP/1.0 200 OK") == 0);
    const auto session_pos = setup.find("Session: ");
    REQUIRE(session_pos != std::string::npos);
    const auto session_end = setup.find("\r\n", session_pos);
    const auto session = setup.substr(session_pos, session_end - session_pos);

    client.Send("PLAY " + url + " RTSP/1.0\r\nCSeq: 4\r\n" + session +
                "\r\n\r\n");
    REQUIRE(client.ReadResponse().find("RTSP/1.0 200 OK") == 0);
    REQUIRE(muxer->inputFrame(KeyFrame(80)));
    muxer->flush();
    CHECK(client.ReadInterleavedPacket());
  }

  muxer.reset();
  server.reset();
  auto rebound = std::make_shared<toolkit::TcpServer>();
  rebound->start<mediakit::RtspSession>(port, "127.0.0.1");
  CHECK(rebound->getPort() == port);
}

TEST_CASE("RTSP发布的NTP时间跨RTP时间戳回绕保持连续") {
  auto muxer = std::make_shared<mediakit::RtspMuxer>(nullptr, true);
  auto capture = std::make_shared<RtpCapture>();
  muxer->getRtpRing()->setDelegate(capture);
  const std::string sps(
      "\x00\x00\x00\x01\x67\x42\xc0\x1e\xda\x02\x80\x2d\xc8\x08\x80", 15);
  const std::string pps("\x00\x00\x00\x01\x68\xce\x06\xe2", 8);
  REQUIRE(muxer->addTrack(std::make_shared<mediakit::H264Track>(sps, pps)));
  muxer->addTrackCompleted();

  constexpr std::uint64_t kBeforeWrapMs = 47721000;
  constexpr std::uint64_t kAfterWrapMs = kBeforeWrapMs + 1000;
  REQUIRE(muxer->inputFrame(KeyFrame(kBeforeWrapMs)));
  muxer->flush();
  REQUIRE_FALSE(capture->packets.empty());
  const auto before = capture->packets.back();
  REQUIRE(muxer->inputFrame(KeyFrame(kAfterWrapMs)));
  muxer->flush();
  const auto after = capture->packets.back();
  CHECK(after->getStamp() < before->getStamp());
  CHECK(after->ntp_stamp - before->ntp_stamp == 1000);
}
