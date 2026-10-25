#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "mw/streamer/remuxer/remuxer.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "Poller/EventPoller.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::Remuxer;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    context_.reset(mw::streamer::Init(config));
  }

 private:
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      context_{nullptr, &mw::streamer::Shutdown};
};

class Gate final {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 2s, [&] { return entered_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool released_ = false;
};

struct ReleaseGate {
  Gate& gate;
  ~ReleaseGate() { gate.Release(); }
};

struct CloseFormat {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, CloseFormat> Open(const std::string& path) {
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0);
  std::unique_ptr<AVFormatContext, CloseFormat> input(raw);
  REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
  return input;
}

struct Media {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
  Media() {
    auto input =
        Open(std::string(MW_STREAMER_TEST_DATA_DIR) + "/h264_video.mp4");
    const auto* source = input->streams[0];
    ffmpeg::StreamInfo stream;
    stream.stream_index = 0;
    stream.time_base = source->time_base;
    stream.codec_parameters = ffmpeg::CodecParameters(*source->codecpar);
    streams.push_back(stream);
    ffmpeg::Packet packet;
    while (av_read_frame(input.get(), packet.get()) >= 0) {
      packet->time_base = stream.time_base;
      packets.push_back(packet.Ref());
      packet.Unref();
    }
    REQUIRE(packets.size() >= 2);
  }
};

class Directory final {
 public:
  Directory()
      : root_(fs::absolute(fs::temp_directory_path()).lexically_normal()),
        path_(root_ / ("mw-remux-queue-" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()))) {
    fs::create_directories(path_);
  }
  ~Directory() {
    if (path_.parent_path() == root_) {
      std::error_code error;
      fs::remove_all(path_, error);
    }
  }
  std::string File(const std::string& name) const {
    return (path_ / name).generic_u8string();
  }

 private:
  fs::path root_;
  fs::path path_;
};

void CheckRecording(const std::string& path, std::size_t expected) {
  auto input = Open(path);
  REQUIRE(input->nb_streams == 1);
  const auto* codec =
      avcodec_find_decoder(input->streams[0]->codecpar->codec_id);
  REQUIRE(codec);
  ffmpeg::CodecContext decoder(codec);
  REQUIRE(avcodec_parameters_to_context(decoder.get(),
                                        input->streams[0]->codecpar) >= 0);
  REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  std::size_t packets = 0;
  std::size_t frames = 0;
  const auto receive = [&] {
    ffmpeg::Frame frame;
    int result;
    while ((result = avcodec_receive_frame(decoder.get(), frame.get())) >= 0)
      ++frames;
    CHECK((result == AVERROR(EAGAIN) || result == AVERROR_EOF));
  };
  ffmpeg::Packet packet;
  while (av_read_frame(input.get(), packet.get()) >= 0) {
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    receive();
    ++packets;
    packet.Unref();
  }
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  receive();
  CHECK(packets == expected);
  CHECK(frames == expected);
}

TEST_CASE(
    "Remuxer capacity-one handoff resumes or closes without losing accepted "
    "packets",
    "[remuxer][queue][backpressure][recording]") {
  enum class Finish { kResume, kDrain, kStop };
  Finish finish = Finish::kResume;
  SECTION("poller consumption resumes blocked submission") {}
  SECTION("Drain closes blocked submission and preserves accepted packet") {
    finish = Finish::kDrain;
  }
  SECTION("Stop closes blocked submission and waits for accepted packet") {
    finish = Finish::kStop;
  }
  Runtime runtime;
  Media media;
  Directory directory;
  Gate gate;
  Remuxer remuxer;
  const auto first = remuxer.AddPushUrl(directory.File("first.mp4"));
  REQUIRE(remuxer.Start(media.streams));
  ReleaseGate release{gate};
  toolkit::EventPollerPool::Instance().getPoller()->async(
      [&] { gate.Enter(); });
  REQUIRE(gate.Wait());
  REQUIRE(remuxer.SubmitPacket(media.packets[0]));
  auto submitted = std::async(std::launch::async, [&] {
    return remuxer.SubmitPacket(media.packets[1]);
  });
  ReleaseGate release_submit{gate};
  const auto blocked = submitted.wait_for(40ms);
  std::future<void> stopped;
  if (finish == Finish::kDrain) remuxer.Drain();
  if (finish == Finish::kStop)
    stopped = std::async(std::launch::async, [&] { remuxer.Stop(); });
  ReleaseGate release_stop{gate};
  auto cancelled = std::future_status::timeout;
  auto stopping = std::future_status::timeout;
  if (finish != Finish::kResume) cancelled = submitted.wait_for(1s);
  if (stopped.valid()) stopping = stopped.wait_for(40ms);
  gate.Release();
  const auto completed = submitted.wait_for(2s);
  if (stopped.valid())
    stopped.get();
  else
    remuxer.Stop();
  CHECK(blocked == std::future_status::timeout);
  REQUIRE(completed == std::future_status::ready);
  CHECK(submitted.get() == (finish == Finish::kResume));
  if (finish != Finish::kResume) CHECK(cancelled == std::future_status::ready);
  if (finish == Finish::kStop) CHECK(stopping == std::future_status::timeout);
  CHECK_FALSE(remuxer.error());
  const std::size_t accepted = finish == Finish::kResume ? 2 : 1;
  const auto recordings = remuxer.recording_statuses();
  REQUIRE(recordings.size() == 1);
  CHECK(recordings[0].status.written_packets == accepted);
  CHECK(recordings[0].status.dropped_packets == 0);
  CheckRecording(first, accepted);
  CHECK_FALSE(remuxer.SubmitPacket(media.packets[1]));
  const auto restarted = remuxer.AddPushUrl(directory.File("restarted.mp4"));
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Stop();
  CHECK_FALSE(remuxer.error());
  CheckRecording(restarted, media.packets.size());
}

TEST_CASE("Remuxer terminal error is queryable before its observer returns",
          "[remuxer][queue][error]") {
  Runtime runtime;
  Media media;
  Gate callback;
  Remuxer remuxer;
  std::exception_ptr reported;
  std::atomic<int> errors{0};
  remuxer.SetOnError([&](std::exception_ptr failure) {
    reported = failure;
    ++errors;
    callback.Enter();
  });
  REQUIRE(remuxer.Start(media.streams));
  ReleaseGate release_callback{callback};
  REQUIRE(remuxer.SubmitPacket(media.packets[0]));
  REQUIRE(remuxer.SubmitPacket(media.packets[1]));
  REQUIRE(remuxer.SubmitPacket(media.packets[0]));
  REQUIRE(callback.Wait());
  const auto failure_during_callback = remuxer.error();
  auto submitted = std::async(std::launch::async, [&] {
    return remuxer.SubmitPacket(media.packets[1]);
  });
  ReleaseGate release_submit{callback};
  const auto rejected = submitted.wait_for(250ms);
  callback.Release();
  remuxer.Stop();
  CHECK(rejected == std::future_status::ready);
  CHECK_FALSE(submitted.get());
  REQUIRE(reported);
  CHECK(failure_during_callback == reported);
  CHECK(remuxer.error() == reported);
  CHECK_THROWS_AS(std::rethrow_exception(reported), std::invalid_argument);
  CHECK(errors.load() == 1);
  REQUIRE(remuxer.Start(media.streams));
  CHECK_FALSE(remuxer.error());
  remuxer.Stop();
  CHECK_FALSE(remuxer.error());
}

TEST_CASE("Remuxer rejects submission on its own poller without deadlocking",
          "[remuxer][queue][poller]") {
  Runtime runtime;
  Media media;
  Remuxer remuxer;
  REQUIRE(remuxer.Start(media.streams));
  std::promise<bool> completion;
  auto completed = completion.get_future();
  toolkit::EventPollerPool::Instance().getPoller()->async([&] {
    try {
      remuxer.SubmitPacket(media.packets[0]);
      completion.set_value(false);
    } catch (const std::logic_error&) {
      completion.set_value(true);
    } catch (...) {
      completion.set_exception(std::current_exception());
    }
  });
  const auto ready = completed.wait_for(2s);
  remuxer.Stop();
  REQUIRE(ready == std::future_status::ready);
  CHECK(completed.get());
  CHECK_FALSE(remuxer.error());
}

}  // namespace
