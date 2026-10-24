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
#include <libavformat/avformat.h>
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
  config.syncer.options.initial_timeout = 20ms;
  config.syncer.options.timeout = 20ms;
  config.syncer.options.freeze = 20ms;
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

class TemporaryVideoJump final {
 public:
  TemporaryVideoJump()
      : path_(
            std::filesystem::temp_directory_path() /
            ("mw-scheduler-jump-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".mp4")) {
    AVFormatContext* raw_input = nullptr;
    REQUIRE(avformat_open_input(&raw_input, SamplePath().c_str(), nullptr,
                                nullptr) >= 0);
    const auto close_input = [](AVFormatContext* context) {
      avformat_close_input(&context);
    };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw_input,
                                                                  close_input);
    REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
    const int video = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1,
                                          -1, nullptr, 0);
    REQUIRE(video >= 0);
    AVFormatContext* raw_output = nullptr;
    REQUIRE(avformat_alloc_output_context2(&raw_output, nullptr, nullptr,
                                           path().c_str()) >= 0);
    const auto close_output = [](AVFormatContext* context) {
      if (context->pb) avio_closep(&context->pb);
      avformat_free_context(context);
    };
    std::unique_ptr<AVFormatContext, decltype(close_output)> output(
        raw_output, close_output);
    auto* stream = avformat_new_stream(output.get(), nullptr);
    REQUIRE(stream != nullptr);
    const auto* original = input->streams[video];
    REQUIRE(avcodec_parameters_copy(stream->codecpar, original->codecpar) >= 0);
    stream->codecpar->codec_tag = 0;
    stream->time_base = original->time_base;
    stream->avg_frame_rate = original->avg_frame_rate;
    REQUIRE(avio_open(&output->pb, path().c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(output.get(), nullptr) >= 0);
    const auto release_packet = [](AVPacket* packet) {
      av_packet_free(&packet);
    };
    std::unique_ptr<AVPacket, decltype(release_packet)> packet(
        av_packet_alloc(), release_packet);
    REQUIRE(packet != nullptr);
    bool first = true;
    int result;
    while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
      if (packet->stream_index == video) {
        if (!first) {
          const auto jump =
              av_rescale_q(1, AVRational{1, 1}, original->time_base);
          if (packet->pts != AV_NOPTS_VALUE) packet->pts += jump;
          if (packet->dts != AV_NOPTS_VALUE) packet->dts += jump;
        }
        first = false;
        packet->stream_index = stream->index;
        packet->pos = -1;
        av_packet_rescale_ts(packet.get(), original->time_base,
                             stream->time_base);
        REQUIRE(av_interleaved_write_frame(output.get(), packet.get()) >= 0);
      }
      av_packet_unref(packet.get());
    }
    REQUIRE(result == AVERROR_EOF);
    REQUIRE_FALSE(first);
    REQUIRE(av_write_trailer(output.get()) >= 0);
  }

  ~TemporaryVideoJump() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  std::string path() const { return path_.generic_u8string(); }

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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++delivered_video;
    return true;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++delivered_audio;
    return true;
  });
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

TEST_CASE("Scheduler standby output follows the Syncer shared clock",
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
  std::vector<std::int64_t> pts;
  const auto wanted = static_cast<std::size_t>(fps * 4 / 5 + 1);
  Scheduler scheduler(source, config);
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame& frame) {
    std::lock_guard lock(mutex);
    delivered.push_back(std::chrono::steady_clock::now());
    pts.push_back(frame->pts);
    changed.notify_all();
    return true;
  });
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(scheduler.Start());
  bool enough_frames;
  {
    std::unique_lock lock(mutex);
    enough_frames =
        changed.wait_for(lock, 5s, [&] { return delivered.size() >= wanted; });
  }
  scheduler.Stop();
  REQUIRE(enough_frames);
  // Sentinel holds its shared clock for two seconds, then fills in batches
  // of at most five per retry. The FPS specifies media PTS, not a second cap.
  CHECK(delivered.front() - started >= 2s);
  for (std::size_t index = 1; index < wanted; ++index) {
    CHECK(pts[index] - pts[index - 1] == 1);
  }
  for (std::size_t index = 5; index < wanted; ++index) {
    CHECK(delivered[index] - delivered[index - 5] >= 10ms);
  }
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++video_frames;
    return true;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame& frame) {
    if (frame->sample_rate != 32000 || frame->format != AV_SAMPLE_FMT_S16 ||
        frame->ch_layout.nb_channels != 1) {
      bad_audio_format.store(true);
    }
    ++audio_frames;
    return true;
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

TEST_CASE(
    "Scheduler adapts audio format and block size before business filtering",
    "[scheduler][audio][frame-size]") {
  auto source = Source();
  auto config = Config();
  config.output_samples_per_frame = GENERATE(256, 1024);
  std::atomic<int> filtered{0};
  std::atomic<bool> invalid{false};
  Scheduler scheduler(source, config);
  scheduler.media_filter().SetOnAudio([&](const ffmpeg::Frame& frame) {
    if (frame->sample_rate != 32000 || frame->format != AV_SAMPLE_FMT_S16 ||
        frame->ch_layout.nb_channels != 1 ||
        frame->nb_samples != config.output_samples_per_frame ||
        frame->time_base.num != 1 || frame->time_base.den != 32000) {
      invalid.store(true);
    }
    ++filtered;
    return frame.Ref();
  });
  scheduler.SetOnVideoFrame([](const ffmpeg::Frame&) { return true; });
  scheduler.SetOnAudioFrame([](const ffmpeg::Frame&) { return true; });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  const bool received = WaitFor([&] { return filtered.load() >= 8; });
  scheduler.Stop();
  CHECK(received);
  CHECK_FALSE(invalid.load());
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++frames;
    return true;
  });
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++(video_only ? frames : disabled_frames);
    return true;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++(video_only ? disabled_frames : frames);
    return true;
  });
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++video_frames;
    return true;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++audio_frames;
    return true;
  });
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

TEST_CASE("Scheduler releases converted frames in input-paced batches",
          "[scheduler][realtime]") {
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.syncer.options.timeout = 200ms;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::chrono::steady_clock::time_point> delivered;
  constexpr std::size_t kWanted = 21;
  Scheduler scheduler(source, config);
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    std::lock_guard lock(mutex);
    delivered.push_back(std::chrono::steady_clock::now());
    changed.notify_all();
    return true;
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
  // Realtime paces this 10 FPS input; ForceFps produces 25 FPS timestamps.
  // Converted repeats may be delivered together, without another output cap.
  const auto elapsed = delivered[kWanted - 1] - delivered.front();
  CHECK(elapsed >= 500ms);
  CHECK(elapsed <= 1400ms);
  std::size_t short_intervals = 0;
  for (std::size_t i = 1; i < kWanted; ++i) {
    if (delivered[i] - delivered[i - 1] < 13ms) ++short_intervals;
  }
  CHECK(short_intervals > 0);
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++video_frames;
    return true;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++audio_frames;
    return true;
  });
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
    scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
      ++frames;
      return true;
    });
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
  scheduler.SetOnVideoFrame([](const ffmpeg::Frame&) -> bool {
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++frames;
    return true;
  });
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
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++video_frames;
    return true;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++audio_frames;
    return true;
  });
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

TEST_CASE("Scheduler preserves sentinel forward-jump batches and their pause",
          "[scheduler][realtime]") {
  using Clock = std::chrono::steady_clock;
  TemporaryVideoJump video;
  auto source = Source();
  auto config = Config();
  config.syncer.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.syncer.options.timeout = 5s;
  config.realtime.discontinuity_threshold = 2s;
  Scheduler scheduler(source, config);
  std::mutex mutex;
  std::vector<Clock::time_point> arrivals;
  std::vector<std::int64_t> pts;
  std::atomic<int> delivered{0};
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame& frame) {
    std::lock_guard lock(mutex);
    arrivals.push_back(Clock::now());
    pts.push_back(frame->pts);
    delivered.store(static_cast<int>(arrivals.size()));
    return true;
  });
  REQUIRE(source->Start(video.path()));
  REQUIRE(scheduler.Start());
  const bool finished = WaitFor([&] { return delivered.load() >= 7; });
  scheduler.Stop();
  REQUIRE(finished);
  REQUIRE_FALSE(scheduler.error());
  REQUIRE(arrivals.size() >= 7);
  // A one-second jump after the first real frame produces five 40 ms
  // replacements immediately, then waits half their duration before real input.
  CHECK(arrivals[5] - arrivals[1] < 80ms);
  CHECK(arrivals[6] - arrivals[5] >= 80ms);
  for (std::size_t index = 1; index < 7; ++index) {
    CHECK(pts[index] > pts[index - 1]);
  }
}

TEST_CASE("Scheduler waits for video output space while audio continues",
          "[scheduler][backpressure]") {
  auto source = Source();
  Scheduler scheduler(source, Config());
  std::atomic<int> video_calls{0};
  std::atomic<int> audio_calls{0};
  std::atomic<int> accepted_frames{0};
  std::atomic<bool> accept_video{false};
  std::mutex mutex;
  std::vector<std::int64_t> video_pts;
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame& frame) {
    {
      std::lock_guard lock(mutex);
      video_pts.push_back(frame->pts);
    }
    ++video_calls;
    const bool accepted = accept_video.load();
    if (accepted) ++accepted_frames;
    return accepted;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++audio_calls;
    return true;
  });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  const bool blocked = WaitFor([&] { return video_calls.load() >= 1; });
  const int audio_before = audio_calls.load();
  const bool audio_continued =
      WaitFor([&] { return audio_calls.load() >= audio_before + 4; });
  scheduler.NotifyOutputSpace(false);
  std::this_thread::sleep_for(100ms);
  const int calls_while_full = video_calls.load();
  accept_video.store(true);
  scheduler.NotifyOutputSpace(true);
  const bool resumed = WaitFor([&] { return accepted_frames.load() > 0; });
  scheduler.Stop();
  CHECK(blocked);
  CHECK(audio_continued);
  CHECK(calls_while_full >= 2);
  REQUIRE(resumed);
  REQUIRE(video_pts.size() >= static_cast<std::size_t>(calls_while_full));
  for (int attempt = 1; attempt < calls_while_full; ++attempt) {
    CHECK(video_pts[static_cast<std::size_t>(attempt)] == video_pts[0]);
  }
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler preserves output space notification before rejection",
          "[scheduler][backpressure]") {
  using Clock = std::chrono::steady_clock;
  auto source = Source();
  Scheduler scheduler(source, Config());
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Clock::time_point> attempts;
  std::vector<std::int64_t> pts;
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame& frame) {
    std::lock_guard lock(mutex);
    attempts.push_back(Clock::now());
    pts.push_back(frame->pts);
    changed.notify_all();
    if (attempts.size() <= 8) {
      scheduler.NotifyOutputSpace(true);
      return false;
    }
    return true;
  });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  bool retried;
  {
    std::unique_lock lock(mutex);
    retried = changed.wait_for(lock, 1s, [&] { return attempts.size() >= 9; });
  }
  scheduler.Stop();
  REQUIRE(retried);
  REQUIRE(attempts.size() >= 9);
  CHECK(attempts[8] - attempts[0] < 100ms);
  for (std::size_t attempt = 1; attempt < 9; ++attempt) {
    CHECK(pts[attempt] == pts[0]);
  }
  CHECK_FALSE(scheduler.error());
}

TEST_CASE(
    "Scheduler retries rejected output periodically without notifications",
    "[scheduler][backpressure]") {
  using Clock = std::chrono::steady_clock;
  const bool video = GENERATE(false, true);
  auto source = Source();
  Scheduler scheduler(source, Config());
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Clock::time_point> attempts;
  std::vector<std::int64_t> pts;
  const auto output = [&](const ffmpeg::Frame& frame) {
    std::lock_guard lock(mutex);
    attempts.push_back(Clock::now());
    pts.push_back(frame->pts);
    changed.notify_all();
    return attempts.size() >= 3;
  };
  if (video) {
    scheduler.SetOnVideoFrame(output);
  } else {
    scheduler.SetOnAudioFrame(output);
  }
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  bool retried;
  {
    std::unique_lock lock(mutex);
    retried = changed.wait_for(lock, 1s, [&] { return attempts.size() >= 3; });
  }
  scheduler.Stop();
  REQUIRE(retried);
  REQUIRE(attempts.size() >= 3);
  CHECK(pts[1] == pts[0]);
  CHECK(pts[2] == pts[0]);
  for (std::size_t index = 1; index < 3; ++index) {
    CAPTURE(index, video);
    CHECK(attempts[index] - attempts[index - 1] >= 18ms);
    CHECK(attempts[index] - attempts[index - 1] < 250ms);
  }
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler stops promptly while both outputs await space",
          "[scheduler][backpressure]") {
  auto source = Source();
  Scheduler scheduler(source, Config());
  std::atomic<int> video_attempts{0};
  std::atomic<int> audio_attempts{0};
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++video_attempts;
    return false;
  });
  scheduler.SetOnAudioFrame([&](const ffmpeg::Frame&) {
    ++audio_attempts;
    return false;
  });
  REQUIRE(source->Start(SamplePath()));
  REQUIRE(scheduler.Start());
  const bool blocked = WaitFor(
      [&] { return video_attempts.load() >= 1 && audio_attempts.load() >= 1; });
  const auto before_stop = std::chrono::steady_clock::now();
  scheduler.Stop();
  CHECK(std::chrono::steady_clock::now() - before_stop < 500ms);
  CHECK(blocked);
  CHECK(video_attempts.load() >= 1);
  CHECK(audio_attempts.load() >= 1);
  CHECK_FALSE(scheduler.error());
}

TEST_CASE("Scheduler wakes for source frames before a long Syncer timeout",
          "[scheduler][realtime]") {
  auto source = Source();
  auto config = Config();
  config.syncer.options.timeout = 30s;
  Scheduler scheduler(source, config);
  std::atomic<int> frames{0};
  scheduler.SetOnVideoFrame([&](const ffmpeg::Frame&) {
    ++frames;
    return true;
  });
  REQUIRE(scheduler.Start());
  // Let the driver observe empty queues and enter its event wait first.
  std::this_thread::sleep_for(100ms);
  CHECK(frames.load() == 0);
  REQUIRE(source->Start(SamplePath()));
  const bool awakened = WaitFor([&] { return frames.load() > 0; }, 1s);
  scheduler.Stop();
  CHECK(awakened);
  CHECK_FALSE(scheduler.error());
}

}  // namespace
