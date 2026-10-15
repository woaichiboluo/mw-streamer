#include "mw/streamer/timing/syncer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "streamer/timing/syncer_timing.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::Syncer;
using mw::streamer::SyncerConfig;
using mw::streamer::internal::SyncerTiming;
namespace ffmpeg = mw::streamer::ffmpeg;

constexpr std::int64_t kStart = 10000000;

ffmpeg::Frame Video(std::uint8_t value) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = 8;
  frame->height = 8;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate test video");
  }
  for (int row = 0; row < frame->height; ++row) {
    std::memset(frame->data[0] + row * frame->linesize[0], value,
                static_cast<std::size_t>(frame->width));
  }
  return frame;
}

ffmpeg::Frame RgbBackup() {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_RGB24;
  frame->width = 4;
  frame->height = 4;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate test RGB backup");
  }
  for (int row = 0; row < frame->height; ++row) {
    std::memset(frame->data[0] + row * frame->linesize[0], 23, 12);
  }
  return frame;
}

SyncerConfig VideoConfig() {
  SyncerConfig config;
  config.encoder.video_pixel_format = AV_PIX_FMT_GRAY8;
  config.encoder.width = 8;
  config.encoder.height = 8;
  config.encoder.fps = AVRational{25, 1};
  config.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.backup_video = Video(7);
  return config;
}

SyncerConfig AudioConfig(int rate = 48000,
                         AVSampleFormat format = AV_SAMPLE_FMT_S16) {
  SyncerConfig config;
  config.encoder.video_pixel_format = AV_PIX_FMT_NONE;
  config.encoder.audio_sample_format = format;
  config.encoder.audio_sample_rate = rate;
  config.encoder.audio_channel_layout = AV_CHANNEL_LAYOUT_MONO;
  return config;
}

SyncerConfig MixedConfig() {
  auto config = AudioConfig();
  config.encoder.video_pixel_format = AV_PIX_FMT_GRAY8;
  config.encoder.width = 8;
  config.encoder.height = 8;
  config.encoder.fps = AVRational{25, 1};
  config.backup_video = Video(7);
  return config;
}

AvDecoderReadResult VideoInput(std::int64_t pts, std::uint64_t generation = 0) {
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kFrame;
  input.frame = Video(99);
  (*input.frame)->pts = pts;
  (*input.frame)->duration = 40000;
  (*input.frame)->time_base = AVRational{1, 1000000};
  input.time_base = AVRational{1, 1000000};
  input.generation = generation;
  return input;
}

AvDecoderReadResult AudioInput(std::int64_t pts, int rate = 48000,
                               int samples = 960,
                               std::uint64_t generation = 0) {
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kFrame;
  input.frame.emplace();
  auto& frame = *input.frame;
  frame->format = AV_SAMPLE_FMT_S16;
  frame->sample_rate = rate;
  frame->nb_samples = samples;
  av_channel_layout_default(&frame->ch_layout, 1);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate test audio");
  }
  std::memset(frame->data[0], 0x31, static_cast<std::size_t>(samples) * 2);
  frame->pts = pts;
  frame->duration = samples;
  frame->time_base = AVRational{1, rate};
  input.time_base = frame->time_base;
  input.generation = generation;
  input.stream_index = 1;
  return input;
}

AvDecoderReadResult Eof(std::uint64_t generation = 0) {
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kEof;
  input.generation = generation;
  return input;
}

std::int64_t PtsUs(const ffmpeg::Frame& frame) {
  return av_rescale_q(frame->pts, frame->time_base, AVRational{1, 1000000});
}

std::vector<ffmpeg::Frame> DrainAt(SyncerTiming& timing, bool video,
                                   std::int64_t now) {
  std::vector<ffmpeg::Frame> output;
  for (int i = 0; i < 1024; ++i) {
    auto result = timing.Step(video, nullptr, now);
    if (!result.frame) {
      return output;
    }
    output.push_back(std::move(*result.frame));
  }
  FAIL("Syncer emitted an unbounded batch of backup frames");
  return output;
}

TEST_CASE("Syncer generates a black CPU backup from the video target",
          "[syncer][timing]") {
  const auto format =
      GENERATE(AV_PIX_FMT_RGB24, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12);
  auto config = VideoConfig();
  config.backup_video.reset();
  config.encoder.video_pixel_format = format;
  SyncerTiming timing(config);
  REQUIRE_FALSE(timing.Step(true, nullptr, 0).frame);
  REQUIRE_FALSE(timing.Step(true, nullptr, 1000000).frame);
  auto first = timing.Step(true, nullptr, 3000000);
  REQUIRE(first.frame);
  const auto& frame = *first.frame;
  REQUIRE(frame->format == format);
  REQUIRE(frame->width == 8);
  REQUIRE(frame->height == 8);
  REQUIRE(frame->hw_frames_ctx == nullptr);
  REQUIRE(PtsUs(frame) == kStart);
  REQUIRE(av_rescale_q(frame->duration, frame->time_base,
                       AVRational{1, 1000000}) == 40000);
  const int size = av_image_get_buffer_size(format, 8, 8, 1);
  REQUIRE(size > 0);
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
  REQUIRE(av_image_copy_to_buffer(pixels.data(), size, frame->data,
                                  frame->linesize, format, 8, 8, 1) == size);
  for (int index = 0; index < size; ++index) {
    const int black = format == AV_PIX_FMT_RGB24 ? 0 : index < 64 ? 16 : 128;
    REQUIRE(pixels[static_cast<std::size_t>(index)] == black);
  }
  auto second = timing.Step(true, nullptr, 3040000);
  REQUIRE(second.frame);
  REQUIRE(PtsUs(*second.frame) == kStart + 40000);
}

TEST_CASE("Syncer retains a custom audio layout independently of its caller",
          "[syncer][timing]") {
  auto config = AudioConfig();
  auto& layout = config.encoder.audio_channel_layout;
  REQUIRE(av_channel_layout_custom_init(&layout, 2) == 0);
  layout.u.map[0].id = AV_CHAN_FRONT_LEFT;
  layout.u.map[1].id = AV_CHAN_FRONT_RIGHT;
  SyncerTiming timing(config);
  av_channel_layout_uninit(&layout);
  REQUIRE_FALSE(timing.Step(false, nullptr, 0).frame);
  REQUIRE_FALSE(timing.Step(false, nullptr, 1000000).frame);
  auto output = timing.Step(false, nullptr, 3000000);
  REQUIRE(output.frame);
  const auto& actual = (*output.frame)->ch_layout;
  REQUIRE(actual.nb_channels == 2);
  REQUIRE(av_channel_layout_channel_from_index(&actual, 0) ==
          AV_CHAN_FRONT_LEFT);
  REQUIRE(av_channel_layout_channel_from_index(&actual, 1) ==
          AV_CHAN_FRONT_RIGHT);
}

TEST_CASE("Syncer starts backup after input timeout and media clock hold",
          "[syncer][timing]") {
  // With no input, the first timeout initializes the shared media clock.
  // Use independent clocks so a pre-boundary poll does not postpone the next
  // poll by 22 ms and hide the exact two-second clock boundary being tested.
  SyncerTiming before(VideoConfig());
  REQUIRE_FALSE(before.Step(true, nullptr, 0).frame);
  REQUIRE_FALSE(before.Step(true, nullptr, 1000000).frame);
  REQUIRE_FALSE(before.Step(true, nullptr, 2999999).frame);
  SyncerTiming timing(VideoConfig());
  REQUIRE_FALSE(timing.Step(true, nullptr, 0).frame);
  REQUIRE_FALSE(timing.Step(true, nullptr, 1000000).frame);
  const auto output = timing.Step(true, nullptr, 3000000);
  REQUIRE(output.frame);
  REQUIRE(PtsUs(*output.frame) == kStart);
  REQUIRE((*output.frame)->data[0][0] == 7);
  REQUIRE_FALSE(output.consumed);
}

TEST_CASE(
    "Syncer holds the real media clock then freezes and switches to backup",
    "[syncer][timing]") {
  SyncerTiming timing(VideoConfig());
  auto first = VideoInput(50000000);
  auto anchor = timing.Step(true, &first, 0);
  REQUIRE(anchor.consumed);
  REQUIRE(anchor.frame);
  REQUIRE(PtsUs(*anchor.frame) == kStart);
  REQUIRE_FALSE(timing.Step(true, nullptr, 1978000).frame);
  auto frozen = DrainAt(timing, true, 2000000);
  REQUIRE_FALSE(frozen.empty());
  REQUIRE(PtsUs(frozen.front()) == kStart + 40000);
  for (const auto& frame : frozen) {
    REQUIRE(frame->data[0][0] == 99);
  }
  std::vector<ffmpeg::Frame> after_freeze;
  for (std::int64_t now = 2022000; now <= 5200000; now += 22000) {
    auto batch = DrainAt(timing, true, now);
    for (auto& frame : batch) {
      after_freeze.push_back(std::move(frame));
    }
  }
  REQUIRE_FALSE(after_freeze.empty());
  bool saw_backup = false;
  for (const auto& frame : after_freeze) {
    if (PtsUs(frame) < kStart + 5000000) {
      REQUIRE(frame->data[0][0] == 99);
    } else {
      REQUIRE(frame->data[0][0] == 7);
      saw_backup = true;
    }
  }
  REQUIRE(saw_backup);
}

TEST_CASE("Syncer audio backups contain actual silence including unsigned PCM",
          "[syncer][timing]") {
  const auto format =
      GENERATE(AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_U8, AV_SAMPLE_FMT_FLTP);
  SyncerTiming timing(AudioConfig(48000, format));
  REQUIRE_FALSE(timing.Step(false, nullptr, 0).frame);
  REQUIRE_FALSE(timing.Step(false, nullptr, 1000000).frame);
  auto output = timing.Step(false, nullptr, 3000000);
  REQUIRE(output.frame);
  const auto& frame = *output.frame;
  REQUIRE(frame->format == format);
  REQUIRE(frame->sample_rate == 48000);
  REQUIRE(frame->ch_layout.nb_channels == 1);
  REQUIRE(frame->nb_samples > 0);
  REQUIRE(frame->nb_samples <= 1024);
  REQUIRE(frame->time_base.num == 1);
  REQUIRE(frame->time_base.den == 48000);
  REQUIRE(frame->pts == 480000);
  const int bytes = frame->nb_samples * av_get_bytes_per_sample(format);
  const int zero = format == AV_SAMPLE_FMT_U8 ? 128 : 0;
  for (int i = 0; i < bytes; ++i) {
    REQUIRE(frame->extended_data[0][i] == zero);
  }
}

TEST_CASE("Syncer preserves a common initial video audio offset when enabled",
          "[syncer][timing]") {
  auto config = MixedConfig();
  config.forward_start_shift = true;
  SyncerTiming timing(config);
  auto video = VideoInput(50000000);
  auto audio = AudioInput(2403840);
  auto first = timing.Step(true, &video, 0);
  REQUIRE(first.frame);
  REQUIRE(PtsUs(*first.frame) == kStart);
  REQUIRE_FALSE(timing.Step(false, &audio, 79999).frame);
  auto second = timing.Step(false, &audio, 80000);
  REQUIRE(second.consumed);
  REQUIRE(second.frame);
  REQUIRE(PtsUs(*second.frame) == kStart + 80000);
}

TEST_CASE("Syncer default fills an initial audio offset before its real frame",
          "[syncer][timing]") {
  auto config = MixedConfig();
  SyncerTiming timing(config);
  auto video = VideoInput(50000000);
  auto audio = AudioInput(2403840);
  REQUIRE(timing.Step(true, &video, 0).frame);
  auto output = timing.Step(false, &audio, 80000);
  REQUIRE(output.frame);
  REQUIRE_FALSE(output.consumed);
  REQUIRE(PtsUs(*output.frame) == kStart);
  REQUIRE((*output.frame)->data[0][0] == 0);
  bool consumed = false;
  for (int i = 0; i < 10; ++i) {
    output = timing.Step(false, &audio, 80000 + i * 1000);
    if (output.consumed) {
      consumed = true;
      REQUIRE(output.frame);
      REQUIRE(PtsUs(*output.frame) == kStart + 80000);
      REQUIRE((*output.frame)->data[0][0] == 0x31);
      break;
    }
  }
  REQUIRE(consumed);
}

TEST_CASE(
    "Syncer retains sub-threshold PTS jitter rather than recounting frames",
    "[syncer][timing]") {
  SyncerTiming timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto next = VideoInput(50040500);
  REQUIRE_FALSE(timing.Step(true, &next, 40499).frame);
  auto output = timing.Step(true, &next, 40500);
  REQUIRE(output.consumed);
  REQUIRE(output.frame);
  REQUIRE(PtsUs(*output.frame) == kStart + 40500);
}

TEST_CASE(
    "Syncer paces burst input and retains queue heads until their deadline",
    "[syncer][timing]") {
  SyncerTiming timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).consumed);
  auto next = VideoInput(50040000);
  auto early = timing.Step(true, &next, 1000);
  REQUIRE_FALSE(early.consumed);
  REQUIRE_FALSE(early.frame);
  REQUIRE(early.wake_us == 40000);
  auto due = timing.Step(true, &next, 40000);
  REQUIRE(due.consumed);
  REQUIRE(due.frame);
  REQUIRE(PtsUs(*due.frame) == kStart + 40000);
}

TEST_CASE("Syncer excessive lateness delays the other track's common deadline",
          "[syncer][timing]") {
  auto config = MixedConfig();
  config.forward_start_shift = true;
  SyncerTiming timing(config);
  auto video = VideoInput(50000000);
  auto audio = AudioInput(2400000, 48000, 3840);
  REQUIRE(timing.Step(true, &video, 0).consumed);
  REQUIRE(timing.Step(false, &audio, 0).consumed);
  video = VideoInput(50040000);
  auto late = timing.Step(true, &video, 1000000);
  REQUIRE(late.consumed);
  REQUIRE(late.frame);
  REQUIRE(PtsUs(*late.frame) == kStart + 40000);
  audio = AudioInput(2403840);
  auto early = timing.Step(false, &audio, 1039999);
  REQUIRE_FALSE(early.consumed);
  REQUIRE_FALSE(early.frame);
  auto due = timing.Step(false, &audio, 1040000);
  REQUIRE(due.consumed);
  REQUIRE(due.frame);
  REQUIRE(PtsUs(*due.frame) == kStart + 80000);
}

TEST_CASE("Syncer small lateness releases immediately without moving the clock",
          "[syncer][timing]") {
  auto config = MixedConfig();
  config.forward_start_shift = true;
  SyncerTiming timing(config);
  auto video = VideoInput(0);
  auto audio = AudioInput(0, 48000, 4800);
  REQUIRE(timing.Step(true, &video, 0).consumed);
  REQUIRE(timing.Step(false, &audio, 0).consumed);
  video = VideoInput(40000);
  auto late = timing.Step(true, &video, 80000);
  REQUIRE(late.consumed);
  REQUIRE(late.frame);
  REQUIRE(PtsUs(*late.frame) == kStart + 40000);
  audio = AudioInput(4800);
  REQUIRE_FALSE(timing.Step(false, &audio, 99999).frame);
  auto due = timing.Step(false, &audio, 100000);
  REQUIRE(due.consumed);
  REQUIRE(due.frame);
  REQUIRE(PtsUs(*due.frame) == kStart + 100000);
}

TEST_CASE(
    "Syncer optionally discards moderate lateness before continuity repair",
    "[syncer][timing]") {
  auto config = VideoConfig();
  config.negative_time_discard = 30ms;
  SyncerTiming timing(config);
  auto first = VideoInput(0);
  REQUIRE(timing.Step(true, &first, 0).consumed);
  auto late = VideoInput(40000);
  auto discarded = timing.Step(true, &late, 80000);
  REQUIRE(discarded.consumed);
  REQUIRE_FALSE(discarded.frame);
  auto next = VideoInput(80000);
  auto filler = timing.Step(true, &next, 80000);
  REQUIRE_FALSE(filler.consumed);
  REQUIRE(filler.frame);
  REQUIRE(PtsUs(*filler.frame) == kStart + 40000);
  REQUIRE((*filler.frame)->data[0][0] == 99);
}

TEST_CASE(
    "Syncer jitter margin delays subsequent input but not the first anchor",
    "[syncer][timing]") {
  auto config = VideoConfig();
  config.jitter_margin = 50ms;
  SyncerTiming timing(config);
  auto first = VideoInput(0);
  auto anchor = timing.Step(true, &first, 0);
  REQUIRE(anchor.consumed);
  REQUIRE(anchor.frame);
  auto next = VideoInput(40000);
  auto early = timing.Step(true, &next, 89999);
  REQUIRE_FALSE(early.consumed);
  REQUIRE_FALSE(early.frame);
  auto due = timing.Step(true, &next, 90000);
  REQUIRE(due.consumed);
  REQUIRE(due.frame);
  REQUIRE(PtsUs(*due.frame) == kStart + 40000);
}

TEST_CASE("Syncer fractional frame rate stays exact across a long paced stream",
          "[syncer][timing]") {
  auto config = VideoConfig();
  config.encoder.fps = AVRational{30000, 1001};
  SyncerTiming timing(config);
  const AVRational time_base{1001, 30000};
  const auto start = av_rescale_q(kStart, AVRational{1, 1000000}, time_base);
  for (std::int64_t i = 0; i < 10000; ++i) {
    auto input = VideoInput(i);
    input.time_base = time_base;
    (*input.frame)->time_base = time_base;
    (*input.frame)->duration = 1;
    const auto now = av_rescale_rnd(i * 1001, 1000000, 30000, AV_ROUND_UP);
    auto result = timing.Step(true, &input, now);
    REQUIRE(result.consumed);
    REQUIRE(result.frame);
    REQUIRE((*result.frame)->pts == start + i);
    REQUIRE((*result.frame)->time_base.num == 1001);
    REQUIRE((*result.frame)->time_base.den == 30000);
    REQUIRE((*result.frame)->duration == 1);
  }
}

TEST_CASE(
    "Syncer backward jumps and generation resets preserve output continuity",
    "[syncer][timing]") {
  const auto generation = GENERATE(std::uint64_t{0}, std::uint64_t{1});
  SyncerTiming timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto restart = VideoInput(0, generation);
  auto result = timing.Step(true, &restart, 40000);
  REQUIRE(result.consumed);
  REQUIRE(result.frame);
  REQUIRE(PtsUs(*result.frame) == kStart + 40000);
}

TEST_CASE("Syncer large forward jumps do not leak the source discontinuity",
          "[syncer][timing]") {
  SyncerTiming timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto jump = VideoInput(300000000);
  std::int64_t previous = kStart;
  bool consumed = false;
  for (int i = 0; i < 100; ++i) {
    const auto result = timing.Step(true, &jump, 40000 + i * 40000);
    if (result.frame) {
      const auto pts = PtsUs(*result.frame);
      REQUIRE(pts > previous);
      REQUIRE(pts - previous <= 40000);
      previous = pts;
    }
    if (result.consumed) {
      consumed = true;
      REQUIRE(result.frame);
      REQUIRE((*result.frame)->data[0][0] == 99);
      break;
    }
  }
  REQUIRE(consumed);
}

TEST_CASE("Syncer audio continuity advances by exact sample counts",
          "[syncer][timing]") {
  const int rate = GENERATE(44100, 48000);
  SyncerTiming timing(AudioConfig(rate));
  auto first = AudioInput(50LL * rate, rate, 1024);
  auto result = timing.Step(false, &first, 0);
  REQUIRE(result.frame);
  REQUIRE((*result.frame)->pts == 10LL * rate);
  for (int i = 1; i <= 200; ++i) {
    auto input = AudioInput(50LL * rate + i * 1024, rate, 1024);
    const auto now = av_rescale_rnd(i * 1024LL, 1000000, rate, AV_ROUND_UP);
    result = timing.Step(false, &input, now);
    REQUIRE(result.consumed);
    REQUIRE(result.frame);
    REQUIRE((*result.frame)->time_base.num == 1);
    REQUIRE((*result.frame)->time_base.den == rate);
    REQUIRE((*result.frame)->pts == 10LL * rate + i * 1024);
  }
}

TEST_CASE("Syncer consumes EOF and continues the live media timeline",
          "[syncer][timing]") {
  SyncerTiming timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto eof = Eof();
  auto marker = timing.Step(true, &eof, 10000);
  REQUIRE(marker.consumed);
  REQUIRE_FALSE(marker.frame);
  const auto backups = DrainAt(timing, true, 2000000);
  REQUIRE_FALSE(backups.empty());
  REQUIRE(PtsUs(backups.front()) == kStart + 40000);
  auto next = VideoInput(0, 1);
  auto resumed = timing.Step(true, &next, 2040000);
  REQUIRE(resumed.consumed);
  REQUIRE(resumed.frame);
  REQUIRE(PtsUs(*resumed.frame) > PtsUs(backups.back()));
}

TEST_CASE("Syncer first real frame starts after the last emitted backup ends",
          "[syncer][timing]") {
  SyncerTiming timing(VideoConfig());
  REQUIRE_FALSE(timing.Step(true, nullptr, 1000000).frame);
  std::vector<ffmpeg::Frame> backups;
  std::int64_t now = 3000000;
  for (; now <= 3500000; now += 22000) {
    auto batch = DrainAt(timing, true, now);
    for (auto& frame : batch) {
      backups.push_back(std::move(frame));
    }
  }
  now -= 22000;
  REQUIRE_FALSE(backups.empty());
  const auto& last = backups.back();
  const auto previous_end = av_rescale_q(
      last->pts + last->duration, last->time_base, AVRational{1, 1000000});
  auto input = VideoInput(0);
  auto first = timing.Step(true, &input, now);
  REQUIRE(first.consumed);
  REQUIRE(first.frame);
  REQUIRE((*first.frame)->data[0][0] == 99);
  REQUIRE(PtsUs(*first.frame) >= previous_end);
}

// The first callback can be parked without relying on the scheduler's speed.
// Always release this gate before Stop joins the callback thread.
class CallbackGate {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return open_; });
  }
  bool WaitEntered() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 2s, [&] { return entered_; });
  }
  void Open() {
    std::lock_guard lock(mutex_);
    open_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool open_ = false;
};

TEST_CASE("Syncer full queue blocks its producer and stopping wakes it",
          "[syncer][thread]") {
  auto config = VideoConfig();
  config.input_capacity = 1;
  CallbackGate gate;
  Syncer syncer(config);
  syncer.SetOnVideoFrame([&](ffmpeg::Frame) { gate.Enter(); });
  REQUIRE(syncer.Start());
  REQUIRE(syncer.PushVideo(VideoInput(0)));
  const bool entered = gate.WaitEntered();
  if (!entered) {
    gate.Open();
    syncer.Stop();
    FAIL("Scheduler did not deliver the first frame");
  }
  const bool queued = syncer.PushVideo(VideoInput(40000));
  std::promise<void> started;
  auto producer = std::async(std::launch::async, [&] {
    started.set_value();
    return syncer.PushVideo(VideoInput(80000));
  });
  started.get_future().wait();
  const auto blocked = producer.wait_for(20ms);
  syncer.RequestStop();
  const auto released = producer.wait_for(1s);
  gate.Open();
  syncer.Stop();
  const bool accepted = producer.get();
  REQUIRE(queued);
  REQUIRE(blocked == std::future_status::timeout);
  REQUIRE(released == std::future_status::ready);
  REQUIRE_FALSE(accepted);
  REQUIRE_FALSE(syncer.error());
}

TEST_CASE("Syncer delivers paced audio and video serially on its own thread",
          "[syncer][thread]") {
  auto config = MixedConfig();
  config.forward_start_shift = true;
  Syncer syncer(config);
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::thread::id> threads;
  std::vector<std::chrono::steady_clock::time_point> video_times;
  int audio_count = 0;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame) {
    std::lock_guard lock(mutex);
    threads.push_back(std::this_thread::get_id());
    video_times.push_back(std::chrono::steady_clock::now());
    changed.notify_all();
  });
  syncer.SetOnAudioFrame([&](ffmpeg::Frame) {
    std::lock_guard lock(mutex);
    threads.push_back(std::this_thread::get_id());
    ++audio_count;
    changed.notify_all();
  });
  REQUIRE(syncer.Start());
  for (int i = 0; i < 3; ++i) {
    REQUIRE(syncer.PushVideo(VideoInput(i * 40000)));
    REQUIRE(syncer.PushAudio(AudioInput(i * 960)));
  }
  bool delivered;
  {
    std::unique_lock lock(mutex);
    delivered = changed.wait_for(
        lock, 2s, [&] { return video_times.size() >= 3 && audio_count >= 3; });
  }
  syncer.Stop();
  const auto count_after_stop = threads.size();
  REQUIRE(delivered);
  REQUIRE(video_times.size() == 3);
  REQUIRE(audio_count == 3);
  REQUIRE(video_times.back() - video_times.front() >= 65ms);
  REQUIRE(count_after_stop == 6);
  for (const auto thread : threads) {
    REQUIRE(thread == threads.front());
    REQUIRE(thread != std::this_thread::get_id());
  }
  REQUIRE_FALSE(syncer.PushVideo(VideoInput(120000)));
  REQUIRE(threads.size() == count_after_stop);
}

TEST_CASE(
    "Syncer Stop waits for an executing callback and cancels queued output",
    "[syncer][thread]") {
  CallbackGate gate;
  Syncer syncer(VideoConfig());
  int count = 0;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame) {
    ++count;
    gate.Enter();
  });
  REQUIRE(syncer.Start());
  REQUIRE(syncer.PushVideo(VideoInput(0)));
  const bool entered = gate.WaitEntered();
  if (!entered) {
    gate.Open();
    syncer.Stop();
    FAIL("Scheduler did not enter callback");
  }
  const bool queued = syncer.PushVideo(VideoInput(40000));
  auto stopped = std::async(std::launch::async, [&] { syncer.Stop(); });
  const auto waiting = stopped.wait_for(20ms);
  gate.Open();
  const auto finished = stopped.wait_for(2s);
  stopped.get();
  REQUIRE(queued);
  REQUIRE(waiting == std::future_status::timeout);
  REQUIRE(finished == std::future_status::ready);
  REQUIRE(count == 1);
  REQUIRE_FALSE(syncer.PushVideo(VideoInput(80000)));
  REQUIRE(count == 1);
}

TEST_CASE("Syncer worker stays live across EOF and a subsequent generation",
          "[syncer][thread]") {
  Syncer syncer(VideoConfig());
  std::promise<void> resumed;
  std::vector<std::int64_t> timestamps;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame frame) {
    timestamps.push_back(PtsUs(frame));
    if (timestamps.size() == 2) {
      syncer.RequestStop();
      resumed.set_value();
    }
  });
  REQUIRE(syncer.Start());
  REQUIRE(syncer.PushVideo(VideoInput(50000000)));
  REQUIRE(syncer.PushVideo(Eof()));
  REQUIRE(syncer.PushVideo(VideoInput(0, 1)));
  auto ready = resumed.get_future();
  const auto status = ready.wait_for(2s);
  syncer.Stop();
  REQUIRE(status == std::future_status::ready);
  REQUIRE(timestamps.size() == 2);
  REQUIRE(timestamps[0] == kStart);
  REQUIRE(timestamps[1] == kStart + 40000);
  REQUIRE_FALSE(syncer.error());
}

TEST_CASE("Syncer RequestStop is safe in callbacks and restart begins fresh",
          "[syncer][thread]") {
  Syncer syncer(VideoConfig());
  std::promise<std::int64_t> first;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame frame) {
    syncer.RequestStop();
    first.set_value(PtsUs(frame));
  });
  REQUIRE(syncer.Start());
  REQUIRE_FALSE(syncer.Start());
  REQUIRE(syncer.PushVideo(VideoInput(50000000, 8)));
  auto first_future = first.get_future();
  const auto first_status = first_future.wait_for(2s);
  syncer.Stop();
  REQUIRE(first_status == std::future_status::ready);
  REQUIRE(first_future.get() == kStart);
  std::promise<std::int64_t> second;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame frame) {
    syncer.RequestStop();
    second.set_value(PtsUs(frame));
  });
  REQUIRE(syncer.Start());
  REQUIRE(syncer.PushVideo(VideoInput(0)));
  auto second_future = second.get_future();
  const auto second_status = second_future.wait_for(2s);
  syncer.Stop();
  REQUIRE(second_status == std::future_status::ready);
  REQUIRE(second_future.get() == kStart);
  REQUIRE_FALSE(syncer.error());
}

TEST_CASE(
    "Syncer reports callback failure once and contains error callback throws",
    "[syncer][thread]") {
  Syncer syncer(VideoConfig());
  std::promise<std::exception_ptr> error;
  int error_count = 0;
  syncer.SetOnVideoFrame([](ffmpeg::Frame) {
    throw std::runtime_error("Business callback failed");
  });
  syncer.SetOnError([&](std::exception_ptr failure) {
    ++error_count;
    error.set_value(failure);
    throw std::logic_error("Error observer failed");
  });
  REQUIRE(syncer.Start());
  REQUIRE(syncer.PushVideo(VideoInput(0)));
  auto error_future = error.get_future();
  const auto status = error_future.wait_for(2s);
  syncer.Stop();
  REQUIRE(status == std::future_status::ready);
  REQUIRE(error_count == 1);
  REQUIRE(syncer.error());
  REQUIRE_THROWS_WITH(std::rethrow_exception(error_future.get()),
                      "Business callback failed");
  REQUIRE_FALSE(syncer.PushVideo(VideoInput(40000)));
}

TEST_CASE("Syncer validates disabled tracks and rejects reversed generations",
          "[syncer][thread]") {
  REQUIRE_THROWS_AS(Syncer{SyncerConfig{}}, std::invalid_argument);
  auto config = VideoConfig();
  config.input_capacity = 0;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  Syncer syncer(VideoConfig());
  REQUIRE(syncer.Start());
  REQUIRE_THROWS_AS(syncer.PushAudio(AudioInput(0)), std::invalid_argument);
  REQUIRE_THROWS_AS(syncer.PushVideo({}), std::invalid_argument);
  REQUIRE(syncer.PushVideo(VideoInput(0, 2)));
  REQUIRE_THROWS_AS(syncer.PushVideo(VideoInput(40000, 1)),
                    std::invalid_argument);
  REQUIRE(syncer.PushVideo(Eof(2)));
  REQUIRE_THROWS_AS(syncer.PushVideo(VideoInput(80000, 2)),
                    std::invalid_argument);
  syncer.Stop();
}

TEST_CASE("Syncer disables video by format even with a supplied backup",
          "[syncer][thread]") {
  auto config = AudioConfig();
  config.backup_video = Video(7);
  config.encoder.width = -1;
  config.encoder.height = -1;
  config.encoder.fps = AVRational{0, 0};
  Syncer syncer(config);
  int video_count = 0;
  std::promise<ffmpeg::Frame> delivered;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame) { ++video_count; });
  syncer.SetOnAudioFrame([&](ffmpeg::Frame frame) {
    syncer.RequestStop();
    delivered.set_value(std::move(frame));
  });
  REQUIRE(syncer.Start());
  REQUIRE_THROWS_AS(syncer.PushVideo(VideoInput(0)), std::invalid_argument);
  REQUIRE(syncer.PushAudio(AudioInput(0)));
  auto ready = delivered.get_future();
  const auto status = ready.wait_for(2s);
  syncer.Stop();
  REQUIRE(status == std::future_status::ready);
  REQUIRE(ready.get()->sample_rate == 48000);
  REQUIRE(video_count == 0);
  REQUIRE_FALSE(syncer.error());
}

TEST_CASE("Syncer ignores disabled audio fields and rejects no enabled tracks",
          "[syncer]") {
  auto config = VideoConfig();
  config.encoder.audio_sample_rate = -1;
  config.encoder.audio_channel_layout = {};
  REQUIRE_NOTHROW(Syncer{config});
  config.encoder.video_pixel_format = AV_PIX_FMT_NONE;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
}

TEST_CASE(
    "Syncer preserves video contents independently of the target and backup",
    "[syncer][thread]") {
  auto config = VideoConfig();
  config.encoder.video_pixel_format = AV_PIX_FMT_NV12;
  config.encoder.width = 16;
  config.encoder.height = 16;
  config.backup_video = RgbBackup();
  config.freeze = 0us;
  const auto* backup_data = (*config.backup_video)->data[0];
  auto input = VideoInput(0);
  const auto* input_data = (*input.frame)->data[0];
  Syncer syncer(config);
  std::promise<ffmpeg::Frame> delivered;
  syncer.SetOnVideoFrame([&](ffmpeg::Frame frame) {
    syncer.RequestStop();
    delivered.set_value(std::move(frame));
  });
  REQUIRE(syncer.Start());
  REQUIRE(syncer.PushVideo(input));
  auto ready = delivered.get_future();
  const auto status = ready.wait_for(2s);
  syncer.Stop();
  REQUIRE(status == std::future_status::ready);
  auto actual = ready.get();
  REQUIRE(actual->format == AV_PIX_FMT_GRAY8);
  REQUIRE(actual->width == 8);
  REQUIRE(actual->height == 8);
  REQUIRE(actual->data[0] == input_data);
  REQUIRE(actual->data[0][0] == 99);
  REQUIRE(PtsUs(actual) == kStart);
  REQUIRE_FALSE(syncer.error());

  SyncerTiming timing(config);
  REQUIRE(timing.Step(true, &input, 0).frame);
  auto output = timing.Step(true, nullptr, 2000000);
  REQUIRE(output.frame);
  const auto& backup = *output.frame;
  REQUIRE(backup->format == AV_PIX_FMT_RGB24);
  REQUIRE(backup->width == 4);
  REQUIRE(backup->height == 4);
  REQUIRE(backup->data[0] == backup_data);
  REQUIRE(backup->data[0][0] == 23);
  REQUIRE(PtsUs(backup) == kStart + 40000);
}

TEST_CASE("Syncer needs target dimensions only to create the default backup",
          "[syncer]") {
  auto config = VideoConfig();
  config.encoder.width = 0;
  config.encoder.height = 0;
  REQUIRE_NOTHROW(Syncer{config});
}

TEST_CASE("Syncer validates timing and enabled track configuration",
          "[syncer]") {
  auto config = VideoConfig();
  config.backup_video.reset();
  config.encoder.width = 0;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.backup_video.reset();
  config.encoder.height = 0;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.encoder.width = 16;
  REQUIRE_NOTHROW(Syncer{config});
  config = VideoConfig();
  config.encoder.video_pixel_format = AV_PIX_FMT_NV12;
  REQUIRE_NOTHROW(Syncer{config});
  config = VideoConfig();
  config.backup_video.reset();
  config.encoder.video_pixel_format = AV_PIX_FMT_CUDA;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.encoder.fps = AVRational{0, 1};
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.encoder.fps = AVRational{25, 0};
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = AudioConfig();
  config.encoder.audio_sample_rate = 0;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = AudioConfig();
  config.encoder.audio_channel_layout = {};
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = AudioConfig();
  config.encoder.audio_sample_format =
      static_cast<AVSampleFormat>(AV_SAMPLE_FMT_NB);
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.negative_time_tolerance = -1us;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.negative_time_discard = config.negative_time_tolerance;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.jitter_margin = config.discontinuity_threshold;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
  config = VideoConfig();
  config.freeze = -1us;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
}

TEST_CASE("Syncer retains CUDA surfaces for both real frames and backup output",
          "[.][syncer][cuda]") {
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  std::unique_ptr<AVBufferRef, void (*)(AVBufferRef*)> frames(
      av_hwframe_ctx_alloc(device.get()),
      [](AVBufferRef* reference) { av_buffer_unref(&reference); });
  REQUIRE(frames != nullptr);
  auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = AV_PIX_FMT_NV12;
  context->width = 128;
  context->height = 128;
  REQUIRE(av_hwframe_ctx_init(frames.get()) >= 0);
  ffmpeg::Frame backup;
  REQUIRE(av_hwframe_get_buffer(frames.get(), backup.get(), 0) >= 0);
  const auto* backup_surface = backup->data[0];
  SyncerConfig config;
  config.encoder.video_pixel_format = AV_PIX_FMT_NV12;
  config.encoder.width = 128;
  config.encoder.height = 128;
  config.encoder.fps = AVRational{25, 1};
  config.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.freeze = 0us;
  auto input = VideoInput(0);
  input.frame.emplace();
  REQUIRE(av_hwframe_get_buffer(frames.get(), input.frame->get(), 0) >= 0);
  (*input.frame)->pts = 0;
  (*input.frame)->time_base = input.time_base;
  const auto* real_surface = (*input.frame)->data[0];
  const auto* device_context = (*input.frame)->hw_frames_ctx->data;

  SECTION("A supplied CUDA backup retains its original surface") {
    config.backup_video = backup.Ref();
    SyncerTiming timing(config);
    auto real = timing.Step(true, &input, 0);
    REQUIRE(real.frame);
    REQUIRE((*real.frame)->format == AV_PIX_FMT_CUDA);
    REQUIRE((*real.frame)->data[0] == real_surface);
    REQUIRE((*real.frame)->hw_frames_ctx->data == device_context);
    input.frame.reset();
    backup.Unref();
    auto filler = timing.Step(true, nullptr, 2000000);
    REQUIRE(filler.frame);
    REQUIRE((*filler.frame)->format == AV_PIX_FMT_CUDA);
    REQUIRE((*filler.frame)->data[0] == backup_surface);
    REQUIRE((*filler.frame)->hw_frames_ctx->data == device_context);
    REQUIRE(PtsUs(*filler.frame) == kStart + 40000);
    REQUIRE((*real.frame)->data[0] == real_surface);
  }

  SECTION("A differently sized CPU backup does not restrict CUDA input") {
    config.encoder.width = 16;
    config.encoder.height = 16;
    config.backup_video = RgbBackup();
    const auto* cpu_backup_data = (*config.backup_video)->data[0];
    Syncer syncer(config);
    std::promise<ffmpeg::Frame> delivered;
    syncer.SetOnVideoFrame([&](ffmpeg::Frame frame) {
      syncer.RequestStop();
      delivered.set_value(std::move(frame));
    });
    REQUIRE(syncer.Start());
    REQUIRE(syncer.PushVideo(input));
    auto ready = delivered.get_future();
    const auto status = ready.wait_for(2s);
    syncer.Stop();
    REQUIRE(status == std::future_status::ready);
    auto actual = ready.get();
    REQUIRE(actual->format == AV_PIX_FMT_CUDA);
    REQUIRE(actual->width == 128);
    REQUIRE(actual->height == 128);
    REQUIRE(actual->data[0] == real_surface);
    REQUIRE(actual->hw_frames_ctx->data == device_context);
    REQUIRE_FALSE(syncer.error());

    SyncerTiming timing(config);
    REQUIRE(timing.Step(true, &input, 0).frame);
    auto filler = timing.Step(true, nullptr, 2000000);
    REQUIRE(filler.frame);
    REQUIRE((*filler.frame)->format == AV_PIX_FMT_RGB24);
    REQUIRE((*filler.frame)->width == 4);
    REQUIRE((*filler.frame)->height == 4);
    REQUIRE((*filler.frame)->hw_frames_ctx == nullptr);
    REQUIRE((*filler.frame)->data[0] == cpu_backup_data);
    REQUIRE((*filler.frame)->data[0][0] == 23);
    REQUIRE(PtsUs(*filler.frame) == kStart + 40000);
  }

  SECTION(
      "The default CPU backup permits real CUDA input with the same layout") {
    Syncer syncer(config);
    std::promise<ffmpeg::Frame> delivered;
    syncer.SetOnVideoFrame([&](ffmpeg::Frame frame) {
      syncer.RequestStop();
      delivered.set_value(std::move(frame));
    });
    REQUIRE(syncer.Start());
    REQUIRE(syncer.PushVideo(input));
    auto ready = delivered.get_future();
    const auto status = ready.wait_for(2s);
    syncer.Stop();
    REQUIRE(status == std::future_status::ready);
    auto actual = ready.get();
    REQUIRE(actual->format == AV_PIX_FMT_CUDA);
    REQUIRE(actual->data[0] == real_surface);
    REQUIRE(actual->hw_frames_ctx->data == device_context);
    REQUIRE(PtsUs(actual) == kStart);
    REQUIRE_FALSE(syncer.error());

    SyncerTiming timing(config);
    auto real = timing.Step(true, &input, 0);
    REQUIRE(real.frame);
    REQUIRE((*real.frame)->format == AV_PIX_FMT_CUDA);
    REQUIRE((*real.frame)->data[0] == real_surface);
    auto filler = timing.Step(true, nullptr, 2000000);
    REQUIRE(filler.frame);
    const auto& frame = *filler.frame;
    REQUIRE(frame->format == AV_PIX_FMT_NV12);
    REQUIRE(frame->width == 128);
    REQUIRE(frame->height == 128);
    REQUIRE(frame->hw_frames_ctx == nullptr);
    REQUIRE(PtsUs(frame) == kStart + 40000);
    for (int plane = 0; plane < 2; ++plane) {
      const int rows = plane == 0 ? 128 : 64;
      const int black = plane == 0 ? 16 : 128;
      for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < 128; ++column) {
          REQUIRE(frame->data[plane][row * frame->linesize[plane] + column] ==
                  black);
        }
      }
    }
  }
}

}  // namespace
