#include "mw/streamer/syncer/syncer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::Syncer;
using mw::streamer::SyncerConfig;
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

AvDecoderReadResult VideoInput(std::int64_t pts) {
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kFrame;
  input.frame = Video(99);
  (*input.frame)->pts = pts;
  (*input.frame)->duration = 40000;
  (*input.frame)->time_base = AVRational{1, 1000000};
  input.time_base = AVRational{1, 1000000};
  return input;
}

AvDecoderReadResult AudioInput(std::int64_t pts, int rate = 48000,
                               int samples = 960) {
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
  input.stream_index = 1;
  return input;
}

AvDecoderReadResult Eof() {
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kEof;
  return input;
}

AvDecoderReadResult FpsVideoInput(std::int64_t pts) {
  auto input = VideoInput(pts);
  input.time_base = AVRational{1, 25};
  (*input.frame)->time_base = input.time_base;
  (*input.frame)->duration = 1;
  return input;
}

std::int64_t PtsUs(const ffmpeg::Frame& frame) {
  return av_rescale_q(frame->pts, frame->time_base, AVRational{1, 1000000});
}

std::vector<ffmpeg::Frame> DrainAt(Syncer& timing, bool video,
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
  Syncer timing(config);
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
  Syncer timing(config);
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
  Syncer before(VideoConfig());
  REQUIRE_FALSE(before.Step(true, nullptr, 0).frame);
  REQUIRE_FALSE(before.Step(true, nullptr, 1000000).frame);
  REQUIRE_FALSE(before.Step(true, nullptr, 2999999).frame);
  Syncer timing(VideoConfig());
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
  Syncer timing(VideoConfig());
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
  Syncer timing(AudioConfig(48000, format));
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
  Syncer timing(config);
  auto video = VideoInput(50000000);
  auto audio = AudioInput(2403840);
  auto first = timing.Step(true, &video, 0);
  REQUIRE(first.frame);
  REQUIRE(PtsUs(*first.frame) == kStart);
  auto second = timing.Step(false, &audio, 80000);
  REQUIRE(second.consumed);
  REQUIRE(second.frame);
  REQUIRE(PtsUs(*second.frame) == kStart + 80000);
}

TEST_CASE("Syncer default fills an initial audio offset before its real frame",
          "[syncer][timing]") {
  auto config = MixedConfig();
  Syncer timing(config);
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
  Syncer timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto next = VideoInput(50040500);
  auto output = timing.Step(true, &next, 40500);
  REQUIRE(output.consumed);
  REQUIRE(output.frame);
  REQUIRE(PtsUs(*output.frame) == kStart + 40500);
}

TEST_CASE("Syncer fractional frame rate stays exact across a long paced stream",
          "[syncer][timing]") {
  auto config = VideoConfig();
  config.encoder.fps = AVRational{30000, 1001};
  Syncer timing(config);
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

TEST_CASE("Syncer backward PTS jumps preserve output continuity",
          "[syncer][timing]") {
  Syncer timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto restart = VideoInput(0);
  auto result = timing.Step(true, &restart, 40000);
  REQUIRE(result.consumed);
  REQUIRE(result.frame);
  REQUIRE(PtsUs(*result.frame) == kStart + 40000);
}

TEST_CASE("Syncer shared correction converges after interleaved PTS resets",
          "[syncer][timing]") {
  const bool video_leads = GENERATE(true, false);
  const bool forward_start_shift = GENERATE(false, true);
  auto config = MixedConfig();
  config.forward_start_shift = forward_start_shift;
  Syncer timing(config);
  std::vector<AvDecoderReadResult> video{FpsVideoInput(1250),
                                         FpsVideoInput(1251)};
  std::vector<AvDecoderReadResult> audio;
  if (video_leads) {
    for (int index = 0; index < 4; ++index) {
      audio.push_back(AudioInput(2400000 + index * 960, 48000, 960));
    }
  } else {
    audio.push_back(AudioInput(2400000, 48000, 3840));
  }
  const auto old_video_count = video.size();
  const auto old_audio_count = audio.size();
  constexpr std::size_t kFrames = 100;
  for (std::size_t index = 0; index < kFrames; ++index) {
    video.push_back(FpsVideoInput(static_cast<std::int64_t>(index)));
    audio.push_back(
        AudioInput(static_cast<std::int64_t>(index) * 1920, 48000, 1920));
  }
  std::vector<std::int64_t> video_pts;
  std::vector<std::int64_t> audio_pts;
  std::size_t video_index = 0;
  std::size_t audio_index = 0;
  std::int64_t now = 0;
  std::int64_t video_due = 0;
  std::int64_t audio_due = 0;
  for (int iteration = 0; iteration < 10000; ++iteration) {
    auto wake = std::numeric_limits<std::int64_t>::max();
    for (bool is_video : {true, false}) {
      const auto& inputs = is_video ? video : audio;
      auto& index = is_video ? video_index : audio_index;
      auto& outputs = is_video ? video_pts : audio_pts;
      const auto old_count = is_video ? old_video_count : old_audio_count;
      if (index == inputs.size()) continue;
      auto& due = is_video ? video_due : audio_due;
      if (now < due) {
        wake = std::min(wake, due);
        continue;
      }
      const auto step = timing.Step(is_video, &inputs[index], now);
      if (step.frame) {
        const auto& frame = *step.frame;
        due = now + av_rescale_q(frame->duration, frame->time_base,
                                 AVRational{1, 1000000});
      } else {
        due = step.wake_us;
      }
      wake = std::min(wake, due);
      if (step.consumed) {
        REQUIRE(step.frame);
        if (index >= old_count) outputs.push_back(PtsUs(*step.frame));
        ++index;
      }
    }
    if (video_index == video.size() && audio_index == audio.size()) break;
    REQUIRE(wake != std::numeric_limits<std::int64_t>::max());
    now = std::max(now, wake);
  }
  REQUIRE(video_pts.size() == kFrames);
  REQUIRE(audio_pts.size() == kFrames);
  // A 1/25 video time base must feed its quantized correction back to the
  // shared state; otherwise audio permanently retains a 13.333 ms residual.
  for (std::size_t index = 10; index < kFrames; ++index) {
    CAPTURE(video_leads, forward_start_shift, index);
    REQUIRE(video_pts[index] == audio_pts[index]);
    REQUIRE(video_pts[index] - video_pts[index - 1] == 40000);
    REQUIRE(audio_pts[index] - audio_pts[index - 1] == 40000);
  }
}

TEST_CASE("Syncer large forward jumps do not leak the source discontinuity",
          "[syncer][timing]") {
  Syncer timing(VideoConfig());
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
  Syncer timing(AudioConfig(rate));
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
  Syncer timing(VideoConfig());
  auto first = VideoInput(50000000);
  REQUIRE(timing.Step(true, &first, 0).frame);
  auto eof = Eof();
  auto marker = timing.Step(true, &eof, 10000);
  REQUIRE(marker.consumed);
  REQUIRE_FALSE(marker.frame);
  const auto backups = DrainAt(timing, true, 2000000);
  REQUIRE_FALSE(backups.empty());
  REQUIRE(PtsUs(backups.front()) == kStart + 40000);
  auto next = VideoInput(0);
  auto resumed = timing.Step(true, &next, 2040000);
  REQUIRE(resumed.consumed);
  REQUIRE(resumed.frame);
  REQUIRE(PtsUs(*resumed.frame) > PtsUs(backups.back()));
}

TEST_CASE("Syncer first real frame starts after the last emitted backup ends",
          "[syncer][timing]") {
  Syncer timing(VideoConfig());
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

TEST_CASE("Syncer ignores disabled audio fields and rejects no enabled tracks",
          "[syncer]") {
  auto config = VideoConfig();
  config.encoder.audio_sample_rate = -1;
  config.encoder.audio_channel_layout = {};
  REQUIRE_NOTHROW(Syncer{config});
  config.encoder.video_pixel_format = AV_PIX_FMT_NONE;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
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
  config.freeze = -1us;
  REQUIRE_THROWS_AS(Syncer{config}, std::invalid_argument);
}

TEST_CASE("Syncer consumes input without owning realtime pacing", "[syncer]") {
  Syncer syncer(VideoConfig());
  auto first = VideoInput(50000000);
  auto second = VideoInput(50040000);
  REQUIRE(syncer.Step(true, &first, 0).consumed);
  auto output = syncer.Step(true, &second, 0);
  REQUIRE(output.consumed);
  REQUIRE(output.frame);
  REQUIRE(PtsUs(*output.frame) == kStart + 40000);
  REQUIRE((*second.frame)->pts == 50040000);
  REQUIRE((*output.frame)->data[0] == (*second.frame)->data[0]);
}

TEST_CASE("Syncer reset discards both shared timeline and frozen picture",
          "[syncer]") {
  Syncer syncer(VideoConfig());
  auto input = VideoInput(50000000);
  REQUIRE(syncer.Step(true, &input, 0).frame);
  syncer.Reset();
  input = VideoInput(0);
  const auto reset = syncer.Step(true, &input, 0);
  REQUIRE(reset.consumed);
  REQUIRE(reset.frame);
  REQUIRE(PtsUs(*reset.frame) == kStart);
  syncer.Reset();
  REQUIRE_FALSE(syncer.Step(true, nullptr, 1000000).frame);
  const auto backup = syncer.Step(true, nullptr, 3000000);
  REQUIRE(backup.frame);
  REQUIRE((*backup.frame)->data[0][0] == 7);
}

TEST_CASE("Syncer rejects disabled tracks and malformed inputs", "[syncer]") {
  Syncer syncer(VideoConfig());
  REQUIRE_THROWS_AS(syncer.Step(false, nullptr, 0), std::invalid_argument);
  AvDecoderReadResult input;
  REQUIRE_THROWS_AS(syncer.Step(true, &input, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(syncer.Step(true, nullptr, -1), std::invalid_argument);
  input = VideoInput(AV_NOPTS_VALUE);
  REQUIRE_THROWS_AS(syncer.Step(true, &input, 0), std::invalid_argument);
  input = Eof();
  input.frame = Video(1);
  REQUIRE_THROWS_AS(syncer.Step(true, &input, 0), std::invalid_argument);
}

TEST_CASE(
    "Syncer preserves real and backup video storage independently of targets",
    "[syncer]") {
  auto config = VideoConfig();
  config.encoder.video_pixel_format = AV_PIX_FMT_NV12;
  config.encoder.width = 16;
  config.encoder.height = 16;
  config.freeze = 0us;
  const auto* backup_data = (*config.backup_video)->data[0];
  Syncer syncer(config);
  auto input = VideoInput(0);
  const auto* input_data = (*input.frame)->data[0];
  auto real = syncer.Step(true, &input, 0);
  REQUIRE(real.frame);
  REQUIRE((*real.frame)->format == AV_PIX_FMT_GRAY8);
  REQUIRE((*real.frame)->width == 8);
  REQUIRE((*real.frame)->height == 8);
  REQUIRE((*real.frame)->data[0] == input_data);
  REQUIRE((*input.frame)->pts == 0);
  input.frame.reset();
  config.backup_video.reset();
  auto backup = syncer.Step(true, nullptr, 2000000);
  REQUIRE(backup.frame);
  REQUIRE((*backup.frame)->format == AV_PIX_FMT_GRAY8);
  REQUIRE((*backup.frame)->width == 8);
  REQUIRE((*backup.frame)->data[0] == backup_data);
  REQUIRE((*backup.frame)->data[0][0] == 7);
  REQUIRE((*real.frame)->data[0] == input_data);
}

TEST_CASE("Syncer retains CUDA surfaces without download for real and backup",
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
  auto config = VideoConfig();
  config.backup_video = backup.Ref();
  config.freeze = 0us;
  const auto* backup_surface = backup->data[0];
  Syncer syncer(config);
  auto input = VideoInput(0);
  input.frame.emplace();
  REQUIRE(av_hwframe_get_buffer(frames.get(), input.frame->get(), 0) >= 0);
  (*input.frame)->pts = 0;
  const auto* real_surface = (*input.frame)->data[0];
  auto real = syncer.Step(true, &input, 0);
  REQUIRE(real.frame);
  REQUIRE((*real.frame)->format == AV_PIX_FMT_CUDA);
  REQUIRE((*real.frame)->data[0] == real_surface);
  REQUIRE((*real.frame)->hw_frames_ctx->data == frames->data);
  input.frame.reset();
  backup.Unref();
  auto filler = syncer.Step(true, nullptr, 2000000);
  REQUIRE(filler.frame);
  REQUIRE((*filler.frame)->format == AV_PIX_FMT_CUDA);
  REQUIRE((*filler.frame)->data[0] == backup_surface);
  REQUIRE((*filler.frame)->hw_frames_ctx->data == frames->data);
  REQUIRE((*real.frame)->data[0] == real_surface);
}

}  // namespace
