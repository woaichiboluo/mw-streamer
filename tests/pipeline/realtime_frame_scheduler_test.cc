#include "mw/synchronizer/internal/realtime_frame_scheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::media::FrameStreamsReady;
using mw::streamer::synchronizer::SynchronizerSinkConfig;
using mw::streamer::synchronizer::internal::RealtimeFrameScheduler;
namespace ffmpeg = mw::streamer::ffmpeg;
using Clock = RealtimeFrameScheduler::Clock;
constexpr int kWidth = 256;
constexpr int kHeight = 144;

Clock::time_point Epoch() { return Clock::time_point(100s); }

SynchronizerSinkConfig Config() {
  SynchronizerSinkConfig config;
  config.max_frame_lateness = 40ms;
  config.standby_timeout = 100ms;
  return config;
}

FrameStreamsReady Streams(bool audio, std::uint64_t generation = 1,
                          const ffmpeg::HardwareContext* device = nullptr,
                          AVRational video_frame_rate = {20, 1}) {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = audio ? AVRational{1, 48000} : AVRational{1, 1000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = audio ? AV_CODEC_ID_AAC : AV_CODEC_ID_H264;
  if (audio) {
    parameters->sample_rate = 48000;
    av_channel_layout_default(&parameters->ch_layout, 1);
  } else {
    parameters->width = kWidth;
    parameters->height = kHeight;
    parameters->framerate = video_frame_rate;
  }
  return {generation, {std::move(stream)}, device};
}

ffmpeg::Frame Video(std::int64_t pts, std::uint8_t marker = 32) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = kWidth;
  frame->height = kHeight;
  frame->pts = pts;
  frame->duration = 50;
  frame->time_base = {1, 1000};
  frame->color_range = AVCOL_RANGE_MPEG;
  ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 32),
                       "allocate scheduler video");
  for (int plane = 0; plane < 3; ++plane) {
    std::memset(frame->data[plane], plane == 0 ? marker : 128,
                static_cast<std::size_t>(frame->linesize[plane]) *
                    (plane == 0 ? kHeight : kHeight / 2));
  }
  return frame;
}

ffmpeg::Frame Audio(std::int64_t pts, float marker) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 960;
  frame->pts = pts;
  frame->duration = 960;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 1);
  ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 0),
                       "allocate scheduler audio");
  auto* samples = reinterpret_cast<float*>(frame->data[0]);
  for (int index = 0; index < frame->nb_samples; ++index)
    samples[index] = marker;
  return frame;
}

bool IsAudioValue(const ffmpeg::Frame& frame, float expected) {
  const auto* samples = reinterpret_cast<const float*>(frame->data[0]);
  for (int index = 0; index < frame->nb_samples; ++index) {
    if (samples[index] != expected) return false;
  }
  return true;
}

ffmpeg::Frame CudaVideo(const ffmpeg::HardwareContext& device) {
  AVBufferRef* pool =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(device.get()));
  if (!pool) throw std::bad_alloc();
  auto* context = reinterpret_cast<AVHWFramesContext*>(pool->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = AV_PIX_FMT_NV12;
  context->width = kWidth;
  context->height = kHeight;
  context->initial_pool_size = 2;
  try {
    ffmpeg::ThrowIfError(av_hwframe_ctx_init(pool),
                         "initialize scheduler CUDA pool");
    ffmpeg::Frame software;
    software->format = AV_PIX_FMT_NV12;
    software->width = kWidth;
    software->height = kHeight;
    ffmpeg::ThrowIfError(av_frame_get_buffer(software.get(), 32),
                         "allocate scheduler CUDA upload");
    std::memset(software->data[0], 32,
                static_cast<std::size_t>(software->linesize[0]) * kHeight);
    std::memset(software->data[1], 128,
                static_cast<std::size_t>(software->linesize[1]) * kHeight / 2);
    ffmpeg::Frame frame;
    ffmpeg::ThrowIfError(av_hwframe_get_buffer(pool, frame.get(), 0),
                         "allocate scheduler CUDA frame");
    ffmpeg::ThrowIfError(
        av_hwframe_transfer_data(frame.get(), software.get(), 0),
        "upload scheduler CUDA frame");
    frame->time_base = {1, 1000};
    frame->pts = 0;
    frame->duration = 50;
    frame->sample_aspect_ratio = {1, 1};
    frame->color_range = AVCOL_RANGE_MPEG;
    av_buffer_unref(&pool);
    return frame;
  } catch (...) {
    av_buffer_unref(&pool);
    throw;
  }
}

ffmpeg::Frame Download(const ffmpeg::Frame& frame) {
  ffmpeg::Frame downloaded;
  ffmpeg::ThrowIfError(
      av_hwframe_transfer_data(downloaded.get(), frame.get(), 0),
      "download retained scheduler CUDA frame");
  return downloaded;
}

}  // namespace

TEST_CASE(
    "RealtimeFrameScheduler rational video slots do not accumulate rounding "
    "error") {
  auto config = Config();
  config.standby_timeout = 60s;
  RealtimeFrameScheduler scheduler(config);
  scheduler.Configure(Streams(false, 1, nullptr, {30000, 1001}));
  scheduler.Push({1, Video(0)}, false, Epoch());
  constexpr AVRational kFrameTimeBase{1001, 30000};
  constexpr AVRational kMicroseconds{1, 1000000};
  for (std::int64_t slot = 0; slot < 1000; ++slot) {
    const auto due = Epoch() + config.max_frame_lateness +
                     std::chrono::microseconds(
                         av_rescale_q(slot, kFrameTimeBase, kMicroseconds));
    auto output = scheduler.TakeReady(due);
    REQUIRE(output.has_value());
    CHECK_FALSE(output->audio);
    CHECK(output->frame->pts == slot);
    CHECK(output->frame->duration == 1);
    CHECK(output->frame->time_base.num == 1001);
    CHECK(output->frame->time_base.den == 30000);
    const auto next = Epoch() + config.max_frame_lateness +
                      std::chrono::microseconds(av_rescale_q(
                          slot + 1, kFrameTimeBase, kMicroseconds));
    CHECK_FALSE(scheduler.TakeReady(due).has_value());
    REQUIRE(scheduler.deadline().has_value());
    CHECK(*scheduler.deadline() == next);
    CHECK_FALSE(scheduler.TakeReady(next - 1us).has_value());
    CHECK(*scheduler.deadline() == next);
  }
  // The exact rational schedule differs from repeated rounded frame durations.
  CHECK(av_rescale_q(1000, kFrameTimeBase, kMicroseconds) !=
        1000 * av_rescale_q(1, kFrameTimeBase, kMicroseconds));
}

TEST_CASE(
    "RealtimeFrameScheduler stalled worker skips backlog without changing the "
    "source mapping") {
  RealtimeFrameScheduler scheduler(Config());
  scheduler.Configure(Streams(true));
  scheduler.Push({1, Audio(0, 0.25F)}, true, Epoch());
  auto initial = scheduler.TakeReady(Epoch() + 40ms);
  REQUIRE(initial.has_value());
  CHECK(initial->frame->pts == 0);
  CHECK(IsAudioValue(initial->frame, 0.25F));

  // This newly arrived frame still belongs to source time 20 ms, not 1 second.
  scheduler.Push({1, Audio(960, 0.5F)}, true, Epoch() + 1s);
  auto resumed = scheduler.TakeReady(Epoch() + 1s);
  REQUIRE(resumed.has_value());
  CHECK(resumed->frame->pts == 960);
  CHECK(IsAudioValue(resumed->frame, 0.0F));
  CHECK_FALSE(scheduler.TakeReady(Epoch() + 1s).has_value());
  REQUIRE(scheduler.deadline().has_value());
  CHECK(*scheduler.deadline() == Epoch() + 1020ms);

  // The fixed 40 ms arrival budget still separates source and release time.
  scheduler.Push({1, Audio(47040, 0.75F)}, true, Epoch() + 1020ms);
  auto fresh = scheduler.TakeReady(Epoch() + 1020ms);
  REQUIRE(fresh.has_value());
  CHECK(fresh->frame->pts == 1920);
  CHECK(IsAudioValue(fresh->frame, 0.75F));
  CHECK_FALSE(scheduler.TakeReady(Epoch() + 1020ms).has_value());
  REQUIRE(scheduler.deadline().has_value());
  CHECK(*scheduler.deadline() == Epoch() + 1040ms);
  CHECK(scheduler.queue_depth() == 0);
}

TEST_CASE(
    "RealtimeFrameScheduler reset remaps a new source while output ticks "
    "remain continuous") {
  RealtimeFrameScheduler scheduler(Config());
  scheduler.Configure(Streams(false));
  auto first_frame = Video(10000, 32);
  scheduler.Push({1, first_frame}, false, Epoch());
  auto first = scheduler.TakeReady(Epoch() + 40ms);
  REQUIRE(first.has_value());
  CHECK(first->frame->pts == 0);
  auto repeat = scheduler.TakeReady(Epoch() + 90ms);
  REQUIRE(repeat.has_value());
  CHECK(repeat->frame->pts == 1);
  CHECK(repeat->frame->data[0] == first_frame->data[0]);

  auto late_frame = Video(10050, 113);
  scheduler.Push({1, late_frame}, false, Epoch() + 1s);
  auto late_slot = scheduler.TakeReady(Epoch() + 1s);
  REQUIRE(late_slot.has_value());
  CHECK(late_slot->frame->pts == 2);
  CHECK(late_slot->frame->data[0] != late_frame->data[0]);
  CHECK(scheduler.standby());

  scheduler.Reset();
  scheduler.Configure(Streams(false, 2));
  auto new_frame = Video(200, 211);
  scheduler.Push({2, new_frame}, false, Epoch() + 1050ms);
  // The source has just remapped. Keep the existing output tick, then release
  // the new frame once its nominal source time is within the selection window.
  auto waiting = scheduler.TakeReady(Epoch() + 1050ms);
  REQUIRE(waiting.has_value());
  CHECK(waiting->frame->pts == 3);
  CHECK(scheduler.standby());
  auto recovered = scheduler.TakeReady(Epoch() + 1100ms);
  REQUIRE(recovered.has_value());
  CHECK(recovered->frame->pts == 4);
  CHECK(recovered->frame->data[0] == new_frame->data[0]);
  CHECK_FALSE(scheduler.standby());
  CHECK(first_frame->pts == 10000);
  CHECK(new_frame->pts == 200);
}

TEST_CASE(
    "RealtimeFrameScheduler retains CUDA original and standby frames beyond "
    "source and scheduler lifetime",
    "[.cuda]") {
  std::optional<ffmpeg::Frame> original;
  std::optional<ffmpeg::Frame> standby;
  {
    RealtimeFrameScheduler scheduler(Config());
    {
      auto device = ffmpeg::HardwareContext::CreateCuda(0);
      scheduler.Configure(Streams(false, 1, &device));
      auto source = CudaVideo(device);
      scheduler.Push({1, source}, false, Epoch());
      auto output = scheduler.TakeReady(Epoch() + 40ms);
      REQUIRE(output.has_value());
      original = output->frame.Ref();
    }
    // Only scheduler/frame references now own the CUDA device and frame pools.
    auto output = scheduler.TakeReady(Epoch() + 200ms);
    REQUIRE(output.has_value());
    REQUIRE(scheduler.standby());
    standby = output->frame.Ref();
    REQUIRE((*original)->format == AV_PIX_FMT_CUDA);
    REQUIRE((*standby)->format == AV_PIX_FMT_CUDA);
    CHECK((*original)->data[0] != (*standby)->data[0]);
  }
  auto downloaded_original = Download(*original);
  auto downloaded_standby = Download(*standby);
  for (const auto* frame : {&downloaded_original, &downloaded_standby}) {
    CHECK((*frame)->width == kWidth);
    CHECK((*frame)->height == kHeight);
    CHECK((*frame)->format == AV_PIX_FMT_NV12);
    REQUIRE((*frame)->data[0] != nullptr);
  }
  bool standby_differs = false;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      CHECK(downloaded_original
                ->data[0][y * downloaded_original->linesize[0] + x] == 32);
      standby_differs =
          standby_differs ||
          downloaded_standby
                  ->data[0][y * downloaded_standby->linesize[0] + x] != 32;
    }
  }
  CHECK(standby_differs);
}

TEST_CASE(
    "RealtimeFrameScheduler starts from retained current media after a bounded "
    "startup prefix") {
  auto config = Config();
  config.frame_queue_capacity = 2;
  RealtimeFrameScheduler scheduler(config);
  auto streams = Streams(false);
  auto audio = Streams(true).source_streams.front();
  audio.stream_index = 1;
  streams.source_streams.push_back(std::move(audio));
  scheduler.Configure(streams);
  // Audio's first 30 seconds were discarded while video had no prototype.
  for (int index = 0; index <= 1500; ++index) {
    scheduler.Push({1, Audio(index * 960, 0.75F)}, true, Epoch());
  }
  CHECK_FALSE(scheduler.TakeReady(Epoch()).has_value());
  CHECK(scheduler.queue_depth() <= config.frame_queue_capacity);
  auto current_video = Video(30000, 211);
  scheduler.Push({1, current_video}, false, Epoch());
  auto first = scheduler.TakeReady(Epoch() + 40ms);
  auto second = scheduler.TakeReady(Epoch() + 40ms);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->audio != second->audio);
  const auto& first_audio = first->audio ? first->frame : second->frame;
  const auto& first_video = first->audio ? second->frame : first->frame;
  CHECK(first_audio->pts == 0);
  CHECK(IsAudioValue(first_audio, 0.75F));
  CHECK(first_video->pts == 0);
  CHECK(first_video->data[0] == current_video->data[0]);
  CHECK_FALSE(scheduler.standby());
}

TEST_CASE(
    "RealtimeFrameScheduler buffers one millisecond late video without losing "
    "marker frames",
    "[arrival_buffer]") {
  auto config = Config();
  RealtimeFrameScheduler scheduler(config);
  auto streams = Streams(false, 1, nullptr, {25, 1});
  auto audio = Streams(true).source_streams.front();
  audio.stream_index = 1;
  streams.source_streams.push_back(std::move(audio));
  scheduler.Configure(streams);
  scheduler.Push({1, Video(0, 32)}, false, Epoch());
  scheduler.Push({1, Audio(0, 0.25F)}, true, Epoch());
  int video_count = 0;
  int audio_count = 0;
  int white_frames = 0;
  // Arrival times are independent of release times. Every video frame after
  // startup is one millisecond late, while audio arrives on its source tick.
  for (int milliseconds = 0; milliseconds <= 4040; ++milliseconds) {
    const auto now = Epoch() + std::chrono::milliseconds(milliseconds);
    if (milliseconds > 0 && milliseconds % 40 == 1) {
      const int source_ms = milliseconds - 1;
      if (source_ms > 0) {
        const bool white = source_ms >= 40 && source_ms < 160;
        scheduler.Push({1, Video(source_ms, white ? 235 : 32)}, false, now);
      }
    }
    if (milliseconds > 0 && milliseconds % 20 == 0) {
      scheduler.Push(
          {1, Audio(milliseconds * 48, 0.25F + (milliseconds / 20) / 1000.0F)},
          true, now);
    }
    while (auto output = scheduler.TakeReady(now)) {
      REQUIRE(milliseconds >= 40);
      if (output->audio) {
        CHECK(output->frame->pts == audio_count * 960);
        CHECK(milliseconds == audio_count * 20 + 40);
        CHECK(IsAudioValue(output->frame, 0.25F + audio_count / 1000.0F));
        ++audio_count;
      } else {
        CHECK(output->frame->pts == video_count);
        CHECK(milliseconds == video_count * 40 + 40);
        const bool white = video_count >= 1 && video_count <= 3;
        CHECK(output->frame->data[0][0] == (white ? 235 : 32));
        if (output->frame->data[0][0] == 235) ++white_frames;
        ++video_count;
      }
    }
    REQUIRE(scheduler.deadline().has_value());
    CHECK(*scheduler.deadline() > now);
  }
  CHECK(video_count == 101);
  CHECK(audio_count == 201);
  CHECK(white_frames * 40 == 120);
}

TEST_CASE(
    "RealtimeFrameScheduler has a bounded release deadline and drains EOF",
    "[arrival_buffer]") {
  RealtimeFrameScheduler scheduler(Config());
  scheduler.Configure(Streams(false));
  scheduler.Push({1, Video(0, 32)}, false, Epoch());
  scheduler.Finish();
  CHECK_FALSE(scheduler.TakeReady(Epoch()).has_value());
  REQUIRE(scheduler.deadline().has_value());
  CHECK(*scheduler.deadline() == Epoch() + 40ms);
  CHECK_FALSE(scheduler.TakeReady(Epoch() + 40ms - 1us).has_value());
  CHECK_FALSE(scheduler.finished());
  auto output = scheduler.TakeReady(Epoch() + 40ms);
  REQUIRE(output.has_value());
  CHECK(output->frame->pts == 0);
  CHECK(output->frame->data[0][0] == 32);
  CHECK(scheduler.finished());
  CHECK_FALSE(scheduler.TakeReady(Epoch() + 90ms).has_value());
}

TEST_CASE(
    "RealtimeFrameScheduler keeps realtime output when arrival exceeds the "
    "budget",
    "[arrival_buffer]") {
  RealtimeFrameScheduler scheduler(Config());
  scheduler.Configure(Streams(false));
  scheduler.Push({1, Video(0, 32)}, false, Epoch());
  REQUIRE(scheduler.TakeReady(Epoch() + 40ms).has_value());
  auto repeated = scheduler.TakeReady(Epoch() + 90ms);
  REQUIRE(repeated.has_value());
  CHECK(repeated->frame->pts == 1);
  CHECK(repeated->frame->data[0][0] == 32);
  scheduler.Push({1, Video(50, 235)}, false, Epoch() + 91ms);
  scheduler.Push({1, Video(100, 64)}, false, Epoch() + 101ms);
  auto current = scheduler.TakeReady(Epoch() + 140ms);
  REQUIRE(current.has_value());
  CHECK(current->frame->pts == 2);
  CHECK(current->frame->data[0][0] == 64);
  CHECK(scheduler.queue_depth() == 0);
  CHECK_FALSE(scheduler.TakeReady(Epoch() + 140ms).has_value());
  REQUIRE(scheduler.deadline().has_value());
  CHECK(*scheduler.deadline() == Epoch() + 190ms);
}

TEST_CASE("RealtimeFrameScheduler zero arrival budget retains immediate output",
          "[arrival_buffer]") {
  auto config = Config();
  config.max_frame_lateness = 0ms;
  RealtimeFrameScheduler scheduler(config);
  scheduler.Configure(Streams(false));
  scheduler.Push({1, Video(0, 32)}, false, Epoch());
  REQUIRE(scheduler.TakeReady(Epoch()).has_value());
  CHECK_FALSE(scheduler.TakeReady(Epoch() + 50ms - 1us).has_value());
  auto repeated = scheduler.TakeReady(Epoch() + 50ms);
  REQUIRE(repeated.has_value());
  CHECK(repeated->frame->data[0][0] == 32);
  scheduler.Push({1, Video(50, 235)}, false, Epoch() + 51ms);
  scheduler.Push({1, Video(100, 64)}, false, Epoch() + 100ms);
  auto current = scheduler.TakeReady(Epoch() + 100ms);
  REQUIRE(current.has_value());
  CHECK(current->frame->pts == 2);
  CHECK(current->frame->data[0][0] == 64);
}
