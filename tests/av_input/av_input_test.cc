#include "mw/streamer/input/av_input.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <fmt/format.h>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/thread.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvInput;
using mw::streamer::AvInputConfig;
using mw::streamer::AvInputReadStatus;
using mw::streamer::AvInputState;
namespace ffmpeg = mw::streamer::ffmpeg;

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

std::string SamplePath(std::string_view name = "h264_aac.mp4") {
  return fmt::format("{}/{}", MW_AV_INPUT_TEST_DATA_DIR, name);
}

struct FormatDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, FormatDeleter> OpenNative(
    const std::string& path, bool ignore_editlist = false) {
  AVDictionary* options = nullptr;
  if (ignore_editlist) av_dict_set(&options, "ignore_editlist", "1", 0);
  AVFormatContext* context = nullptr;
  const int result =
      avformat_open_input(&context, path.c_str(), nullptr, &options);
  av_dict_free(&options);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE(result == 0);
  REQUIRE(avformat_find_stream_info(context, nullptr) >= 0);
  return owner;
}

void CheckPacket(const AVPacket& actual, const AVPacket& expected) {
  CHECK(actual.stream_index == expected.stream_index);
  CHECK(actual.pts == expected.pts);
  CHECK(actual.dts == expected.dts);
  CHECK(actual.duration == expected.duration);
  CHECK(actual.pos == expected.pos);
  CHECK(actual.flags == expected.flags);
  REQUIRE(actual.size == expected.size);
  if (actual.size > 0) {
    CHECK(std::memcmp(actual.data, expected.data,
                      static_cast<std::size_t>(actual.size)) == 0);
  }
  REQUIRE(actual.side_data_elems == expected.side_data_elems);
  for (int index = 0; index < actual.side_data_elems; ++index) {
    const auto& left = actual.side_data[index];
    const auto& right = expected.side_data[index];
    CHECK(left.type == right.type);
    REQUIRE(left.size == right.size);
    if (left.size > 0)
      CHECK(std::memcmp(left.data, right.data, left.size) == 0);
  }
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

class TrackFixture final {
 public:
  TrackFixture()
      : path_((std::filesystem::temp_directory_path() /
               fmt::format(
                   "mw-av-input-tracks-{}.mp4",
                   std::chrono::steady_clock::now().time_since_epoch().count()))
                  .string()) {}

  ~TrackFixture() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  void Write(const std::vector<AVMediaType>& types) {
    auto source = OpenNative(SamplePath());
    AVFormatContext* raw_output = nullptr;
    REQUIRE(avformat_alloc_output_context2(&raw_output, nullptr, "mp4",
                                           path_.c_str()) >= 0);
    const auto release = [](AVFormatContext* output) {
      avio_closep(&output->pb);
      avformat_free_context(output);
    };
    std::unique_ptr<AVFormatContext, decltype(release)> output(raw_output,
                                                               release);
    std::vector<int> source_indices;
    for (const auto type : types) {
      const int index =
          av_find_best_stream(source.get(), type, -1, -1, nullptr, 0);
      REQUIRE(index >= 0);
      source_indices.push_back(index);
      auto* stream = avformat_new_stream(output.get(), nullptr);
      REQUIRE(stream != nullptr);
      REQUIRE(avcodec_parameters_copy(stream->codecpar,
                                      source->streams[index]->codecpar) >= 0);
      stream->codecpar->codec_tag = 0;
      stream->time_base = source->streams[index]->time_base;
      // Prefer later tracks to prove selection does not use default flags.
      stream->disposition =
          source_indices.size() > 2 ? AV_DISPOSITION_DEFAULT : 0;
    }
    REQUIRE(avio_open(&output->pb, path_.c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(output.get(), nullptr) >= 0);
    ffmpeg::Packet packet;
    while (av_read_frame(source.get(), packet.get()) >= 0) {
      for (std::size_t index = 0; index < source_indices.size(); ++index) {
        if (source_indices[index] != packet->stream_index) continue;
        auto copy = packet.Ref();
        av_packet_rescale_ts(copy.get(),
                             source->streams[packet->stream_index]->time_base,
                             output->streams[index]->time_base);
        copy->stream_index = static_cast<int>(index);
        copy->pos = -1;
        REQUIRE(av_interleaved_write_frame(output.get(), copy.get()) >= 0);
      }
      packet.Unref();
    }
    REQUIRE(av_write_trailer(output.get()) >= 0);
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

TEST_CASE("AvInput selects the first video and audio tracks in source order",
          "[av-input][tracks]") {
  std::vector<AVMediaType> types{AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_VIDEO,
                                 AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_VIDEO};
  int expected_audio = 0;
  int expected_video = 1;
  SECTION("multiple tracks with later defaults") {}
  SECTION("video only") {
    types = {AVMEDIA_TYPE_VIDEO};
    expected_audio = -1;
    expected_video = 0;
  }
  SECTION("audio only") {
    types = {AVMEDIA_TYPE_AUDIO};
    expected_audio = 0;
    expected_video = -1;
  }
  TrackFixture fixture;
  fixture.Write(types);
  auto native = OpenNative(fixture.path());
  AvInputConfig config;
  config.packet_capacity = 1;
  AvInput input(config);
  input.Open(fixture.path());
  const auto info = input.info();
  REQUIRE(info.streams.size() == types.size());
  CHECK(info.video_stream_index == expected_video);
  CHECK(info.audio_stream_index == expected_audio);
  for (std::size_t index = 0; index < info.streams.size(); ++index) {
    CHECK(info.streams[index].stream_index == static_cast<int>(index));
    CHECK(info.streams[index].codec_parameters.get()->codec_type ==
          types[index]);
  }
  std::size_t count = 0;
  ffmpeg::Packet packet;
  while (av_read_frame(native.get(), packet.get()) >= 0) {
    if (packet->stream_index == expected_video ||
        packet->stream_index == expected_audio) {
      auto result = input.ReadPacket();
      REQUIRE(result.status == AvInputReadStatus::kPacket);
      REQUIRE(result.packet.has_value());
      CheckPacket(*result.packet->get(), *packet.get());
      ++count;
    }
    packet.Unref();
  }
  REQUIRE(count > 0);
  CHECK(input.ReadPacket().status == AvInputReadStatus::kEof);
}

TEST_CASE("AvInput preserves native packet content, timing and track order",
          "[av-input][packet]") {
  std::string name = "h264_aac.mp4";
  SECTION("MP4") {}
  SECTION("MPEG-TS") { name = "h264_aac.ts"; }
  auto native = OpenNative(SamplePath(name));
  AvInputConfig config;
  config.packet_capacity = 2;
  AvInput input(config);
  input.Open(SamplePath(name));
  const auto info = input.info();
  REQUIRE(info.streams.size() == native->nb_streams);
  CHECK(info.start_time_us == native->start_time);
  CHECK(info.duration_us == native->duration);
  for (std::size_t index = 0; index < info.streams.size(); ++index) {
    const auto& stream = info.streams[index];
    const auto* expected = native->streams[index];
    CHECK(stream.stream_index == expected->index);
    CHECK(stream.time_base.num == expected->time_base.num);
    CHECK(stream.time_base.den == expected->time_base.den);
    CHECK(stream.codec_parameters.get()->codec_id ==
          expected->codecpar->codec_id);
    const auto* parameters = stream.codec_parameters.get();
    REQUIRE(parameters->extradata_size == expected->codecpar->extradata_size);
    if (parameters->extradata_size > 0) {
      CHECK(std::memcmp(parameters->extradata, expected->codecpar->extradata,
                        static_cast<std::size_t>(parameters->extradata_size)) ==
            0);
    }
    CHECK(stream.disposition == expected->disposition);
  }
  ffmpeg::Packet expected;
  std::size_t count = 0;
  while (av_read_frame(native.get(), expected.get()) >= 0) {
    auto result = input.ReadPacket();
    REQUIRE(result.status == AvInputReadStatus::kPacket);
    REQUIRE(result.packet.has_value());
    CheckPacket(*result.packet->get(), *expected.get());
    expected.Unref();
    ++count;
  }
  REQUIRE(count > 0);
  CHECK(input.ReadPacket().status == AvInputReadStatus::kEof);
  CHECK(input.TryReadPacket().status == AvInputReadStatus::kEof);
  CHECK(input.status().state == AvInputState::kEnded);
}

TEST_CASE(
    "AvInput packet capacity bounds prefetch and cancellation unblocks it",
    "[av-input][backpressure][cancel]") {
  AvInputConfig config;
  SECTION("one packet") { config.packet_capacity = 1; }
  SECTION("two packets") { config.packet_capacity = 2; }
  AvInput input(config);
  input.Open(SamplePath());
  REQUIRE(WaitFor(
      [&] { return input.buffered_packets() == config.packet_capacity; }));
  std::this_thread::sleep_for(20ms);
  CHECK(input.buffered_packets() == config.packet_capacity);
  CHECK(input.status().state == AvInputState::kOpened);
  CHECK(input.TryReadPacket().status == AvInputReadStatus::kPacket);
  REQUIRE(WaitFor(
      [&] { return input.buffered_packets() == config.packet_capacity; }));
  input.RequestCancel();
  CHECK(input.ReadPacket().status == AvInputReadStatus::kNull);
  const auto start = std::chrono::steady_clock::now();
  input.Close();
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK(input.status().state == AvInputState::kClosed);
  input.Close();
}

TEST_CASE("AvInput cancellation wakes an empty reader before packet production",
          "[av-input][backpressure][cancel]") {
  AvInputConfig config;
  config.packet_capacity = 1;
  AvInput input(config);
  std::promise<void> opened;
  auto opening = opened.get_future();
  std::promise<void> release;
  auto allowed = release.get_future();
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvInputState::kOpened) {
      opened.set_value();
      // Hold producer startup so the consumer must wait on an empty queue.
      allowed.wait();
    }
  });
  std::exception_ptr open_error;
  std::thread opener([&] {
    try {
      input.Open(SamplePath());
    } catch (...) {
      open_error = std::current_exception();
    }
  });
  const auto ready = opening.wait_for(2s);
  std::promise<void> completed;
  auto completion = completed.get_future();
  std::thread consumer;
  AvInputReadStatus result = AvInputReadStatus::kPacket;
  bool has_packet = true;
  std::exception_ptr read_error;
  auto blocked = std::future_status::ready;
  if (ready == std::future_status::ready) {
    consumer = std::thread([&] {
      try {
        const auto output = input.ReadPacket();
        result = output.status;
        has_packet = output.packet.has_value();
      } catch (...) {
        read_error = std::current_exception();
      }
      completed.set_value();
    });
    blocked = completion.wait_for(40ms);
  }
  input.RequestCancel();
  const auto resumed = completion.wait_for(250ms);
  release.set_value();
  opener.join();
  if (consumer.joinable()) consumer.join();
  input.Close();
  REQUIRE(ready == std::future_status::ready);
  CHECK(blocked == std::future_status::timeout);
  CHECK(resumed == std::future_status::ready);
  CHECK_FALSE(open_error);
  CHECK_FALSE(read_error);
  CHECK(result == AvInputReadStatus::kNull);
  CHECK_FALSE(has_packet);
}

TEST_CASE("AvInput nonblocking reads preserve packet order and EOF",
          "[av-input][packet][nonblocking]") {
  AvInput input;
  CHECK(input.TryReadPacket().status == AvInputReadStatus::kNull);
  AvInputConfig config;
  config.packet_capacity = 2;
  AvInput opened(config);
  opened.Open(SamplePath());
  REQUIRE(WaitFor([&] { return opened.buffered_packets() > 0; }));
  auto native = OpenNative(SamplePath());
  ffmpeg::Packet expected;
  std::size_t count = 0;
  while (av_read_frame(native.get(), expected.get()) >= 0) {
    mw::streamer::AvInputReadResult result;
    REQUIRE(WaitFor([&] {
      result = opened.TryReadPacket();
      return result.status != AvInputReadStatus::kNull;
    }));
    REQUIRE(result.status == AvInputReadStatus::kPacket);
    REQUIRE(result.packet.has_value());
    CheckPacket(*result.packet->get(), *expected.get());
    expected.Unref();
    ++count;
  }
  REQUIRE(count > 0);
  CHECK(opened.ReadPacket().status == AvInputReadStatus::kEof);
  CHECK(opened.TryReadPacket().status == AvInputReadStatus::kEof);
}

TEST_CASE("AvInput returned packets and descriptions survive Close",
          "[av-input][ownership]") {
  AvInput input;
  input.Open(SamplePath());
  auto info = input.info();
  auto packet = input.ReadPacket();
  REQUIRE(packet.packet.has_value());
  const auto saved = packet.packet->Clone();
  input.Close();
  CheckPacket(*packet.packet->get(), *saved.get());
  REQUIRE_FALSE(info.streams.empty());
  CHECK(info.streams.front().codec_parameters.get()->codec_id !=
        AV_CODEC_ID_NONE);
  input.Open(SamplePath());
  CHECK(input.ReadPacket().status == AvInputReadStatus::kPacket);
}

TEST_CASE("AvInput validates configuration and lifecycle calls",
          "[av-input][contract]") {
  AvInputConfig config;
  SECTION("zero capacity") { config.packet_capacity = 0; }
  SECTION("zero open timeout") { config.open_timeout = 0ms; }
  SECTION("negative read timeout") { config.read_timeout = -1ms; }
  CHECK_THROWS_AS(AvInput(config), std::invalid_argument);
  AvInput input;
  CHECK(input.ReadPacket().status == AvInputReadStatus::kNull);
  CHECK(input.TryReadPacket().status == AvInputReadStatus::kNull);
  input.Open(SamplePath());
  CHECK_THROWS_AS(input.Open(SamplePath()), std::logic_error);
  CHECK_THROWS_AS(input.SetOnStateChanged({}), std::logic_error);
}

TEST_CASE("AvInput reading exposes only packet, EOF and null results",
          "[av-input][contract][packet]") {
  AvInput input;
  auto [closed_status, closed_packet] = input.TryReadPacket();
  CHECK(closed_status == AvInputReadStatus::kNull);
  CHECK_FALSE(closed_packet.has_value());
  input.Open(SamplePath());
  auto [first_status, first_packet] = input.ReadPacket();
  CHECK(first_status == AvInputReadStatus::kPacket);
  REQUIRE(first_packet.has_value());
  for (;;) {
    auto [read_status, packet] = input.ReadPacket();
    if (read_status == AvInputReadStatus::kEof) {
      CHECK_FALSE(packet.has_value());
      break;
    }
    REQUIRE(read_status == AvInputReadStatus::kPacket);
    REQUIRE(packet.has_value());
  }
  input.Close();
  auto [stopped_status, stopped_packet] = input.ReadPacket();
  CHECK(stopped_status == AvInputReadStatus::kNull);
  CHECK_FALSE(stopped_packet.has_value());
}

TEST_CASE("AvInput forwards input format and effective FFmpeg options",
          "[av-input][options]") {
  AvInputConfig config;
  config.input_format_name = "mov";
  config.options["ignore_editlist"] = "1";
  AvInput input(config);
  input.Open(SamplePath());
  auto expected = OpenNative(SamplePath(), true);
  auto unchanged = OpenNative(SamplePath());
  ffmpeg::Packet native_packet;
  ffmpeg::Packet unchanged_packet;
  REQUIRE(av_read_frame(expected.get(), native_packet.get()) >= 0);
  REQUIRE(av_read_frame(unchanged.get(), unchanged_packet.get()) >= 0);
  REQUIRE(native_packet->pts != unchanged_packet->pts);
  auto result = input.ReadPacket();
  REQUIRE(result.packet.has_value());
  CheckPacket(*result.packet->get(), *native_packet.get());
}

TEST_CASE("AvInput opening failures preserve their error and require Close",
          "[av-input][error][options]") {
  AvInputConfig config;
  std::string path = SamplePath();
  SECTION("invalid known option") { config.options["probesize"] = "invalid"; }
  SECTION("unknown input format") {
    config.input_format_name = "mw_nonexistent_demuxer";
  }
  SECTION("missing file") { path += ".missing"; }
  std::exception_ptr observed_error;
  AvInput input(config);
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvInputState::kFailed) observed_error = status.error;
  });
  int opening_error_code = 0;
  std::string opening_error_message;
  try {
    input.Open(path);
    FAIL("Expected opening failure");
  } catch (const ffmpeg::FfmpegException& error) {
    opening_error_code = error.error_code();
    opening_error_message = error.what();
  }
  const auto status = input.status();
  CHECK(status.state == AvInputState::kFailed);
  REQUIRE(opening_error_code < 0);
  REQUIRE_FALSE(opening_error_message.empty());
  // Recapturing an exception may copy it, so compare its contents, not
  // identity.
  for (const auto& exception : {status.error, observed_error}) {
    REQUIRE(exception != nullptr);
    try {
      std::rethrow_exception(exception);
    } catch (const ffmpeg::FfmpegException& error) {
      CHECK(error.error_code() == opening_error_code);
      CHECK(std::string(error.what()) == opening_error_message);
    }
  }
  const auto result = input.ReadPacket();
  CHECK(result.status == AvInputReadStatus::kNull);
  CHECK_FALSE(result.packet.has_value());
  CHECK_THROWS_AS(input.Open(SamplePath()), std::logic_error);
  input.Close();
  CHECK(ExceptionCode(observed_error) == opening_error_code);
}

TEST_CASE("AvInput state observers can query snapshots and throw safely",
          "[av-input][notification]") {
  std::mutex mutex;
  std::vector<AvInputState> events;
  AvInput input;
  input.SetOnStateChanged([&](const auto& status) {
    const auto snapshot = input.status();
    const auto info = input.info();
    static_cast<void>(snapshot);
    static_cast<void>(info);
    {
      std::lock_guard<std::mutex> lock(mutex);
      events.push_back(status.state);
    }
    throw std::runtime_error("observer failure");
  });
  input.Open(SamplePath());
  while (input.ReadPacket().status == AvInputReadStatus::kPacket) {
  }
  input.Close();
  std::lock_guard<std::mutex> lock(mutex);
  REQUIRE(events.size() == 4);
  CHECK(events[0] == AvInputState::kOpening);
  CHECK(events[1] == AvInputState::kOpened);
  CHECK(events[2] == AvInputState::kEnded);
  CHECK(events[3] == AvInputState::kClosed);
}

TEST_CASE("AvInput EOF is notified once after buffered packets are consumed",
          "[av-input][notification][packet]") {
  std::atomic<std::size_t> ended_events{0};
  std::atomic<std::size_t> buffered_at_end{1};
  AvInput input;
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvInputState::kEnded) {
      buffered_at_end.store(input.buffered_packets());
      ++ended_events;
    }
  });
  input.Open(SamplePath());
  REQUIRE(WaitFor([&] { return input.buffered_packets() > 0; }));
  std::this_thread::sleep_for(20ms);
  CHECK(input.status().state == AvInputState::kOpened);
  CHECK(ended_events.load() == 0);
  std::size_t count = 0;
  for (;;) {
    const auto result = input.ReadPacket();
    if (result.status == AvInputReadStatus::kEof) break;
    REQUIRE(result.status == AvInputReadStatus::kPacket);
    ++count;
  }
  CHECK(count > 0);
  CHECK(input.ReadPacket().status == AvInputReadStatus::kEof);
  CHECK(input.status().state == AvInputState::kEnded);
  input.Close();
  CHECK(ended_events.load() == 1);
  CHECK(buffered_at_end.load() == 0);
}

TEST_CASE("AvInput observer can request cancellation without deadlocking",
          "[av-input][notification][cancel]") {
  AvInput input;
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvInputState::kOpened) input.RequestCancel();
  });
  input.Open(SamplePath());
  CHECK(input.ReadPacket().status == AvInputReadStatus::kNull);
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvInputState::kCancelled; }));
}

#ifndef _WIN32

// The existing network fixtures require the old pipeline's ZLMediaKit. This
// single-client fixture keeps AvInput tests independent of that dependency.
class StallingHttpServer final {
 public:
  explicit StallingHttpServer(bool send_media = false) {
    if (send_media) {
      std::ifstream file(SamplePath("h264_aac.flv"), std::ios::binary);
      if (!file) throw std::runtime_error("Cannot open HTTP media fixture");
      media_.assign(std::istreambuf_iterator<char>(file), {});
    }
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
    if (!worker_.Start([this]() noexcept {
          try {
            Run();
          } catch (const std::exception& error) {
            MW_LOG_ERROR("streamer", "HTTP test server failed: {}",
                         error.what());
          } catch (...) {
            MW_LOG_ERROR("streamer", "HTTP test server failed: unknown error");
          }
        })) {
      close(listener_);
      throw std::runtime_error("Cannot start HTTP server thread");
    }
  }

  ~StallingHttpServer() {
    stop_.store(true);
    changed_.notify_all();
    worker_.Join();
    close(listener_);
  }

  const std::string& url() const { return url_; }
  bool WaitAccepted() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 2s, [this] { return accepted_; });
  }

 private:
  void Run() {
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
    if (!media_.empty()) {
      const auto response = fmt::format(
          "HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\n"
          "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
          media_.size() + 1024, media_);
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count = send(client, response.data() + sent,
                                response.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
      }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return stop_.load(); });
    shutdown(client, SHUT_RDWR);
    close(client);
  }

  int listener_ = -1;
  std::string media_;
  std::string url_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  bool accepted_ = false;
  mw::streamer::internal::Thread worker_;
};

TEST_CASE(
    "AvInput blocking reads wait through producer underflow until cancellation",
    "[av-input][network][cancel]") {
  StallingHttpServer server(true);
  AvInputConfig config;
  config.input_format_name = "flv";
  config.packet_capacity = 2;
  config.open_timeout = 5s;
  config.read_timeout = 5s;
  config.options["analyzeduration"] = "1";
  config.options["probesize"] = "2048";
  AvInput input(config);
  input.Open(server.url());
  std::atomic<std::size_t> packets{0};
  auto received = std::async(std::launch::async, [&] {
    for (;;) {
      auto result = input.ReadPacket();
      if (result.status != AvInputReadStatus::kPacket) return result;
      ++packets;
    }
  });
  const bool underflow = WaitFor(
      [&] { return packets.load() > 0 && input.buffered_packets() == 0; });
  const auto blocked = received.wait_for(40ms);
  const auto status = input.status();
  input.RequestCancel();
  const auto resumed = received.wait_for(1s);
  input.Close();
  CHECK(underflow);
  CHECK(blocked == std::future_status::timeout);
  CHECK(status.state == AvInputState::kOpened);
  CHECK(status.error == nullptr);
  REQUIRE(resumed == std::future_status::ready);
  CHECK(received.get().status == AvInputReadStatus::kNull);
}

TEST_CASE("AvInput interrupts stalled opening on timeout and explicit cancel",
          "[av-input][network][cancel][timeout]") {
  bool cancel = false;
  SECTION("open deadline") {}
  SECTION("cross-thread cancellation") { cancel = true; }
  StallingHttpServer server;
  AvInputConfig config;
  config.open_timeout = cancel ? 5s : 150ms;
  AvInput input(config);
  auto opened = std::async(std::launch::async, [&] {
    try {
      input.Open(server.url());
      return false;
    } catch (const std::exception&) {
      return true;
    }
  });
  REQUIRE(server.WaitAccepted());
  if (cancel) input.RequestCancel();
  REQUIRE(opened.wait_for(2s) == std::future_status::ready);
  CHECK(opened.get());
  if (cancel) {
    CHECK(input.status().state == AvInputState::kCancelled);
    CHECK(input.ReadPacket().status == AvInputReadStatus::kNull);
  } else {
    CHECK(input.status().state == AvInputState::kFailed);
    CHECK(ExceptionCode(input.status().error) == AVERROR(ETIMEDOUT));
  }
}

TEST_CASE("AvInput wakes blocked consumers on read timeout or cancellation",
          "[av-input][network][cancel][timeout]") {
  bool cancel = false;
  bool drain_failure = false;
  SECTION("read deadline") {}
  SECTION("cancel empty packet queue") { cancel = true; }
  SECTION("failure first drains cached packets") { drain_failure = true; }
  StallingHttpServer server(true);
  AvInputConfig config;
  config.input_format_name = "flv";
  config.packet_capacity = drain_failure ? 256 : 2;
  config.open_timeout = 2s;
  config.read_timeout = cancel ? 5s : 150ms;
  config.options["analyzeduration"] = "1";
  config.options["probesize"] = "2048";
  std::mutex error_mutex;
  std::exception_ptr observed_error;
  AvInput input(config);
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvInputState::kFailed) {
      std::lock_guard<std::mutex> lock(error_mutex);
      observed_error = status.error;
    }
  });
  input.Open(server.url());
  if (drain_failure) {
    REQUIRE(
        WaitFor([&] { return input.status().state == AvInputState::kFailed; }));
    REQUIRE(input.buffered_packets() > 0);
  }
  auto received = std::async(std::launch::async, [&] {
    std::size_t count = 0;
    for (;;) {
      auto result = input.ReadPacket();
      if (result.status != AvInputReadStatus::kPacket) {
        return std::make_pair(count, std::move(result));
      }
      ++count;
    }
  });
  if (cancel) {
    REQUIRE(WaitFor([&] { return input.buffered_packets() == 0; }));
    CHECK(received.wait_for(20ms) == std::future_status::timeout);
    const auto start = std::chrono::steady_clock::now();
    CHECK(input.TryReadPacket().status == AvInputReadStatus::kNull);
    CHECK(std::chrono::steady_clock::now() - start < 100ms);
    input.RequestCancel();
  }
  REQUIRE(received.wait_for(2s) == std::future_status::ready);
  auto [count, result] = received.get();
  CHECK(count > 0);
  CHECK(result.status == AvInputReadStatus::kNull);
  CHECK_FALSE(result.packet.has_value());
  if (cancel) {
    REQUIRE(WaitFor(
        [&] { return input.status().state == AvInputState::kCancelled; }));
    CHECK(input.status().error == nullptr);
  } else {
    CHECK(input.status().state == AvInputState::kFailed);
    CHECK(ExceptionCode(input.status().error) == AVERROR(ETIMEDOUT));
    input.Close();
    std::lock_guard<std::mutex> lock(error_mutex);
    CHECK(ExceptionCode(observed_error) == AVERROR(ETIMEDOUT));
  }
}

#endif

}  // namespace
