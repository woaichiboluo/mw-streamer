#include "mw/streamer/input/av_input_proxy.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <fmt/format.h>

#include "mw/streamer/ffmpeg/av_dictionary.h"
#include "mw/streamer/ffmpeg/error.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvInputProxy;
using mw::streamer::AvInputProxyConfig;
using mw::streamer::AvInputProxyState;
using mw::streamer::AvInputProxyStatus;
using mw::streamer::AvInputReadStatus;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string SamplePath() {
  return fmt::format("{}/h264_aac.mp4", MW_AV_INPUT_TEST_DATA_DIR);
}

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct FormatDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct NativeSource {
  std::vector<ffmpeg::Packet> packets;
  mw::streamer::AvInputInfo info;
};

NativeSource ReadNativeSource() {
  AVFormatContext* context = nullptr;
  const int opened =
      avformat_open_input(&context, SamplePath().c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE(opened == 0);
  REQUIRE(avformat_find_stream_info(context, nullptr) >= 0);
  NativeSource source;
  source.info.start_time_us = context->start_time;
  source.info.duration_us = context->duration;
  source.info.metadata = ffmpeg::AvDictionary::ToMap(context->metadata);
  for (unsigned int index = 0; index < context->nb_streams; ++index) {
    const auto* stream = context->streams[index];
    ffmpeg::StreamInfo description{stream->index,
                                   ffmpeg::CodecParameters(*stream->codecpar),
                                   stream->time_base};
    description.disposition = stream->disposition;
    description.metadata = ffmpeg::AvDictionary::ToMap(stream->metadata);
    source.info.streams.push_back(std::move(description));
  }
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(context, packet.get());
    if (result == AVERROR_EOF) break;
    REQUIRE(result >= 0);
    source.packets.push_back(std::move(packet));
  }
  REQUIRE_FALSE(source.packets.empty());
  return source;
}

void CheckPacket(const ffmpeg::Packet& actual, const ffmpeg::Packet& expected) {
  CHECK(actual->pts == expected->pts);
  CHECK(actual->dts == expected->dts);
  CHECK(actual->duration == expected->duration);
  CHECK(actual->stream_index == expected->stream_index);
  CHECK(actual->flags == expected->flags);
  REQUIRE(actual->size == expected->size);
  if (actual->size > 0) {
    CHECK(std::memcmp(actual->data, expected->data,
                      static_cast<std::size_t>(actual->size)) == 0);
  }
}

int ExceptionCode(const std::exception_ptr& exception) {
  REQUIRE(exception != nullptr);
  try {
    std::rethrow_exception(exception);
  } catch (const ffmpeg::FfmpegException& error) {
    return error.error_code();
  }
  FAIL("Expected original FFmpeg exception");
  return 0;
}

class TemporarySource final {
 public:
  TemporarySource() {
    static std::atomic<unsigned int> sequence{0};
    path_ =
        std::filesystem::temp_directory_path() /
        fmt::format("mw-av-input-proxy-{}-{}.mp4",
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    sequence.fetch_add(1));
  }
  ~TemporarySource() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string path() const { return path_.string(); }
  void Restore() { std::filesystem::copy_file(SamplePath(), path_); }

 private:
  std::filesystem::path path_;
};

TEST_CASE("AvInputProxy nonloop source preserves every packet and reports EOF",
          "[av-input-proxy][packet]") {
  const auto source = ReadNativeSource();
  const auto& expected = source.packets;
  AvInputProxyConfig config;
  config.input.packet_capacity = 2;
  AvInputProxy proxy(config);
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kNull);
  proxy.Start(SamplePath());
  std::uint64_t generation = 0;
  std::shared_ptr<const mw::streamer::AvInputInfo> info;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    auto result = proxy.ReadPacket();
    REQUIRE(result.status == AvInputReadStatus::kPacket);
    REQUIRE(result.packet.has_value());
    REQUIRE(result.info != nullptr);
    if (index == 0) {
      generation = result.generation;
      info = result.info;
    }
    CHECK(result.generation == generation);
    CHECK(result.info == info);
    CheckPacket(*result.packet, expected[index]);
  }
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kEof);
  CHECK(proxy.TryReadPacket().status == AvInputReadStatus::kEof);
  REQUIRE(WaitFor(
      [&] { return proxy.status().state == AvInputProxyState::kEnded; }));
  CHECK(proxy.info() == info);
  REQUIRE(info != nullptr);
  CHECK(info->start_time_us == source.info.start_time_us);
  CHECK(info->duration_us == source.info.duration_us);
  CHECK(info->metadata == source.info.metadata);
  CHECK(info->streams == source.info.streams);
  REQUIRE(info->streams.size() == source.info.streams.size());
  for (std::size_t index = 0; index < info->streams.size(); ++index) {
    CHECK(info->streams[index].metadata == source.info.streams[index].metadata);
    CHECK(info->streams[index].disposition ==
          source.info.streams[index].disposition);
  }
  REQUIRE_FALSE(info->streams.empty());
  CHECK(info->streams.front().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_H264);
  CHECK(info->streams.front().time_base.num > 0);
  proxy.Stop();
  CHECK(proxy.status().state == AvInputProxyState::kEnded);
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kNull);
}

TEST_CASE("AvInputProxy exposes EOF before reopening each complete generation",
          "[av-input-proxy][loop][ownership]") {
  bool nonblocking = false;
  SECTION("blocking reads") {}
  SECTION("nonblocking reads") { nonblocking = true; }
  const auto source = ReadNativeSource();
  const auto& expected = source.packets;
  AvInputProxyConfig config;
  config.loop = true;
  config.input.packet_capacity = 1;
  AvInputProxy proxy(config);
  proxy.Start(SamplePath());
  std::optional<ffmpeg::Packet> retained_packet;
  std::shared_ptr<const mw::streamer::AvInputInfo> retained_info;
  auto read = [&] {
    if (!nonblocking) return proxy.ReadPacket();
    mw::streamer::AvInputProxyReadResult result;
    REQUIRE(WaitFor([&] {
      result = proxy.TryReadPacket();
      return result.status != AvInputReadStatus::kNull;
    }));
    return result;
  };
  std::uint64_t previous_generation = 0;
  for (int cycle = 0; cycle < 3; ++cycle) {
    std::uint64_t generation = 0;
    std::shared_ptr<const mw::streamer::AvInputInfo> info;
    for (std::size_t index = 0; index < expected.size(); ++index) {
      auto result = read();
      REQUIRE(result.status == AvInputReadStatus::kPacket);
      REQUIRE(result.packet.has_value());
      REQUIRE(result.info != nullptr);
      if (index == 0) {
        generation = result.generation;
        info = result.info;
        if (cycle == 0) {
          retained_packet = *result.packet;
          retained_info = info;
        } else {
          CHECK(generation == previous_generation + 1);
          CHECK(info != retained_info);
        }
      }
      CHECK(result.generation == generation);
      CHECK(result.info == info);
      CheckPacket(*result.packet, expected[index]);
    }
    // Draining packets alone must not retire the input or publish proxy EOF.
    std::this_thread::sleep_for(20ms);
    CHECK(proxy.status().generation == generation);
    CHECK(proxy.status().state == AvInputProxyState::kConnected);
    const auto eof = read();
    REQUIRE(eof.status == AvInputReadStatus::kEof);
    CHECK_FALSE(eof.packet.has_value());
    CHECK(eof.generation == generation);
    CHECK(eof.info == info);
    previous_generation = generation;
  }
  proxy.Stop();
  CHECK(proxy.status().state == AvInputProxyState::kStopped);
  REQUIRE(retained_packet.has_value());
  CheckPacket(*retained_packet, expected.front());
  REQUIRE(retained_info != nullptr);
  REQUIRE_FALSE(retained_info->streams.empty());
  CHECK(retained_info->streams.front().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_H264);
}

TEST_CASE(
    "AvInputProxy retains the dequeued final packet after closing its source",
    "[av-input-proxy][eof][ownership]") {
  bool loop = false;
  SECTION("normal EOF closes the source") {}
  SECTION("looping replaces the source") { loop = true; }
  const auto source = ReadNativeSource();
  AvInputProxyConfig config;
  config.loop = loop;
  config.input.packet_capacity = 1;
  AvInputProxy proxy(config);
  proxy.Start(SamplePath());
  mw::streamer::AvInputProxyReadResult tail;
  std::uint64_t generation = 0;
  std::shared_ptr<const mw::streamer::AvInputInfo> info;
  for (std::size_t index = 0; index < source.packets.size(); ++index) {
    auto result = proxy.ReadPacket();
    REQUIRE(result.status == AvInputReadStatus::kPacket);
    REQUIRE(result.packet.has_value());
    REQUIRE(result.info != nullptr);
    if (index == 0) {
      generation = result.generation;
      info = result.info;
    }
    CHECK(result.generation == generation);
    CHECK(result.info == info);
    CheckPacket(*result.packet, source.packets[index]);
    tail = std::move(result);
  }
  const auto eof = proxy.ReadPacket();
  REQUIRE(eof.status == AvInputReadStatus::kEof);
  CHECK(eof.generation == generation);
  CHECK(eof.info == info);
  if (loop) {
    REQUIRE(WaitFor([&] {
      const auto status = proxy.status();
      return status.state == AvInputProxyState::kConnected &&
             status.generation == generation + 1;
    }));
  } else {
    REQUIRE(WaitFor(
        [&] { return proxy.status().state == AvInputProxyState::kEnded; }));
  }
  REQUIRE(tail.packet.has_value());
  CHECK(tail.generation == generation);
  CHECK(tail.info == info);
  REQUIRE(tail.info != nullptr);
  CHECK(tail.info->metadata == source.info.metadata);
  CHECK(tail.info->streams == source.info.streams);
  CheckPacket(*tail.packet, source.packets.back());
  // A consumer can hand off its retained packet after the source is closed.
  // Source EOF does not acknowledge delivery to an application sink.
  const auto delivered_packet = tail.packet->Ref();
  CheckPacket(delivered_packet, source.packets.back());
  if (loop) {
    const auto next = proxy.ReadPacket();
    REQUIRE(next.status == AvInputReadStatus::kPacket);
    REQUIRE(next.packet.has_value());
    CHECK(next.generation == generation + 1);
    CHECK(next.info != tail.info);
    CheckPacket(*next.packet, source.packets.front());
    CheckPacket(*tail.packet, source.packets.back());
  } else {
    CHECK(proxy.ReadPacket().status == AvInputReadStatus::kEof);
  }
  proxy.Stop();
}

TEST_CASE("AvInputProxy delivers EOF even when the next open fails",
          "[av-input-proxy][eof][retry]") {
  TemporarySource file;
  file.Restore();
  const auto source = ReadNativeSource();
  AvInputProxyConfig config;
  config.loop = true;
  config.input.packet_capacity = 1;
  AvInputProxy proxy(config);
  proxy.Start(file.path());
  std::shared_ptr<const mw::streamer::AvInputInfo> info;
  std::uint64_t generation = 0;
  for (const auto& expected : source.packets) {
    auto result = proxy.ReadPacket();
    REQUIRE(result.status == AvInputReadStatus::kPacket);
    REQUIRE(result.packet.has_value());
    CheckPacket(*result.packet, expected);
    generation = result.generation;
    info = result.info;
  }
  REQUIRE(std::filesystem::remove(file.path()));
  mw::streamer::AvInputProxyReadResult eof;
  REQUIRE(WaitFor([&] {
    eof = proxy.TryReadPacket();
    return eof.status != AvInputReadStatus::kNull;
  }));
  REQUIRE(eof.status == AvInputReadStatus::kEof);
  CHECK_FALSE(eof.packet.has_value());
  CHECK(eof.generation == generation);
  CHECK(eof.info == info);
  REQUIRE(WaitFor([&] {
    return proxy.status().state == AvInputProxyState::kWaitingRetry;
  }));
  CHECK(proxy.status().generation == generation + 1);
  CHECK(proxy.TryReadPacket().status == AvInputReadStatus::kNull);
  proxy.Stop();
  CHECK(proxy.TryReadPacket().status == AvInputReadStatus::kNull);
}

TEST_CASE(
    "AvInputProxy bounds retries and preserves original connection errors",
    "[av-input-proxy][retry][error]") {
  TemporarySource missing;
  std::mutex mutex;
  std::vector<AvInputProxyStatus> events;
  std::thread::id observer_thread;
  bool single_observer_thread = true;
  AvInputProxyConfig config;
  config.max_retries = 2;
  AvInputProxy proxy(config);
  proxy.SetOnStateChanged([&](const auto& status) {
    static_cast<void>(proxy.status());
    static_cast<void>(proxy.info());
    std::lock_guard<std::mutex> lock(mutex);
    if (events.empty()) observer_thread = std::this_thread::get_id();
    single_observer_thread &= observer_thread == std::this_thread::get_id();
    events.push_back(status);
  });
  const auto started = std::chrono::steady_clock::now();
  proxy.Start(missing.path());
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kNull);
  CHECK(std::chrono::steady_clock::now() - started >= 2900ms);
  const auto final_status = proxy.status();
  CHECK(final_status.state == AvInputProxyState::kFailed);
  CHECK(ExceptionCode(final_status.error) == AVERROR(ENOENT));
  proxy.Stop();
  CHECK(proxy.status().state == AvInputProxyState::kFailed);
  std::lock_guard<std::mutex> lock(mutex);
  CHECK(single_observer_thread);
  CHECK(observer_thread != std::this_thread::get_id());
  std::size_t connecting = 0;
  std::vector<AvInputProxyStatus> retries;
  for (const auto& event : events) {
    if (event.state == AvInputProxyState::kConnecting) ++connecting;
    if (event.state == AvInputProxyState::kWaitingRetry)
      retries.push_back(event);
  }
  CHECK(connecting == 3);
  REQUIRE(retries.size() == 2);
  CHECK(retries[0].retry_count == 1);
  CHECK(retries[0].retry_delay == 1s);
  CHECK(retries[1].retry_count == 2);
  CHECK(retries[1].retry_delay == 2s);
  CHECK(ExceptionCode(retries[0].error) == AVERROR(ENOENT));
  REQUIRE_FALSE(events.empty());
  CHECK(events.back().error == final_status.error);
}

TEST_CASE("AvInputProxy reconnect success resets consecutive retry count",
          "[av-input-proxy][retry][recovery]") {
  TemporarySource source;
  AvInputProxyConfig config;
  config.input.packet_capacity = 1;
  AvInputProxy proxy(config);
  proxy.Start(source.path());
  REQUIRE(WaitFor([&] {
    return proxy.status().state == AvInputProxyState::kWaitingRetry;
  }));
  CHECK(proxy.status().retry_count == 1);
  CHECK(proxy.TryReadPacket().status == AvInputReadStatus::kNull);
  source.Restore();
  auto result = proxy.ReadPacket();
  REQUIRE(result.status == AvInputReadStatus::kPacket);
  REQUIRE(result.packet.has_value());
  REQUIRE(WaitFor(
      [&] { return proxy.status().state == AvInputProxyState::kConnected; }));
  const auto connected = proxy.status();
  CHECK(connected.state == AvInputProxyState::kConnected);
  CHECK(connected.retry_count == 0);
  CHECK(connected.retry_delay == 0s);
  CHECK(connected.error == nullptr);
  CHECK(connected.info == result.info);
  proxy.Stop();
  CHECK(proxy.status().state == AvInputProxyState::kStopped);
}

TEST_CASE(
    "AvInputProxy exponential backoff caps repeated retries at eight seconds",
    "[av-input-proxy][retry][backoff]") {
  TemporarySource missing;
  std::mutex mutex;
  std::vector<AvInputProxyStatus> events;
  AvInputProxyConfig config;
  config.max_retries = 5;
  AvInputProxy proxy(config);
  proxy.SetOnStateChanged([&](const auto& status) {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(status);
  });
  const auto started = std::chrono::steady_clock::now();
  proxy.Start(missing.path());
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kNull);
  CHECK(std::chrono::steady_clock::now() - started >= 22900ms);
  CHECK(proxy.status().state == AvInputProxyState::kFailed);
  proxy.Stop();
  std::lock_guard<std::mutex> lock(mutex);
  std::size_t connections = 0;
  std::vector<std::chrono::seconds> delays;
  for (const auto& event : events) {
    if (event.state == AvInputProxyState::kConnecting) ++connections;
    if (event.state == AvInputProxyState::kWaitingRetry) {
      CHECK(event.retry_count == delays.size() + 1);
      delays.push_back(event.retry_delay);
    }
  }
  CHECK(connections == 6);
  const std::vector<std::chrono::seconds> expected{1s, 2s, 4s, 8s, 8s};
  CHECK(delays == expected);
  REQUIRE_FALSE(events.empty());
  CHECK(events.back().state == AvInputProxyState::kFailed);
}

TEST_CASE("AvInputProxy Stop cancels backoff and wakes a waiting consumer",
          "[av-input-proxy][retry][stop]") {
  TemporarySource missing;
  AvInputProxy proxy;
  proxy.Start(missing.path());
  REQUIRE(WaitFor([&] {
    return proxy.status().state == AvInputProxyState::kWaitingRetry;
  }));
  auto received =
      std::async(std::launch::async, [&] { return proxy.ReadPacket(); });
  CHECK(received.wait_for(20ms) == std::future_status::timeout);
  const auto started = std::chrono::steady_clock::now();
  proxy.Stop();
  CHECK(std::chrono::steady_clock::now() - started < 500ms);
  REQUIRE(received.wait_for(1s) == std::future_status::ready);
  CHECK(received.get().status == AvInputReadStatus::kNull);
  CHECK(proxy.status().state == AvInputProxyState::kStopped);
  proxy.Stop();
  proxy.Start(SamplePath());
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kPacket);
}

TEST_CASE("AvInputProxy observers may request Stop and throw without deadlock",
          "[av-input-proxy][notification][stop]") {
  TemporarySource missing;
  std::atomic<bool> observed_retry{false};
  std::atomic<std::size_t> connections{0};
  AvInputProxy proxy;
  proxy.SetOnStateChanged([&](const auto& status) {
    static_cast<void>(proxy.status());
    if (status.state == AvInputProxyState::kConnecting) ++connections;
    if (status.state == AvInputProxyState::kWaitingRetry) {
      observed_retry.store(true);
      proxy.RequestStop();
    }
    throw std::runtime_error("observer failure");
  });
  proxy.Start(missing.path());
  CHECK(proxy.ReadPacket().status == AvInputReadStatus::kNull);
  REQUIRE(WaitFor([&] {
    return observed_retry.load() &&
           proxy.status().state == AvInputProxyState::kStopped;
  }));
  proxy.Stop();
  CHECK(connections.load() == 1);
}

#ifndef _WIN32

class StalledHttpServer final {
 public:
  StalledHttpServer() {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("Cannot create HTTP socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener_, 1) != 0) {
      close(listener_);
      throw std::runtime_error("Cannot bind HTTP socket");
    }
    socklen_t size = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) !=
        0) {
      close(listener_);
      throw std::runtime_error("Cannot query HTTP port");
    }
    url_ = fmt::format("http://127.0.0.1:{}/live.flv", ntohs(address.sin_port));
    worker_ = std::thread([this] {
      pollfd descriptor{listener_, POLLIN, 0};
      while (!stop_.load()) {
        if (poll(&descriptor, 1, 20) > 0) break;
      }
      if (stop_.load()) return;
      const int client = accept(listener_, nullptr, nullptr);
      if (client < 0) return;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepted_ = true;
      }
      changed_.notify_all();
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return stop_.load(); });
      shutdown(client, SHUT_RDWR);
      close(client);
    });
  }
  ~StalledHttpServer() {
    stop_.store(true);
    changed_.notify_all();
    worker_.join();
    close(listener_);
  }
  const std::string& url() const { return url_; }
  bool WaitAccepted() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 2s, [this] { return accepted_; });
  }

 private:
  int listener_ = -1;
  std::string url_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  bool accepted_ = false;
  std::thread worker_;
};

TEST_CASE("AvInputProxy Stop cancels opening I/O and empty consumers",
          "[av-input-proxy][network][stop]") {
  StalledHttpServer server;
  AvInputProxyConfig config;
  config.input.open_timeout = 5s;
  AvInputProxy proxy(config);
  proxy.Start(server.url());
  REQUIRE(server.WaitAccepted());
  CHECK(proxy.status().state == AvInputProxyState::kConnecting);
  CHECK(proxy.TryReadPacket().status == AvInputReadStatus::kNull);
  auto received =
      std::async(std::launch::async, [&] { return proxy.ReadPacket(); });
  CHECK(received.wait_for(20ms) == std::future_status::timeout);
  const auto started = std::chrono::steady_clock::now();
  proxy.Stop();
  CHECK(std::chrono::steady_clock::now() - started < 1s);
  REQUIRE(received.wait_for(1s) == std::future_status::ready);
  CHECK(received.get().status == AvInputReadStatus::kNull);
  CHECK(proxy.status().state == AvInputProxyState::kStopped);
}

#endif

}  // namespace
