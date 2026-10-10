#include "mw/streamer/processor/batch_processor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <exception>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace {

using mw::streamer::BatchProcessor;
namespace ffmpeg = mw::streamer::ffmpeg;

static_assert(std::is_nothrow_default_constructible_v<BatchProcessor>);
static_assert(std::is_nothrow_destructible_v<BatchProcessor>);
static_assert(noexcept(std::declval<BatchProcessor&>().SetOnReady({})));
static_assert(noexcept(std::declval<BatchProcessor&>().SetOnVideo({})));
static_assert(noexcept(std::declval<BatchProcessor&>().SetOnAudio({})));
static_assert(noexcept(std::declval<BatchProcessor&>().SetOnEnded({})));
static_assert(noexcept(std::declval<BatchProcessor&>().SetOnStop({})));
static_assert(noexcept(std::declval<BatchProcessor&>().Start(
    std::declval<const std::vector<ffmpeg::StreamInfo>&>(),
    std::declval<const ffmpeg::HwDeviceContext&>())));
static_assert(noexcept(std::declval<BatchProcessor&>().ProcessVideo(
    std::declval<const ffmpeg::Frame&>())));
static_assert(noexcept(std::declval<BatchProcessor&>().ProcessAudio(
    std::declval<const ffmpeg::Frame&>())));
static_assert(noexcept(std::declval<BatchProcessor&>().End()));
static_assert(noexcept(std::declval<BatchProcessor&>().Stop()));

std::vector<ffmpeg::StreamInfo> Streams() {
  std::vector<ffmpeg::StreamInfo> streams(2);
  auto& video = streams[0];
  video.stream_index = 3;
  video.time_base = {1, 90000};
  auto* parameters = video.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->width = 1920;
  parameters->height = 1080;
  auto& audio = streams[1];
  audio.stream_index = 7;
  audio.time_base = {1, 48000};
  parameters = audio.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_AAC;
  parameters->sample_rate = 48000;
  av_channel_layout_default(&parameters->ch_layout, 2);
  return streams;
}

AVBufferRef* TrackedDevice(int& releases) {
  auto* data = static_cast<uint8_t*>(av_mallocz(sizeof(AVHWDeviceContext)));
  if (!data) throw std::bad_alloc();
  auto* reference = av_buffer_create(
      data, sizeof(AVHWDeviceContext),
      [](void* opaque, uint8_t* buffer) {
        ++*static_cast<int*>(opaque);
        av_free(buffer);
      },
      &releases, 0);
  if (!reference) {
    av_free(data);
    throw std::bad_alloc();
  }
  return reference;
}

TEST_CASE("BatchProcessor borrows original stream information and device",
          "[processor][batch][lifecycle]") {
  const bool accepted = GENERATE(false, true);
  int releases = 0, ready = 0, ended = 0, stopped = 0;
  bool borrowed = false;
  const auto streams = Streams();
  BatchProcessor processor;
  processor.SetOnEnded([&] { ++ended; });
  processor.SetOnStop([&] { ++stopped; });
  {
    ffmpeg::HwDeviceContext device(TrackedDevice(releases));
    processor.SetOnReady(
        [&](const auto& supplied, const auto& supplied_device) {
          ++ready;
          borrowed = &supplied == &streams && &supplied_device == &device &&
                     av_buffer_get_ref_count(device.get()) == 1;
          return accepted;
        });
    CHECK(processor.Start(streams, device) == accepted);
    CHECK(av_buffer_get_ref_count(device.get()) == 1);
  }
  CHECK(borrowed);
  CHECK(releases == 1);
  CHECK(ready == 1);
  processor.End();
  processor.End();
  processor.Stop();
  processor.Stop();
  CHECK(ended == (accepted ? 1 : 0));
  CHECK(stopped == (accepted ? 1 : 0));
}

TEST_CASE("BatchProcessor synchronously consumes unchanged frames",
          "[processor][batch][frame]") {
  BatchProcessor processor;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  ffmpeg::Frame video;
  video->format = AV_PIX_FMT_GRAY8;
  video->width = video->height = 8;
  video->pts = -123;
  video->duration = 333;
  video->best_effort_timestamp = 789;
  video->time_base = {1, 10000};
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(video.get(), 0),
                                        "allocate batch video");
  video->data[0][0] = 42;
  ffmpeg::Frame audio;
  audio->format = AV_SAMPLE_FMT_S16;
  audio->sample_rate = 32000;
  audio->nb_samples = 1500;
  av_channel_layout_default(&audio->ch_layout, 2);
  audio->pts = 567;
  audio->duration = 99;
  audio->time_base = {1, 1000};
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(audio.get(), 0),
                                        "allocate batch audio");
  audio->data[0][0] = 17;
  const auto* video_buffer = video->buf[0]->buffer;
  const auto* audio_buffer = audio->buf[0]->buffer;
  const auto caller = std::this_thread::get_id();
  int videos = 0, audios = 0;
  bool synchronous = true;
  ffmpeg::Frame saved_video, saved_audio;
  std::exception_ptr error;
  processor.SetOnVideo([&](const ffmpeg::Frame& frame) noexcept {
    ++videos;
    synchronous &= &frame == &video && std::this_thread::get_id() == caller;
    try {
      saved_video = frame.Ref();
    } catch (...) {
      error = std::current_exception();
    }
  });
  processor.SetOnAudio([&](const ffmpeg::Frame& frame) noexcept {
    ++audios;
    synchronous &= &frame == &audio && std::this_thread::get_id() == caller;
    try {
      saved_audio = frame.Ref();
    } catch (...) {
      error = std::current_exception();
    }
  });
  REQUIRE(processor.Start(Streams(), cpu));
  processor.ProcessVideo(video);
  processor.ProcessAudio(audio);
  REQUIRE_FALSE(error);
  CHECK(videos == 1);
  CHECK(audios == 1);
  CHECK(synchronous);
  CHECK(saved_video.get() != video.get());
  CHECK(saved_audio.get() != audio.get());
  CHECK(saved_video->buf[0]->buffer == video_buffer);
  CHECK(saved_audio->buf[0]->buffer == audio_buffer);
  video.Unref();
  audio.Unref();
  processor.Stop();
  CHECK(saved_video->data[0][0] == 42);
  CHECK(saved_video->pts == -123);
  CHECK(saved_video->duration == 333);
  CHECK(saved_video->best_effort_timestamp == 789);
  CHECK(saved_video->time_base.den == 10000);
  CHECK(saved_video->format == AV_PIX_FMT_GRAY8);
  CHECK(saved_audio->data[0][0] == 17);
  CHECK(saved_audio->pts == 567);
  CHECK(saved_audio->duration == 99);
  CHECK(saved_audio->time_base.den == 1000);
  CHECK(saved_audio->sample_rate == 32000);
  CHECK(saved_audio->nb_samples == 1500);
  CHECK(saved_audio->format == AV_SAMPLE_FMT_S16);
}

TEST_CASE(
    "BatchProcessor empty consumers skip frames without retaining buffers",
    "[processor][batch][frame]") {
  BatchProcessor processor;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  REQUIRE(processor.Start(Streams(), cpu));
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = frame->height = 8;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "allocate skipped batch frame");
  processor.ProcessVideo(frame);
  processor.ProcessAudio(frame);
  CHECK(av_buffer_get_ref_count(frame->buf[0]) == 1);
}

TEST_CASE("BatchProcessor lifecycle is idempotent and retains callbacks",
          "[processor][batch][lifecycle]") {
  int ready = 0, videos = 0, ended = 0, stopped = 0;
  std::thread::id stop_thread;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  {
    BatchProcessor processor;
    processor.SetOnReady([&](const auto&, const auto&) {
      ++ready;
      return true;
    });
    processor.SetOnVideo([&](const auto&) { ++videos; });
    processor.SetOnEnded([&] { ++ended; });
    processor.SetOnStop([&] {
      ++stopped;
      stop_thread = std::this_thread::get_id();
    });
    processor.End();
    processor.Stop();
    CHECK(ended == 0);
    CHECK(stopped == 0);
    for (int session = 1; session <= 2; ++session) {
      REQUIRE(processor.Start(Streams(), cpu));
      processor.ProcessVideo(ffmpeg::Frame{});
      processor.End();
      processor.End();
      processor.Stop();
      processor.Stop();
      processor.End();
      CHECK(ready == session);
      CHECK(videos == session);
      CHECK(ended == session);
      CHECK(stopped == session);
    }
    REQUIRE(processor.Start(Streams(), cpu));
  }
  CHECK(ready == 3);
  CHECK(ended == 2);
  CHECK(stopped == 3);
  CHECK(stop_thread == std::this_thread::get_id());
}

}  // namespace
