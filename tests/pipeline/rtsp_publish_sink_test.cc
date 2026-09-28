#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/MediaSource.h"
#include "Extension/Frame.h"
#include "Record/MP4Demuxer.h"
#include "mw/streamer/converter/zlm_codec_parameters_converter.h"
#include "mw/streamer/converter/zlm_packet_converter.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/output/rtsp_publish_sink.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::Packet;
using mw::streamer::PacketSinkState;
using mw::streamer::RtspPublishSink;
using mw::streamer::RtspPublishSinkConfig;
using mw::streamer::StreamEndReason;
using mw::streamer::StreamInfo;
using mw::streamer::ZlmCodecParametersConverter;
using mw::streamer::ZlmPacketConverter;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void CloseSocket(SocketHandle socket) {
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

std::uint16_t UnusedPort() {
  const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(socket != kInvalidSocket);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(bind(socket, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == 0);
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  REQUIRE(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) ==
          0);
  const auto port = ntohs(address.sin_port);
  CloseSocket(socket);
  return port;
}

bool CanConnect(std::uint16_t port) {
  const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket == kInvalidSocket) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool connected = connect(socket, reinterpret_cast<sockaddr*>(&address),
                                 sizeof(address)) == 0;
  CloseSocket(socket);
  return connected;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

class RtspClient final {
 public:
  explicit RtspClient(std::uint16_t port) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(socket_ != kInvalidSocket);
#ifdef _WIN32
    DWORD timeout = 3000;
#else
    timeval timeout{};
    timeout.tv_sec = 3;
#endif
    REQUIRE(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(socket_, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0);
  }

  ~RtspClient() { CloseSocket(socket_); }

  void Send(const std::string& request) {
    REQUIRE(send(socket_, request.data(), static_cast<int>(request.size()),
                 0) == static_cast<int>(request.size()));
  }

  std::string ReadResponse() {
    std::string response;
    char byte;
    while (response.find("\r\n\r\n") == std::string::npos) {
      REQUIRE(recv(socket_, &byte, 1, 0) == 1);
      response.push_back(byte);
    }
    const auto end = response.find("\r\n\r\n") + 4;
    const auto length_pos = response.find("Content-Length:");
    if (length_pos != std::string::npos && length_pos < end) {
      const auto length = std::stoul(response.substr(length_pos + 15));
      while (response.size() < end + length) {
        REQUIRE(recv(socket_, &byte, 1, 0) == 1);
        response.push_back(byte);
      }
    }
    return response;
  }

  bool ReadRtp() {
    for (int attempt = 0; attempt < 8; ++attempt) {
      char header[4];
      for (char& byte : header) {
        if (recv(socket_, &byte, 1, 0) != 1) return false;
      }
      if (header[0] != '$') return false;
      const auto length = (static_cast<unsigned char>(header[2]) << 8) |
                          static_cast<unsigned char>(header[3]);
      std::string packet(length, '\0');
      std::size_t received = 0;
      while (received < packet.size()) {
        const auto count = recv(socket_, packet.data() + received,
                                static_cast<int>(packet.size() - received), 0);
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
      }
      if (header[1] == 0) {
        return packet.size() >= 12 &&
               (static_cast<unsigned char>(packet[0]) >> 6) == 2;
      }
    }
    return false;
  }

 private:
  SocketHandle socket_ = kInvalidSocket;
};

struct Sample {
  std::vector<StreamInfo> streams;
  std::vector<Packet> packets;
};

Sample ReadSample() {
  mediakit::MP4Demuxer input;
  input.openMP4(std::string(MW_RTSP_PUBLISH_SINK_TEST_DATA_DIR) +
                "/h264_video.mp4");
  Sample sample;
  std::unordered_map<int, std::unique_ptr<ZlmPacketConverter>> converters;
  for (const auto& track : input.getTracks(true)) {
    const auto index = static_cast<int>(sample.streams.size());
    ZlmCodecParametersConverter parameters(track);
    sample.streams.push_back(
        {index, parameters.codec_parameters(), parameters.time_base()});
    auto converter = std::make_unique<ZlmPacketConverter>(track, index);
    converter->SetOnPacket([&sample](const Packet& packet) {
      sample.packets.push_back(packet);
      return true;
    });
    converters.emplace(track->getIndex(), std::move(converter));
  }
  bool eof = false;
  while (!eof) {
    bool key = false;
    int error = 0;
    auto frame = input.readFrame(key, eof, &error);
    REQUIRE(error == 0);
    if (!frame) continue;
    if (key && !frame->keyFrame()) {
      frame = std::make_shared<mediakit::FrameCacheAble>(frame, true);
    }
    REQUIRE(converters.at(frame->getIndex())->InputFrame(frame));
  }
  for (const auto& [index, converter] : converters) {
    REQUIRE(converter->Flush());
  }
  REQUIRE_FALSE(sample.packets.empty());
  return sample;
}

void FeedPackets(RtspPublishSink& sink, const Sample& sample,
                 std::int64_t offset_ms = 0) {
  for (const auto& packet : sample.packets) {
    auto adjusted = packet.Clone();
    const auto& stream = sample.streams.at(packet->stream_index);
    const auto offset =
        av_rescale_q(offset_ms, AVRational{1, 1000}, stream.time_base);
    adjusted->dts += offset;
    adjusted->pts += offset;
    sink.OnPacket({1, std::move(adjusted)});
  }
}

RtspPublishSinkConfig Config(std::uint16_t port, std::string stream) {
  RtspPublishSinkConfig config;
  config.bind_ip = "127.0.0.1";
  config.port = port;
  config.app = "live";
  config.stream = std::move(stream);
  return config;
}

bool HasSource(const std::string& stream) {
  return mediakit::MediaSource::find("rtsp", "__defaultVhost__", "live", stream,
                                     false) != nullptr;
}

}  // namespace

TEST_CASE("RtspPublishSink按需监听并在最后一路结束后释放端口") {
  const auto sample = ReadSample();
  const auto port = UnusedPort();
  RtspPublishSink first("first", Config(port, "first"));
  CHECK_FALSE(CanConnect(port));

  first.OnStreamsReady({1, sample.streams});
  FeedPackets(first, sample);
  REQUIRE(WaitUntil([&] { return HasSource("first"); }));
  REQUIRE(CanConnect(port));

  RtspPublishSink second("second", Config(port, "second"));
  second.OnStreamsReady({1, sample.streams});
  FeedPackets(second, sample);
  REQUIRE(WaitUntil([&] { return HasSource("second"); }));
  CHECK(second.state() != PacketSinkState::kFailed);

  RtspPublishSink duplicate("duplicate", Config(port, "first"));
  duplicate.OnStreamsReady({1, sample.streams});
  REQUIRE(
      WaitUntil([&] { return duplicate.state() == PacketSinkState::kFailed; }));
  CHECK_FALSE(duplicate.error().empty());
  duplicate.Stop();
  CHECK(CanConnect(port));

  first.OnInputEnded({1, StreamEndReason::kEof});
  REQUIRE(WaitUntil([&] { return first.state() == PacketSinkState::kEnded; }));
  CHECK(CanConnect(port));
  second.Stop();
  REQUIRE(WaitUntil([&] { return !CanConnect(port); }));
  CHECK_FALSE(HasSource("first"));
  CHECK_FALSE(HasSource("second"));
}

TEST_CASE("RtspPublishSink发布的编码视频可被RTSP客户端拉取") {
  const auto sample = ReadSample();
  const auto port = UnusedPort();
  RtspPublishSink sink("publish", Config(port, "video"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample);
  REQUIRE(WaitUntil([&] { return HasSource("video"); }));
  const auto url = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/video";

  RtspClient client(port);
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
  FeedPackets(sink, sample, 3000);
  CHECK(client.ReadRtp());
  INFO(sink.error());
  CHECK(sink.state() != PacketSinkState::kFailed);
  sink.Stop();
  REQUIRE(WaitUntil([&] { return !CanConnect(port); }));
}
