#include "mw/streamer/scheduler/scheduler.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <fmt/format.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/input/av_media_source.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;

// A complete FLV body followed by a stalled HTTP read is an input error, not
// normal media EOF. Every connection serves identical stream descriptions.
class ReconnectingHttpServer final {
 public:
  ReconnectingHttpServer() {
    std::ifstream input(
        std::string(MW_SCHEDULER_TEST_DATA_DIR) + "/h264_aac.flv",
        std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read HTTP media fixture");
    body_.assign(std::istreambuf_iterator<char>(input), {});
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("Cannot create HTTP socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener_, 2) != 0) {
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
    worker_ = std::thread([this] { Run(); });
  }

  ~ReconnectingHttpServer() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    for (const int client : clients_) close(client);
    close(listener_);
  }

  const std::string& url() const { return url_; }
  int connections() const { return connections_.load(); }

 private:
  void Run() {
    while (!stop_.load()) {
      pollfd descriptor{listener_, POLLIN, 0};
      if (poll(&descriptor, 1, 10) <= 0) continue;
      const int client = accept(listener_, nullptr, nullptr);
      if (client < 0) return;
      clients_.push_back(client);
      std::string request;
      while (!stop_.load() && request.find("\r\n\r\n") == std::string::npos) {
        pollfd readable{client, POLLIN, 0};
        if (poll(&readable, 1, 10) <= 0) continue;
        std::array<char, 1024> buffer{};
        const auto count = recv(client, buffer.data(), buffer.size(), 0);
        if (count <= 0) return;
        request.append(buffer.data(), static_cast<std::size_t>(count));
      }
      const auto response = fmt::format(
          "HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\n"
          "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
          body_.size() + 1000000, body_);
      std::size_t sent = 0;
      while (sent < response.size() && !stop_.load()) {
        const auto count = send(client, response.data() + sent,
                                response.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return;
        sent += static_cast<std::size_t>(count);
      }
      ++connections_;
      // Keep accepted sockets open so only the input's read timeout ends an
      // attempt. Continue accepting the recovery connection meanwhile.
    }
  }

  int listener_ = -1;
  std::string url_;
  std::string body_;
  std::vector<int> clients_;
  std::atomic<bool> stop_{false};
  std::atomic<int> connections_{0};
  std::thread worker_;
};

TEST_CASE("Scheduler resets business history at an HTTP reconnect generation",
          "[scheduler][reconnect][generation]") {
  bool block_video_output = false;
  SECTION("both outputs accept frames") {}
  SECTION("video output rejects frames until business reset") {
    block_video_output = true;
  }
  ReconnectingHttpServer server;
  mw::streamer::AvMediaSourceConfig input;
  input.proxy.max_retries = 2;
  input.proxy.input.read_timeout = 300ms;
  input.proxy.input.open_timeout = 3s;
  input.proxy.input.options["analyzeduration"] = "100000";
  input.proxy.input.options["probesize"] = "4096";
  input.proxy.input.packet_capacity = 128;
  input.video_decoder.thread_count = 1;
  input.audio_decoder.thread_count = 1;
  input.video_decoder.packet_capacity = 4;
  input.audio_decoder.packet_capacity = 4;
  input.video_decoder.frame_capacity = 4;
  input.audio_decoder.frame_capacity = 4;
  if (block_video_output) {
    // Keep the input controller free to submit the reconnect even while the
    // first video output is held by Syncer. Audio drives the new generation.
    input.video_decoder.packet_capacity = 64;
    input.video_decoder.frame_capacity = 64;
  }
  auto source = std::make_shared<mw::streamer::AvMediaSource>(input);
  mw::streamer::SchedulerConfig config;
  config.syncer.encoder.width = 64;
  config.syncer.encoder.height = 64;
  config.syncer.encoder.video_pixel_format = AV_PIX_FMT_YUV420P;
  config.syncer.encoder.fps = AVRational{25, 1};
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_S16;
  config.syncer.encoder.audio_sample_rate = 32000;
  config.syncer.encoder.audio_channel_layout = AV_CHANNEL_LAYOUT_MONO;
  config.syncer.options.initial_timeout = 20ms;
  config.syncer.options.timeout = 20ms;
  config.syncer.options.freeze = 20ms;

  std::atomic<int> active{0};
  std::atomic<int> resets{0};
  std::atomic<int> video_after_reset{0};
  std::atomic<int> audio_after_reset{0};
  std::atomic<int> video_outputs_after_reset{0};
  std::atomic<int> audio_outputs_after_reset{0};
  std::atomic<int> rejected_video_outputs{0};
  std::atomic<bool> resetting{false};
  std::atomic<bool> overlap{false};
  std::atomic<bool> saw_retry{false};
  std::atomic<bool> backwards{false};
  std::array<std::optional<std::int64_t>, 2> last_pts;
  source->SetOnStateChanged([&](const auto& status) {
    if (status.state == mw::streamer::AvMediaSourceState::kWaitingRetry)
      saw_retry.store(true);
  });
  mw::streamer::Scheduler scheduler(
      ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCpu), source, config);
  auto process = [&](const ffmpeg::Frame& frame, bool video) {
    ++active;
    if (resetting.load()) overlap.store(true);
    if (resets.load() > 0) ++(video ? video_after_reset : audio_after_reset);
    std::this_thread::sleep_for(2ms);
    --active;
    return frame.Ref();
  };
  scheduler.media_filter().SetOnVideo(
      [&](const ffmpeg::Frame& frame) { return process(frame, true); });
  scheduler.media_filter().SetOnAudio(
      [&](const ffmpeg::Frame& frame) { return process(frame, false); });
  scheduler.media_filter().SetOnReset([&] {
    resetting.store(true);
    if (active.load() != 0) overlap.store(true);
    std::this_thread::sleep_for(10ms);
    ++resets;
    resetting.store(false);
  });
  auto output = [&](const ffmpeg::Frame& frame, std::size_t index) {
    if (block_video_output && index == 0 && resets.load() == 0) {
      ++rejected_video_outputs;
      return false;
    }
    if (last_pts[index] && frame->pts <= *last_pts[index])
      backwards.store(true);
    last_pts[index] = frame->pts;
    if (resets.load() > 0)
      ++(index == 0 ? video_outputs_after_reset : audio_outputs_after_reset);
    return true;
  };
  scheduler.SetOnVideoFrame(
      [&](const ffmpeg::Frame& frame) { return output(frame, 0); });
  scheduler.SetOnAudioFrame(
      [&](const ffmpeg::Frame& frame) { return output(frame, 1); });
  REQUIRE(source->Start(server.url()));
  REQUIRE(scheduler.Start());
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (std::chrono::steady_clock::now() < deadline && !scheduler.error() &&
         (video_after_reset.load() == 0 || audio_after_reset.load() == 0 ||
          video_outputs_after_reset.load() == 0 ||
          audio_outputs_after_reset.load() == 0)) {
    std::this_thread::sleep_for(1ms);
  }
  scheduler.Stop();
  source->Stop();
  CHECK(server.connections() >= 2);
  CHECK(saw_retry.load());
  CHECK(source->status().generation == 1);
  CHECK(resets.load() == 1);
  CHECK(video_after_reset.load() > 0);
  CHECK(audio_after_reset.load() > 0);
  CHECK(video_outputs_after_reset.load() > 0);
  CHECK(audio_outputs_after_reset.load() > 0);
  if (block_video_output) CHECK(rejected_video_outputs.load() > 0);
  CHECK_FALSE(overlap.load());
  CHECK_FALSE(backwards.load());
  CHECK_FALSE(scheduler.error());
  CHECK_FALSE(source->status().error);
}

}  // namespace

#endif  // !_WIN32
