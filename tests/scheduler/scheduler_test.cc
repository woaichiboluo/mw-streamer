#include "mw/streamer/scheduler/scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/input/av_media_source.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvMediaSource;
using mw::streamer::AvMediaSourceConfig;
using mw::streamer::AvMediaSourceState;
using mw::streamer::Scheduler;
using mw::streamer::SchedulerConfig;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string SamplePath() {
  return std::string(MW_SCHEDULER_TEST_DATA_DIR) + "/h264_aac.mp4";
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

std::shared_ptr<AvMediaSource> Source() {
  AvMediaSourceConfig config;
  config.proxy.max_retries = 0;
  config.proxy.input.packet_capacity = 2;
  config.video_decoder.thread_count = 1;
  config.audio_decoder.thread_count = 1;
  config.video_decoder.frame_capacity = 2;
  config.audio_decoder.frame_capacity = 2;
  return std::make_shared<AvMediaSource>(config);
}

SchedulerConfig Config() {
  SchedulerConfig config;
  config.syncer.encoder.width = 16;
  config.syncer.encoder.height = 16;
  config.syncer.encoder.video_pixel_format = AV_PIX_FMT_YUV420P;
  config.syncer.encoder.fps = AVRational{25, 1};
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_S16;
  config.syncer.encoder.audio_sample_rate = 32000;
  config.syncer.encoder.audio_channel_layout = AV_CHANNEL_LAYOUT_MONO;
  config.syncer.timeout = 20ms;
  config.syncer.freeze = 20ms;
  config.syncer.start_ts = 0us;
  return config;
}

class TemporaryAudio final {
 public:
  TemporaryAudio()
      : path_(
            std::filesystem::temp_directory_path() /
            ("mw-scheduler-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".wav")) {
    // A 100 ms, mono S16 PCM WAV at 44100 Hz. The fixture uses raw file writes
    // so the test does not depend on an installed ffmpeg command-line tool.
    const std::array<unsigned char, 44> header{
        'R', 'I', 'F',  'F',  0x98, 0x22, 0,    0,    'W',  'A', 'V',
        'E', 'f', 'm',  't',  ' ',  16,   0,    0,    0,    1,   0,
        1,   0,   0x44, 0xac, 0,    0,    0x88, 0x58, 1,    0,   2,
        0,   16,  0,    'd',  'a',  't',  'a',  0x74, 0x22, 0,   0};
    const std::array<char, 8820> silence{};
    std::ofstream file(path_, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    file.write(silence.data(), silence.size());
    file.close();
    REQUIRE(file.good());
  }

  ~TemporaryAudio() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

TEST_CASE("Scheduler processes audio while the video filter is blocked",
          "[scheduler]") {
  auto source = Source();
  std::mutex mutex;
  std::condition_variable changed;
  bool video_entered = false;
  bool release_video = false;
  std::atomic<bool> audio_during_video{false};
  std::atomic<int> delivered_video{0};
  std::atomic<int> delivered_audio{0};
  Scheduler scheduler(source, Config());
  scheduler.media_filter().SetOnVideo([&](const ffmpeg::Frame& frame) {
    std::unique_lock lock(mutex);
    video_entered = true;
    changed.notify_all();
    changed.wait_for(lock, 8s, [&] { return release_video; });
    return frame.Ref();
  });
  scheduler.media_filter().SetOnAudio([&](const ffmpeg::Frame& frame) {
    std::lock_guard lock(mutex);
    if (video_entered && !release_video) audio_during_video.store(true);
    return frame.Ref();
  });
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++delivered_video; });
  scheduler.SetOnAudioFrame([&](ffmpeg::Frame) { ++delivered_audio; });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  bool video_started;
  {
    std::unique_lock lock(mutex);
    video_started = changed.wait_for(lock, 2s, [&] { return video_entered; });
  }
  const bool processed_in_parallel =
      WaitFor([&] { return audio_during_video.load(); }, 2s);
  const bool output_continued = WaitFor(
      [&] {
        return delivered_video.load() >= 3 && delivered_audio.load() >= 3;
      },
      4s);
  {
    std::lock_guard lock(mutex);
    release_video = true;
  }
  changed.notify_all();
  scheduler.Stop();
  CHECK(video_started);
  CHECK(processed_in_parallel);
  CHECK(output_continued);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler paces file processing before business filtering",
          "[scheduler][realtime]") {
  auto source = Source();
  std::atomic<int> video_filtered{0};
  std::atomic<int> audio_filtered{0};
  Scheduler scheduler(source, Config());
  scheduler.media_filter().SetOnVideo([&](const ffmpeg::Frame& frame) {
    ++video_filtered;
    return frame.Ref();
  });
  scheduler.media_filter().SetOnAudio([&](const ffmpeg::Frame& frame) {
    ++audio_filtered;
    return frame.Ref();
  });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  const bool started = WaitFor(
      [&] { return video_filtered.load() > 0 && audio_filtered.load() > 0; });
  std::this_thread::sleep_for(200ms);
  const int early_video = video_filtered.load();
  const int early_audio = audio_filtered.load();
  scheduler.Stop();
  CHECK(started);
  // The file contains 20 video frames and 95 audio frames over two seconds.
  // Merely bounding the old downstream queue allowed the complete file to
  // pass through business filters immediately; pacing must happen before it.
  CHECK(early_video >= 1);
  CHECK(early_video <= 6);
  CHECK(early_audio >= 1);
  CHECK(early_audio <= 20);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler delivers standby video at the configured realtime FPS",
          "[scheduler][realtime]") {
  const int fps = GENERATE(25, 30);
  CAPTURE(fps);
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.fps = AVRational{fps, 1};
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::chrono::steady_clock::time_point> delivered;
  const auto wanted = static_cast<std::size_t>(fps * 4 / 5 + 1);
  Scheduler scheduler(source, config);
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) {
    std::lock_guard lock(mutex);
    delivered.push_back(std::chrono::steady_clock::now());
    changed.notify_all();
  });
  REQUIRE(scheduler.Start());
  bool enough_frames;
  {
    std::unique_lock lock(mutex);
    enough_frames =
        changed.wait_for(lock, 5s, [&] { return delivered.size() >= wanted; });
  }
  scheduler.Stop();
  REQUIRE(enough_frames);
  const auto expected = std::chrono::duration<double>(1.0 / fps);
  const auto elapsed = delivered[wanted - 1] - delivered.front();
  CHECK(elapsed >= expected * (wanted - 1) * 0.8);
  CHECK(elapsed <= expected * (wanted - 1) * 1.4);
  // Permit occasional OS scheduling jitter, but reject repeated catch-up
  // bursts whose timestamps alone would otherwise appear to have correct FPS.
  std::size_t short_intervals = 0;
  for (std::size_t i = 1; i < wanted; ++i) {
    if (delivered[i] - delivered[i - 1] < expected / 3) ++short_intervals;
  }
  CHECK(short_intervals <= 2);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler continues producing both tracks after normal source EOF",
          "[scheduler]") {
  auto source = Source();
  std::atomic<int> video_frames{0};
  std::atomic<int> audio_frames{0};
  std::atomic<bool> bad_audio_format{false};
  std::atomic<int> errors{0};
  Scheduler scheduler(source, Config());
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++video_frames; });
  scheduler.SetOnAudioFrame([&](ffmpeg::Frame frame) {
    if (frame->sample_rate != 32000 || frame->format != AV_SAMPLE_FMT_S16 ||
        frame->ch_layout.nb_channels != 1) {
      bad_audio_format.store(true);
    }
    ++audio_frames;
  });
  scheduler.SetOnError([&](std::exception_ptr) { ++errors; });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  REQUIRE(WaitFor(
      [&] { return source->status().state == AvMediaSourceState::kEnded; },
      8s));
  // Let the two-second sample and queued frames finish before checking that
  // the live output continues to fill the missing input with video/silence.
  std::this_thread::sleep_for(2500ms);
  const int video_after_eof = video_frames.load();
  const int audio_after_eof = audio_frames.load();
  CHECK(WaitFor([&] {
    return video_frames.load() > video_after_eof + 2 &&
           audio_frames.load() > audio_after_eof + 2;
  }));
  scheduler.Stop();
  CHECK(video_after_eof > 0);
  CHECK(audio_after_eof > 0);
  CHECK_FALSE(bad_audio_format.load());
  CHECK(errors.load() == 0);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler reports a filter failure once and can start a new session",
          "[scheduler]") {
  auto source = Source();
  std::atomic<int> errors{0};
  std::atomic<bool> expected_error{false};
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  std::atomic<int> frames{0};
  Scheduler scheduler(source, Config());
  scheduler.media_filter().SetOnStart([&] { ++starts; });
  scheduler.media_filter().SetOnStop([&] { ++stops; });
  scheduler.media_filter().SetOnVideo(
      [](const ffmpeg::Frame&) -> ffmpeg::Frame {
        throw std::runtime_error("scheduler filter failure");
      });
  scheduler.SetOnError([&](std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::runtime_error& exception) {
      expected_error.store(std::string(exception.what()) ==
                           "scheduler filter failure");
    } catch (...) {
    }
    ++errors;
    scheduler.RequestStop();
  });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  REQUIRE(WaitFor([&] { return errors.load() != 0; }));
  scheduler.Stop();
  CHECK(errors.load() == 1);
  CHECK(expected_error.load());
  CHECK(scheduler.error());
  CHECK(starts.load() == 1);
  CHECK(stops.load() == 1);
  source->Stop();
  REQUIRE(source->Start(SamplePath()));
  scheduler.media_filter().SetOnVideo({});
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++frames; });
  REQUIRE(scheduler.Start());
  CHECK_FALSE(scheduler.error());
  CHECK(WaitFor([&] { return frames.load() >= 3; }));
  scheduler.Stop();
  scheduler.Stop();
  CHECK(errors.load() == 1);
  CHECK(starts.load() == 2);
  CHECK(stops.load() == 2);
}

TEST_CASE(
    "Scheduler drains disabled input tracks to prevent source backpressure",
    "[scheduler]") {
  auto source = Source();
  auto config = Config();
  const bool video_only = GENERATE(true, false);
  if (video_only) {
    config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  } else {
    config.syncer.encoder.video_pixel_format = AV_PIX_FMT_NONE;
  }
  std::atomic<int> frames{0};
  std::atomic<int> disabled_frames{0};
  Scheduler scheduler(source, config);
  scheduler.SetOnVideoFrame(
      [&](ffmpeg::Frame) { ++(video_only ? frames : disabled_frames); });
  scheduler.SetOnAudioFrame(
      [&](ffmpeg::Frame) { ++(video_only ? disabled_frames : frames); });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  CHECK(WaitFor(
      [&] { return source->status().state == AvMediaSourceState::kEnded; },
      8s));
  CHECK(WaitFor([&] { return frames.load() > 0; }));
  scheduler.Stop();
  CHECK(disabled_frames.load() == 0);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler drains disabled audio independently of low video FPS",
          "[scheduler][realtime]") {
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.fps = AVRational{1, 1};
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  std::atomic<int> video_frames{0};
  std::atomic<int> audio_frames{0};
  std::atomic<int> audio_filtered{0};
  Scheduler scheduler(source, config);
  scheduler.media_filter().SetOnAudio([&](const ffmpeg::Frame& frame) {
    ++audio_filtered;
    return frame.Ref();
  });
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++video_frames; });
  scheduler.SetOnAudioFrame([&](ffmpeg::Frame) { ++audio_frames; });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  // This two-second fixture contains 95 audio packets. Draining a small batch
  // only once per one-second video tick leaves audio full, blocking the shared
  // source dispatcher and preventing video/EOF from advancing.
  const bool ended = WaitFor(
      [&] { return source->status().state == AvMediaSourceState::kEnded; }, 8s);
  scheduler.Stop();
  CHECK(ended);
  CHECK(video_frames.load() > 0);
  CHECK(audio_frames.load() == 0);
  CHECK(audio_filtered.load() == 0);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler paces converted live frames on output deadlines",
          "[scheduler][realtime]") {
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.syncer.timeout = 200ms;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::chrono::steady_clock::time_point> delivered;
  constexpr std::size_t kWanted = 21;
  Scheduler scheduler(source, config);
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) {
    std::lock_guard lock(mutex);
    delivered.push_back(std::chrono::steady_clock::now());
    changed.notify_all();
  });
  REQUIRE(source->Start(SamplePath()));
  const auto started_at = std::chrono::steady_clock::now();
  REQUIRE(scheduler.Start());
  bool enough_frames;
  {
    std::unique_lock lock(mutex);
    enough_frames =
        changed.wait_for(lock, 4s, [&] { return delivered.size() >= kWanted; });
  }
  scheduler.Stop();
  REQUIRE(enough_frames);
  CHECK(delivered.front() - started_at < 1s);
  // The fixture is 10 FPS: ForceFps must prepare repeated frames while the
  // driver still delivers them individually at the configured 25 FPS.
  const auto elapsed = delivered[kWanted - 1] - delivered.front();
  CHECK(elapsed >= 640ms);
  CHECK(elapsed <= 1120ms);
  std::size_t short_intervals = 0;
  for (std::size_t i = 1; i < kWanted; ++i) {
    if (delivered[i] - delivered[i - 1] < 13ms) ++short_intervals;
  }
  CHECK(short_intervals <= 2);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler fills missing tracks and drains opposite-only input",
          "[scheduler]") {
  const bool audio_input = GENERATE(true, false);
  const bool both_outputs = GENERATE(true, false);
  CAPTURE(audio_input, both_outputs);
  TemporaryAudio audio;
  auto source = Source();
  auto config = Config();
  if (!both_outputs) {
    if (audio_input) {
      config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
    } else {
      config.syncer.encoder.video_pixel_format = AV_PIX_FMT_NONE;
    }
  }
  std::atomic<int> video_frames{0};
  std::atomic<int> audio_frames{0};
  std::atomic<int> video_filtered{0};
  std::atomic<int> audio_filtered{0};
  Scheduler scheduler(source, config);
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++video_frames; });
  scheduler.SetOnAudioFrame([&](ffmpeg::Frame) { ++audio_frames; });
  scheduler.media_filter().SetOnVideo([&](const ffmpeg::Frame& frame) {
    ++video_filtered;
    return frame.Ref();
  });
  scheduler.media_filter().SetOnAudio([&](const ffmpeg::Frame& frame) {
    ++audio_filtered;
    return frame.Ref();
  });
  const std::string input_path =
      audio_input ? audio.path()
                  : std::string(MW_SCHEDULER_TEST_DATA_DIR) + "/h264_video.mp4";
  REQUIRE(source->Start(input_path));
  REQUIRE(scheduler.Start());
  CHECK(WaitFor(
      [&] { return source->status().state == AvMediaSourceState::kEnded; },
      8s));
  CHECK(WaitFor([&] {
    return (both_outputs || audio_input ? video_frames.load() >= 3 : true) &&
           (both_outputs || !audio_input ? audio_frames.load() >= 3 : true);
  }));
  scheduler.Stop();
  CHECK_FALSE(scheduler.error());
  CHECK((video_filtered.load() > 0) == (both_outputs && !audio_input));
  CHECK((audio_filtered.load() > 0) == (both_outputs && audio_input));
  if (!both_outputs) {
    CHECK((audio_input ? audio_frames.load() : video_frames.load()) == 0);
  }
}

TEST_CASE("Scheduler reports unrecoverable external source failure once",
          "[scheduler]") {
  auto source = Source();
  std::atomic<int> errors{0};
  Scheduler scheduler(source, Config());
  scheduler.SetOnError([&](std::exception_ptr) { ++errors; });
  REQUIRE(scheduler.Start());
  REQUIRE(source->Start(std::string(MW_SCHEDULER_TEST_DATA_DIR) +
                        "/missing_scheduler_input.mp4"));
  REQUIRE(WaitFor([&] { return errors.load() != 0; }));
  scheduler.Stop();
  CHECK(errors.load() == 1);
  CHECK(scheduler.error());
  CHECK(source->status().state == AvMediaSourceState::kFailed);
}

TEST_CASE("Scheduler owns a source reference without controlling its lifecycle",
          "[scheduler]") {
  auto source = Source();
  std::weak_ptr<AvMediaSource> retained = source;
  std::atomic<int> frames{0};
  {
    Scheduler scheduler(source, Config());
    source.reset();
    REQUIRE_FALSE(retained.expired());
    scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++frames; });
    REQUIRE(scheduler.Start());
    CHECK_FALSE(scheduler.Start());
    CHECK(WaitFor([&] { return frames.load() > 0; }));
    scheduler.RequestStop();
    scheduler.Stop();
    CHECK(retained.lock()->status().state == AvMediaSourceState::kStopped);
    CHECK_FALSE(scheduler.error());
    REQUIRE(scheduler.Start());
    scheduler.Stop();
  }
  CHECK(retained.expired());
}

TEST_CASE("Scheduler forwards an output callback failure once", "[scheduler]") {
  auto source = Source();
  std::atomic<int> errors{0};
  Scheduler scheduler(source, Config());
  scheduler.SetOnVideoFrame([](ffmpeg::Frame) {
    throw std::runtime_error("scheduler output failure");
  });
  scheduler.SetOnError([&](std::exception_ptr) { ++errors; });
  REQUIRE(scheduler.Start());
  REQUIRE(WaitFor([&] { return errors.load() != 0; }));
  scheduler.Stop();
  CHECK(errors.load() == 1);
  CHECK(scheduler.error());
}

TEST_CASE("Scheduler reports filter startup failure once and permits restart",
          "[scheduler]") {
  auto source = Source();
  std::atomic<int> errors{0};
  std::atomic<int> frames{0};
  Scheduler scheduler(source, Config());
  scheduler.media_filter().SetOnStart(
      [] { throw std::runtime_error("scheduler filter startup failure"); });
  scheduler.SetOnError([&](std::exception_ptr) { ++errors; });
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++frames; });
  CHECK_FALSE(scheduler.Start());
  CHECK(errors.load() == 1);
  REQUIRE(scheduler.error());
  try {
    std::rethrow_exception(scheduler.error());
  } catch (const std::runtime_error& error) {
    CHECK(std::string(error.what()) == "scheduler filter startup failure");
  }
  CHECK(frames.load() == 0);
  scheduler.Stop();
  CHECK(errors.load() == 1);
  scheduler.media_filter().SetOnStart({});
  REQUIRE(scheduler.Start());
  CHECK_FALSE(scheduler.error());
  CHECK(WaitFor([&] { return frames.load() >= 3; }));
  scheduler.Stop();
  CHECK(errors.load() == 1);
}

TEST_CASE("Scheduler preserves cancellation requested during filter startup",
          "[scheduler]") {
  auto source = Source();
  std::atomic<int> video_frames{0};
  std::atomic<int> audio_frames{0};
  std::atomic<int> errors{0};
  Scheduler scheduler(source, Config());
  scheduler.media_filter().SetOnStart([&] { scheduler.RequestStop(); });
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) { ++video_frames; });
  scheduler.SetOnAudioFrame([&](ffmpeg::Frame) { ++audio_frames; });
  scheduler.SetOnError([&](std::exception_ptr) { ++errors; });
  REQUIRE(scheduler.Start());
  // An accidentally restarted Syncer starts filling absent input after its
  // two-second clock hold. Observe beyond that point before joining it.
  CHECK_FALSE(WaitFor(
      [&] { return video_frames.load() != 0 || audio_frames.load() != 0; },
      2300ms));
  scheduler.Stop();
  CHECK(video_frames.load() == 0);
  CHECK(audio_frames.load() == 0);
  CHECK(errors.load() == 0);
  CHECK_FALSE(scheduler.error());
  scheduler.media_filter().SetOnStart({});
  REQUIRE(scheduler.Start());
  CHECK(WaitFor(
      [&] { return video_frames.load() >= 3 && audio_frames.load() >= 3; }));
  scheduler.Stop();
  CHECK(errors.load() == 0);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler rejects a null source or configurations without tracks",
          "[scheduler]") {
  CHECK_THROWS_AS(Scheduler(nullptr, Config()), std::invalid_argument);
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.video_pixel_format = AV_PIX_FMT_NONE;
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  CHECK_THROWS_AS(Scheduler(source, config), std::invalid_argument);
}

TEST_CASE(
    "Scheduler skips expired video deadlines after a slow output callback",
    "[scheduler]") {
  using Clock = std::chrono::steady_clock;
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  Scheduler scheduler(source, config);
  std::vector<Clock::time_point> arrivals;
  std::atomic<int> delivered{0};
  scheduler.SetOnVideoFrame([&](ffmpeg::Frame) {
    arrivals.push_back(Clock::now());
    if (arrivals.size() == 3) std::this_thread::sleep_for(120ms);
    delivered.store(static_cast<int>(arrivals.size()));
    if (arrivals.size() == 10) scheduler.RequestStop();
  });
  REQUIRE(scheduler.Start());
  const bool finished = WaitFor([&] { return delivered.load() >= 10; });
  scheduler.Stop();
  REQUIRE(finished);
  REQUIRE_FALSE(scheduler.error());
  REQUIRE(arrivals.size() == 10);
  CHECK(arrivals[3] - arrivals[2] >= 120ms);
  // The first callback after the stall may land near a clock boundary.
  // Subsequent callbacks must not burst through the missed 40 ms slots.
  for (std::size_t index = 4; index < arrivals.size(); ++index) {
    CHECK(arrivals[index] - arrivals[index - 1] >= 10ms);
  }
  const auto recovered_span = arrivals.back() - arrivals[3];
  CHECK(recovered_span >= 180ms);
  CHECK(recovered_span <= 480ms);
}

}  // namespace
