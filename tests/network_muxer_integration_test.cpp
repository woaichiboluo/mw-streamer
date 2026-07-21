#include "mw/streamer/internal/network_muxer.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/ffmpeg_interrupt.h"
#include "mw/streamer/internal/demuxer.h"
#include "mw/streamer/internal/output_worker.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {
namespace {

#ifndef _WIN32
class StallingTcpServer final {
 public:
  explicit StallingTcpServer(bool complete_rtsp_handshake = false,
                             bool start_immediately = true)
      : complete_rtsp_handshake_(complete_rtsp_handshake) {
    listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to create test TCP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
      const std::error_code error(errno, std::generic_category());
      close(listen_socket_);
      throw std::system_error(error, "failed to start test TCP server");
    }

    socklen_t address_size = sizeof(address);
    if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address),
                    &address_size) != 0) {
      const std::error_code error(errno, std::generic_category());
      close(listen_socket_);
      throw std::system_error(error, "failed to query test TCP port");
    }
    port_ = ntohs(address.sin_port);
    if (start_immediately) {
      StartListening();
    }
  }

  ~StallingTcpServer() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
    }
    condition_.notify_all();
    shutdown(listen_socket_, SHUT_RDWR);
    close(listen_socket_);
    const int client_socket = client_socket_.load(std::memory_order_acquire);
    if (client_socket >= 0) {
      shutdown(client_socket, SHUT_RDWR);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  StallingTcpServer(const StallingTcpServer&) = delete;
  StallingTcpServer& operator=(const StallingTcpServer&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void StartListening() {
    if (started_ || listen(listen_socket_, 1) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to start test TCP server");
    }
    started_ = true;
    thread_ = std::thread(&StallingTcpServer::Run, this);
  }

 private:
  static bool SendAll(int socket, std::string_view response) {
    std::size_t sent_size = 0;
    while (sent_size < response.size()) {
      const ssize_t result = send(socket, response.data() + sent_size,
                                  response.size() - sent_size, MSG_NOSIGNAL);
      if (result <= 0) {
        return false;
      }
      sent_size += static_cast<std::size_t>(result);
    }
    return true;
  }

  static bool ReceiveRtspRequest(int socket, std::string* buffered_data,
                                 std::string* method, std::string* transport) {
    constexpr std::string_view kHeaderEnd = "\r\n\r\n";
    std::size_t header_end = buffered_data->find(kHeaderEnd);
    while (header_end == std::string::npos) {
      char data[4'096];
      const ssize_t received = recv(socket, data, sizeof(data), 0);
      if (received <= 0) {
        return false;
      }
      buffered_data->append(data, static_cast<std::size_t>(received));
      header_end = buffered_data->find(kHeaderEnd);
    }

    const std::size_t header_size = header_end + kHeaderEnd.size();
    const std::string header = buffered_data->substr(0, header_end);
    const std::size_t method_end = header.find(' ');
    if (method_end == std::string::npos) {
      return false;
    }
    *method = header.substr(0, method_end);

    std::size_t content_length = 0;
    std::size_t line_start = 0;
    while (line_start < header.size()) {
      const std::size_t line_end = header.find("\r\n", line_start);
      const std::string_view line(
          header.data() + line_start,
          (line_end == std::string::npos ? header.size() : line_end) -
              line_start);
      constexpr std::string_view kContentLength = "Content-Length:";
      constexpr std::string_view kTransport = "Transport:";
      if (line.compare(0, kContentLength.size(), kContentLength) == 0) {
        content_length = static_cast<std::size_t>(
            std::stoul(std::string(line.substr(kContentLength.size()))));
      } else if (line.compare(0, kTransport.size(), kTransport) == 0) {
        const std::size_t value_start =
            line.find_first_not_of(' ', kTransport.size());
        *transport = value_start == std::string_view::npos
                         ? std::string()
                         : std::string(line.substr(value_start));
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 2;
    }

    while (buffered_data->size() < header_size + content_length) {
      char data[4'096];
      const ssize_t received = recv(socket, data, sizeof(data), 0);
      if (received <= 0) {
        return false;
      }
      buffered_data->append(data, static_cast<std::size_t>(received));
    }
    buffered_data->erase(0, header_size + content_length);
    return true;
  }

  static bool CompleteRtspHandshake(int socket) {
    std::string buffered_data;
    for (int cseq = 1; cseq <= 4; ++cseq) {
      std::string method;
      std::string transport;
      if (!ReceiveRtspRequest(socket, &buffered_data, &method, &transport)) {
        return false;
      }
      std::string extra_headers;
      if (method == "OPTIONS") {
        extra_headers = "Public: OPTIONS, ANNOUNCE, SETUP, RECORD\r\n";
      } else if (method == "SETUP") {
        extra_headers =
            "Transport: " + transport + "\r\nSession: 12345678;timeout=60\r\n";
      } else if (method == "RECORD") {
        extra_headers = "Session: 12345678\r\n";
      }
      const std::string response =
          "RTSP/1.0 200 OK\r\nCSeq: " + std::to_string(cseq) + "\r\n" +
          extra_headers + "\r\n";
      if (!SendAll(socket, response)) {
        return false;
      }
      if (method == "RECORD") {
        return true;
      }
    }
    return false;
  }

  void Run() {
    const int client_socket = accept(listen_socket_, nullptr, nullptr);
    if (client_socket < 0) {
      return;
    }
    client_socket_.store(client_socket, std::memory_order_release);
    const int receive_buffer_size = 4'096;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size,
               sizeof(receive_buffer_size));
    if (complete_rtsp_handshake_ && !CompleteRtspHandshake(client_socket)) {
      client_socket_.store(-1, std::memory_order_release);
      close(client_socket);
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return stop_requested_; });
    lock.unlock();
    shutdown(client_socket, SHUT_RDWR);
    client_socket_.store(-1, std::memory_order_release);
    close(client_socket);
  }

  int listen_socket_ = -1;
  std::uint16_t port_ = 0;
  const bool complete_rtsp_handshake_;
  std::atomic<int> client_socket_{-1};
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stop_requested_ = false;
  bool started_ = false;
  std::thread thread_;
};
#endif

std::filesystem::path MakeOutputPath(std::string_view extension) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("mw-streamer-network-muxer-" + std::to_string(suffix) + "." +
          std::string(extension));
}

TEST(NetworkMuxerIntegrationTest, WritesRtmpSessionAsFlv) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  const std::filesystem::path output_path = MakeOutputPath("flv");

  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  OutputConfig config;
  config.protocol = OutputProtocol::kRtmp;
  config.url = output_path.string();

  NetworkMuxer muxer;
  ASSERT_NO_THROW(muxer.Open(config, input.video_stream().codec_parameters,
                             input.video_stream().time_base));
  int input_packet_count = 0;
  while (auto packet = input.Read()) {
    muxer.WriteVideo(std::move(*packet));
    ++input_packet_count;
  }
  ASSERT_NO_THROW(muxer.Finish());

  Demuxer output;
  ASSERT_NO_THROW(output.Open(output_path.string()));
  EXPECT_EQ(output.video_stream().codec_parameters.get()->codec_id,
            AV_CODEC_ID_H264);
  int output_packet_count = 0;
  while (output.Read().has_value()) {
    ++output_packet_count;
  }
  EXPECT_EQ(output_packet_count, input_packet_count);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

TEST(NetworkMuxerIntegrationTest, WritesSrtSessionAsMpegTs) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  const std::filesystem::path output_path = MakeOutputPath("ts");

  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  OutputConfig config;
  config.protocol = OutputProtocol::kSrt;
  config.url = output_path.string();

  NetworkMuxer muxer;
  ASSERT_NO_THROW(muxer.Open(config, input.video_stream().codec_parameters,
                             input.video_stream().time_base));
  int input_packet_count = 0;
  while (auto packet = input.Read()) {
    muxer.WriteVideo(std::move(*packet));
    ++input_packet_count;
  }
  ASSERT_NO_THROW(muxer.Finish());

  Demuxer output;
  ASSERT_NO_THROW(output.Open(output_path.string()));
  int output_packet_count = 0;
  while (output.Read().has_value()) {
    ++output_packet_count;
  }
  EXPECT_EQ(output_packet_count, input_packet_count);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

TEST(NetworkMuxerIntegrationTest, RejectsDuplicateVideoDtsAfterRescale) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  const std::filesystem::path output_path = MakeOutputPath("flv");

  {
    Demuxer input;
    input.Open(input_path.string());
    auto first_packet = input.Read();
    ASSERT_TRUE(first_packet.has_value());
    ffmpeg::Packet duplicate = first_packet->Ref();

    OutputConfig config;
    config.protocol = OutputProtocol::kRtmp;
    config.url = output_path.string();
    NetworkMuxer muxer;
    muxer.Open(config, input.video_stream().codec_parameters,
               input.video_stream().time_base);
    muxer.WriteVideo(std::move(*first_packet));
    EXPECT_THROW(muxer.WriteVideo(std::move(duplicate)), Error);
    muxer.Abort();
  }

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

TEST(NetworkMuxerIntegrationTest, ReportsUnreachableRtspEndpoint) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  OutputConfig config;
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:1/missing";

  NetworkMuxer muxer;
  EXPECT_THROW(muxer.Open(config, input.video_stream().codec_parameters,
                          input.video_stream().time_base),
               Error);
}

TEST(NetworkMuxerIntegrationTest, ReportsUnreachableRtmpEndpoint) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  OutputConfig config;
  config.protocol = OutputProtocol::kRtmp;
  config.url = "rtmp://127.0.0.1:1/missing";

  NetworkMuxer muxer;
  EXPECT_THROW(muxer.Open(config, input.video_stream().codec_parameters,
                          input.video_stream().time_base),
               Error);
}

#ifndef _WIN32
TEST(NetworkMuxerIntegrationTest, OpenStopsAtConfiguredDeadline) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  StallingTcpServer server;
  OutputConfig config;
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:" + std::to_string(server.port()) + "/stalled";
  config.open_timeout = std::chrono::milliseconds(100);

  FfmpegInterrupt interrupt;
  NetworkMuxer muxer;
  const auto start = std::chrono::steady_clock::now();
  try {
    muxer.Open(config, input.video_stream().codec_parameters,
               input.video_stream().time_base, nullptr, AVRational{0, 1},
               &interrupt);
    FAIL() << "expected network output open timeout";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kUnavailable);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(NetworkMuxerIntegrationTest, WriteStopsAtConfiguredDeadline) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  std::optional<ffmpeg::Packet> source_packet = input.Read();
  ASSERT_TRUE(source_packet.has_value());
  StallingTcpServer server(/*complete_rtsp_handshake=*/true);
  OutputConfig config;
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:" + std::to_string(server.port()) + "/stalled";
  config.open_timeout = std::chrono::seconds(2);
  config.write_timeout = std::chrono::milliseconds(100);

  FfmpegInterrupt interrupt;
  NetworkMuxer muxer;
  ASSERT_NO_THROW(muxer.Open(config, input.video_stream().codec_parameters,
                             input.video_stream().time_base, nullptr,
                             AVRational{0, 1}, &interrupt));

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  for (std::int64_t index = 0; index < 100'000 && !timed_out; ++index) {
    ffmpeg::Packet packet = source_packet->Ref();
    packet.get()->dts = index * 2'048;
    packet.get()->pts = packet.get()->dts;
    try {
      muxer.WriteVideo(std::move(packet));
    } catch (const Error& error) {
      EXPECT_EQ(error.code(), StatusCode::kUnavailable);
      timed_out = true;
    }
  }
  EXPECT_TRUE(timed_out);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
  muxer.Abort();
}

TEST(NetworkMuxerIntegrationTest, OutputWorkerStopsDuringReconnectBackoff) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  StallingTcpServer server;
  OutputConfig config;
  config.id = "stalled";
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:" + std::to_string(server.port()) + "/stalled";
  config.open_timeout = std::chrono::milliseconds(100);
  config.reconnect.max_retries = 1;
  config.reconnect.initial_backoff = std::chrono::seconds(30);
  config.reconnect.maximum_backoff = std::chrono::seconds(30);
  config.reconnect.jitter_ratio = 0.0;

  std::mutex events_mutex;
  std::vector<EndpointEventType> events;
  OutputWorker worker(
      config, input.video_stream().codec_parameters,
      input.video_stream().time_base, nullptr, AVRational{0, 1},
      [&events_mutex, &events](EndpointEventType type, const Status&) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(type);
      });
  ASSERT_NO_THROW(worker.Start());

  const auto stop_start = std::chrono::steady_clock::now();
  worker.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - stop_start,
            std::chrono::seconds(1));

  std::lock_guard<std::mutex> lock(events_mutex);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front(), EndpointEventType::kReconnecting);
}

TEST(NetworkMuxerIntegrationTest,
     OutputWorkerIgnoresQueueOverflowFromPreviousReconnectBackoff) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  std::optional<ffmpeg::Packet> source_packet;
  while (std::optional<ffmpeg::Packet> packet = input.Read()) {
    if ((packet->get()->flags & AV_PKT_FLAG_KEY) != 0) {
      source_packet = std::move(packet);
      break;
    }
  }
  ASSERT_TRUE(source_packet.has_value());

  StallingTcpServer server(/*complete_rtsp_handshake=*/true,
                           /*start_immediately=*/false);
  OutputConfig config;
  config.id = "recover-after-overflow";
  config.protocol = OutputProtocol::kRtsp;
  config.url =
      "rtsp://127.0.0.1:" + std::to_string(server.port()) + "/recovered";
  config.open_timeout = std::chrono::milliseconds(200);
  config.packet_queue_capacity = 1;
  config.reconnect.max_retries = 1;
  config.reconnect.initial_backoff = std::chrono::milliseconds(200);
  config.reconnect.maximum_backoff = std::chrono::milliseconds(200);
  config.reconnect.jitter_ratio = 0.0;

  std::mutex events_mutex;
  std::condition_variable events_changed;
  std::vector<EndpointEventType> events;
  OutputWorker worker(config, input.video_stream().codec_parameters,
                      input.video_stream().time_base, nullptr, AVRational{0, 1},
                      [&events_mutex, &events_changed, &events](
                          EndpointEventType type, const Status&) {
                        {
                          std::lock_guard<std::mutex> lock(events_mutex);
                          events.push_back(type);
                        }
                        events_changed.notify_all();
                      });
  ASSERT_NO_THROW(worker.Start());

  EncodedPacket first(EncodedMediaType::kVideo, source_packet->Ref());
  EncodedPacket overflow(EncodedMediaType::kVideo, source_packet->Ref());
  EncodedPacket recovered(EncodedMediaType::kVideo, source_packet->Ref());
  ASSERT_TRUE(worker.Enqueue(first));
  ASSERT_FALSE(worker.Enqueue(overflow));
  ASSERT_NO_THROW(server.StartListening());
  ASSERT_TRUE(worker.Enqueue(recovered));

  {
    std::unique_lock<std::mutex> lock(events_mutex);
    ASSERT_TRUE(
        events_changed.wait_for(lock, std::chrono::seconds(2), [&events] {
          return std::find(events.begin(), events.end(),
                           EndpointEventType::kRecovered) != events.end() ||
                 std::find(events.begin(), events.end(),
                           EndpointEventType::kFailed) != events.end();
        }));
    EXPECT_NE(
        std::find(events.begin(), events.end(), EndpointEventType::kRecovered),
        events.end());
    EXPECT_EQ(
        std::find(events.begin(), events.end(), EndpointEventType::kFailed),
        events.end());
    EXPECT_FALSE(events_changed.wait_for(
        lock, std::chrono::milliseconds(700), [&events] {
          return std::find(events.begin(), events.end(),
                           EndpointEventType::kFailed) != events.end();
        }));
  }
  worker.Stop();
}

TEST(NetworkMuxerIntegrationTest, OutputWorkerCancelsBlockedWriteOnStop) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  std::optional<ffmpeg::Packet> source_packet = input.Read();
  ASSERT_TRUE(source_packet.has_value());
  ASSERT_NE(source_packet->get()->flags & AV_PKT_FLAG_KEY, 0);
  StallingTcpServer server(/*complete_rtsp_handshake=*/true);
  OutputConfig config;
  config.id = "blocked-write";
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:" + std::to_string(server.port()) + "/stalled";
  config.open_timeout = std::chrono::seconds(2);
  config.write_timeout = std::chrono::seconds(30);
  config.packet_queue_capacity = 10'000;

  OutputWorker worker(config, input.video_stream().codec_parameters,
                      input.video_stream().time_base, nullptr, AVRational{0, 1},
                      {});
  ASSERT_NO_THROW(worker.Start());
  for (std::int64_t index = 0; index < 5'000; ++index) {
    ffmpeg::Packet packet = source_packet->Ref();
    packet.get()->dts = index * 2'048;
    packet.get()->pts = packet.get()->dts;
    ASSERT_TRUE(worker.Enqueue(
        EncodedPacket(EncodedMediaType::kVideo, std::move(packet))));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto stop_start = std::chrono::steady_clock::now();
  worker.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - stop_start,
            std::chrono::seconds(1));
}
#endif

}  // namespace
}  // namespace mw::streamer::internal
