#include "mw/streamer/input/av_probe.h"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <fmt/format.h>

#include "mw/streamer/ffmpeg/error.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvProbe;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string SamplePath(std::string_view name = "h264_aac.mp4") {
  return fmt::format("{}/{}", MW_AV_INPUT_TEST_DATA_DIR, name);
}

struct FormatDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, FormatDeleter> OpenNative(
    const std::string& path) {
  AVFormatContext* context = nullptr;
  const int result =
      avformat_open_input(&context, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE(result == 0);
  REQUIRE(avformat_find_stream_info(context, nullptr) >= 0);
  return owner;
}

std::map<std::string, std::string> Metadata(const AVDictionary* dictionary) {
  std::map<std::string, std::string> result;
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(dictionary, "", entry, AV_DICT_IGNORE_SUFFIX))) {
    result.emplace(entry->key, entry->value);
  }
  return result;
}

int ExceptionCode(const std::exception_ptr& exception) {
  REQUIRE(exception != nullptr);
  try {
    std::rethrow_exception(exception);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK_FALSE(std::string(error.what()).empty());
    return error.error_code();
  }
  FAIL("Expected original FFmpeg exception");
  return 0;
}

TEST_CASE("AvProbe preserves native media information after closing input",
          "[av-probe][info]") {
  std::string name = "h264_aac.mp4";
  SECTION("audio and video") {}
  SECTION("video only") { name = "h264_video.mp4"; }
  SECTION("transport stream") { name = "h264_aac.ts"; }
  auto native = OpenNative(SamplePath(name));
  const auto result = AvProbe::Probe(SamplePath(name));
  REQUIRE(result.info.has_value());
  CHECK(result.error == nullptr);
  const auto& info = *result.info;
  REQUIRE(info.streams.size() == native->nb_streams);
  CHECK(info.start_time_us == native->start_time);
  CHECK(info.duration_us == native->duration);
  CHECK(info.metadata == Metadata(native->metadata));
  CHECK(info.timestamp_discontinuities ==
        static_cast<bool>(native->iformat->flags & AVFMT_TS_DISCONT));
  int video_index = -1;
  int audio_index = -1;
  for (std::size_t index = 0; index < info.streams.size(); ++index) {
    const auto& stream = info.streams[index];
    auto* expected = native->streams[index];
    const auto* parameters = stream.codec_parameters.get();
    REQUIRE(parameters != nullptr);
    CHECK(stream.stream_index == expected->index);
    CHECK(stream.time_base.num == expected->time_base.num);
    CHECK(stream.time_base.den == expected->time_base.den);
    CHECK(stream.disposition == expected->disposition);
    CHECK(stream.metadata == Metadata(expected->metadata));
    CHECK(parameters->codec_type == expected->codecpar->codec_type);
    CHECK(parameters->codec_id == expected->codecpar->codec_id);
    CHECK(parameters->width == expected->codecpar->width);
    CHECK(parameters->height == expected->codecpar->height);
    CHECK(parameters->sample_rate == expected->codecpar->sample_rate);
    CHECK(parameters->ch_layout.nb_channels ==
          expected->codecpar->ch_layout.nb_channels);
    REQUIRE(parameters->extradata_size == expected->codecpar->extradata_size);
    if (parameters->extradata_size > 0) {
      CHECK(std::memcmp(parameters->extradata, expected->codecpar->extradata,
                        static_cast<std::size_t>(parameters->extradata_size)) ==
            0);
    }
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (video_index < 0) video_index = expected->index;
      const auto fps = av_guess_frame_rate(native.get(), expected, nullptr);
      CHECK(stream.frame_rate.num == fps.num);
      CHECK(stream.frame_rate.den == fps.den);
      const auto aspect =
          av_guess_sample_aspect_ratio(native.get(), expected, nullptr);
      CHECK(stream.sample_aspect_ratio.num == aspect.num);
      CHECK(stream.sample_aspect_ratio.den == aspect.den);
    }
    if (parameters->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0) {
      audio_index = expected->index;
    }
  }
  CHECK(info.video_stream_index == video_index);
  CHECK(info.audio_stream_index == audio_index);
  if (name == "h264_video.mp4") CHECK(info.audio_stream_index == -1);
}

TEST_CASE("AvProbe returns failures without throwing", "[av-probe][error]") {
  std::string url = "mw-invalid-protocol://media";
  SECTION("unsupported protocol") {}
  SECTION("missing local file") {
    url = SamplePath("av-probe-file-that-does-not-exist.mp4");
  }
  SECTION("empty URL") { url.clear(); }
  CHECK_NOTHROW(AvProbe::Probe(url));
  const auto result = AvProbe::Probe(url);
  CHECK_FALSE(result.info.has_value());
  REQUIRE(result.error != nullptr);
  if (!url.empty()) CHECK(ExceptionCode(result.error) < 0);
}

TEST_CASE("AvProbe results own independent stream descriptions",
          "[av-probe][lifetime]") {
  auto first = AvProbe::Probe(SamplePath());
  REQUIRE(first.info.has_value());
  REQUIRE_FALSE(first.info->streams.empty());
  const auto* first_parameters =
      first.info->streams.front().codec_parameters.get();
  const int expected_width = first_parameters->width;
  REQUIRE(first_parameters->extradata_size > 0);
  const auto expected_extradata_byte = first_parameters->extradata[0];
  {
    auto second = AvProbe::Probe(SamplePath());
    REQUIRE(second.info.has_value());
    auto* second_parameters =
        second.info->streams.front().codec_parameters.get();
    CHECK(first_parameters != second_parameters);
    REQUIRE(first_parameters->extradata_size > 0);
    CHECK(first_parameters->extradata != second_parameters->extradata);
    second_parameters->width = expected_width + 1;
    CHECK(first_parameters->width == expected_width);
  }
  CHECK_FALSE(AvProbe::Probe("mw-invalid-protocol://media").info.has_value());
  CHECK(first_parameters->width == expected_width);
  CHECK(first_parameters->extradata[0] == expected_extradata_byte);
  CHECK(AvProbe::Probe(SamplePath()).info.has_value());
}

// One request per instance; all socket waits are bounded so failed probes
// cannot leave the test worker blocked during destruction.
class HttpProbeSocket final {
 public:
#ifdef _WIN32
  using Handle = SOCKET;
  static constexpr Handle kInvalid = INVALID_SOCKET;
#else
  using Handle = int;
  static constexpr Handle kInvalid = -1;
#endif

  HttpProbeSocket() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("Cannot initialize probe test sockets");
#endif
    handle_ = socket(AF_INET, SOCK_STREAM, 0);
    if (handle_ == kInvalid) {
#ifdef _WIN32
      WSACleanup();
#endif
      throw std::runtime_error("Cannot create HTTP probe socket");
    }
  }
  ~HttpProbeSocket() {
    Close(handle_);
#ifdef _WIN32
    WSACleanup();
#endif
  }
  HttpProbeSocket(const HttpProbeSocket&) = delete;
  HttpProbeSocket& operator=(const HttpProbeSocket&) = delete;
  Handle get() const { return handle_; }
  static void Close(Handle handle) {
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
  }

 private:
  Handle handle_ = kInvalid;
};

class HeaderCheckingHttpServer final {
 public:
  HeaderCheckingHttpServer() {
    std::ifstream file(SamplePath(), std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read HTTP probe fixture");
    media_.assign(std::istreambuf_iterator<char>(file), {});
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener_.get(), 1) != 0)
      throw std::runtime_error("Cannot bind HTTP probe socket");
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address),
                    &size) != 0)
      throw std::runtime_error("Cannot query HTTP probe port");
    url_ =
        fmt::format("http://127.0.0.1:{}/sample.mp4", ntohs(address.sin_port));
    worker_ = std::thread([this] {
      try {
        Serve();
      } catch (...) {
        error_ = std::current_exception();
      }
    });
  }
  ~HeaderCheckingHttpServer() { Finish(); }
  const std::string& url() const { return url_; }
  void Finish() {
    if (worker_.joinable()) worker_.join();
  }
  bool authorized() const { return authorized_; }
  std::exception_ptr error() const { return error_; }

 private:
  static bool Ready(HttpProbeSocket::Handle socket, bool writing = false) {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(socket, &descriptors);
    timeval timeout{3, 0};
    return select(static_cast<int>(socket + 1),
                  writing ? nullptr : &descriptors,
                  writing ? &descriptors : nullptr, nullptr, &timeout) > 0;
  }
  void Serve() {
    if (!Ready(listener_.get()))
      throw std::runtime_error("HTTP probe connection timed out");
    const auto client = accept(listener_.get(), nullptr, nullptr);
    if (client == HttpProbeSocket::kInvalid)
      throw std::runtime_error("Cannot accept HTTP probe connection");
    // Close the accepted socket even when request parsing or allocation fails.
    const auto close_client = [](const HttpProbeSocket::Handle* socket) {
      HttpProbeSocket::Close(*socket);
    };
    std::unique_ptr<const HttpProbeSocket::Handle, decltype(close_client)>
        connection(&client, close_client);
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
      if (request.size() >= 16384 || !Ready(client))
        throw std::runtime_error("HTTP probe request timed out or too large");
      char buffer[4096];
      const auto count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0)
        throw std::runtime_error("Cannot read HTTP probe request");
      request.append(buffer, static_cast<std::size_t>(count));
    }
    authorized_ =
        request.find("\r\nReferer: https://probe.test/\r\n") !=
            std::string::npos &&
        request.find("\r\nUser-Agent: mw-probe-test\r\n") != std::string::npos;
    const auto response =
        authorized_ ? fmt::format(
                          "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
                          "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                          media_.size(), media_)
                    : std::string(
                          "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n");
    std::size_t sent = 0;
    while (sent < response.size()) {
      if (!Ready(client, true))
        throw std::runtime_error("HTTP probe response timed out");
#ifdef _WIN32
      constexpr int kSendFlags = 0;
#else
      constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
      const auto count =
          send(client, response.data() + sent,
               static_cast<int>(response.size() - sent), kSendFlags);
      if (count <= 0)
        throw std::runtime_error("Cannot send HTTP probe response");
      sent += static_cast<std::size_t>(count);
    }
  }

  HttpProbeSocket listener_;
  std::string media_;
  std::string url_;
  std::thread worker_;
  bool authorized_ = false;
  std::exception_ptr error_;
};

TEST_CASE("AvProbe forwards HTTP headers and user agent to the input",
          "[av-probe][network][options]") {
  HeaderCheckingHttpServer server;
  std::map<std::string, std::string> options{
      {"headers", "Referer: https://probe.test/\r\n"},
      {"user_agent", "mw-probe-test"}};
  bool authorized = true;
  SECTION("both required options") {}
  SECTION("missing headers") {
    options.erase("headers");
    authorized = false;
  }
  SECTION("missing user agent") {
    options.erase("user_agent");
    authorized = false;
  }
  const auto result = AvProbe::Probe(server.url(), options);
  server.Finish();
  CHECK(server.error() == nullptr);
  CHECK(server.authorized() == authorized);
  if (authorized) {
    REQUIRE(result.info.has_value());
    CHECK(result.error == nullptr);
    CHECK(result.info->video_stream_index >= 0);
    CHECK(result.info->audio_stream_index >= 0);
  } else {
    CHECK_FALSE(result.info.has_value());
    CHECK(ExceptionCode(result.error) == AVERROR_HTTP_FORBIDDEN);
  }
}

#ifndef _WIN32

// TCP connects successfully, but the peer never provides probe data.
class SilentTcpServer final {
 public:
  SilentTcpServer() {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("Cannot create probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener_, 1) != 0) {
      close(listener_);
      throw std::runtime_error("Cannot bind probe socket");
    }
    socklen_t size = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) !=
        0) {
      close(listener_);
      throw std::runtime_error("Cannot query probe socket port");
    }
    url_ = fmt::format("tcp://127.0.0.1:{}", ntohs(address.sin_port));
  }

  ~SilentTcpServer() { close(listener_); }

  const std::string& url() const { return url_; }

 private:
  int listener_ = -1;
  std::string url_;
};

TEST_CASE("AvProbe interrupts a stalled input after thirty seconds",
          "[av-probe][timeout]") {
  SilentTcpServer server;
  const auto start = std::chrono::steady_clock::now();
  const auto result = AvProbe::Probe(server.url());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK_FALSE(result.info.has_value());
  CHECK(ExceptionCode(result.error) == AVERROR(ETIMEDOUT));
  CHECK(elapsed >= 29s);
  CHECK(elapsed < 35s);
}

#endif

}  // namespace
