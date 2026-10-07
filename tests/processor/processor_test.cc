#include "mw/streamer/processor/processor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <new>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace {

using mw::streamer::Processor;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};

std::vector<ffmpeg::StreamInfo> Streams() {
  std::vector<ffmpeg::StreamInfo> streams;
  ffmpeg::StreamInfo video;
  video.stream_index = 0;
  video.time_base = kNanoseconds;
  auto* parameters = video.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
  parameters->format = AV_PIX_FMT_GRAY8;
  parameters->width = parameters->height = 8;
  streams.push_back(std::move(video));
  ffmpeg::StreamInfo audio;
  audio.stream_index = 1;
  audio.time_base = kNanoseconds;
  parameters = audio.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_PCM_F32LE;
  parameters->format = AV_SAMPLE_FMT_FLTP;
  parameters->sample_rate = 48000;
  av_channel_layout_default(&parameters->ch_layout, 2);
  streams.push_back(std::move(audio));
  return streams;
}

ffmpeg::Frame Video(int64_t pts, AVPixelFormat format = AV_PIX_FMT_GRAY8,
                    int width = 8, int height = 8) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->width = width;
  frame->height = height;
  frame->pts = pts;
  frame->duration = 333;
  frame->best_effort_timestamp = 789;
  frame->time_base = {1, 10000};
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配纯Processor视频帧");
  frame->data[0][0] = 42;
  return frame;
}

ffmpeg::Frame Audio(int samples, int64_t pts, int rate = 48000,
                    AVSampleFormat format = AV_SAMPLE_FMT_FLTP) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->sample_rate = rate;
  frame->nb_samples = samples;
  av_channel_layout_default(&frame->ch_layout, 2);
  frame->pts = pts;
  frame->duration = 99;
  frame->time_base = {1, 1000};
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配纯Processor音频帧");
  frame->data[0][0] = 17;
  return frame;
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

TEST_CASE("纯Processor遵循Ready返回值且不持有设备引用",
          "[processor][lifecycle]") {
  const bool accepted = GENERATE(false, true);
  int releases = 0, ready = 0, ended = 0, stopped = 0;
  bool borrowed = false;
  Processor processor;
  processor.SetOnEnded([&] { ++ended; });
  processor.SetOnStop([&] { ++stopped; });
  {
    ffmpeg::HwDeviceContext device(TrackedDevice(releases));
    const auto* expected = device.get();
    processor.SetOnReady([&](const auto& streams, const auto& supplied) {
      ++ready;
      borrowed = streams.size() == 2 && supplied.get() == expected &&
                 av_buffer_get_ref_count(supplied.get()) == 1;
      return accepted;
    });
    CHECK(processor.Start(Streams(), device) == accepted);
    CHECK(av_buffer_get_ref_count(device.get()) == 1);
  }
  CHECK(releases == 1);
  CHECK(ready == 1);
  CHECK(borrowed);
  processor.End();
  processor.End();
  processor.Stop();
  processor.Stop();
  CHECK(ended == (accepted ? 1 : 0));
  CHECK(stopped == (accepted ? 1 : 0));
}

TEST_CASE("纯Processor默认直接Ref音视频且保留格式样本与时间戳",
          "[processor][frame]") {
  Processor processor;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  REQUIRE(processor.Start(Streams(), cpu));
  auto video = Video(-123);
  auto audio = Audio(1500, 567, 32000, AV_SAMPLE_FMT_S16);
  const auto* video_buffer = video->buf[0]->buffer;
  const auto* audio_buffer = audio->buf[0]->buffer;
  auto processed_video = processor.ProcessVideo(video);
  auto processed_audio = processor.ProcessAudio(audio);
  CHECK(processed_video.get() != video.get());
  CHECK(processed_audio.get() != audio.get());
  CHECK(processed_video->buf[0]->buffer == video_buffer);
  CHECK(processed_audio->buf[0]->buffer == audio_buffer);
  video.Unref();
  audio.Unref();
  processor.Stop();
  CHECK(processed_video->data[0][0] == 42);
  CHECK(processed_video->format == AV_PIX_FMT_GRAY8);
  CHECK(processed_video->width == 8);
  CHECK(processed_video->height == 8);
  CHECK(processed_video->pts == -123);
  CHECK(processed_video->duration == 333);
  CHECK(processed_video->best_effort_timestamp == 789);
  CHECK(processed_video->time_base.num == 1);
  CHECK(processed_video->time_base.den == 10000);
  CHECK(processed_audio->data[0][0] == 17);
  CHECK(processed_audio->format == AV_SAMPLE_FMT_S16);
  CHECK(processed_audio->sample_rate == 32000);
  CHECK(processed_audio->nb_samples == 1500);
  CHECK(processed_audio->pts == 567);
  CHECK(processed_audio->duration == 99);
  CHECK(processed_audio->time_base.num == 1);
  CHECK(processed_audio->time_base.den == 1000);
}

TEST_CASE("纯Processor同步转交自定义帧且不重采样或重写PTS",
          "[processor][filter]") {
  Processor processor;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  const auto caller = std::this_thread::get_id();
  int videos = 0, audios = 0;
  bool same_thread = true;
  processor.SetOnVideo([&](const ffmpeg::Frame&) {
    ++videos;
    same_thread &= std::this_thread::get_id() == caller;
    return Video(444, AV_PIX_FMT_RGB24, 16, 4);
  });
  processor.SetOnAudio([&](const ffmpeg::Frame&) {
    ++audios;
    same_thread &= std::this_thread::get_id() == caller;
    return Audio(1500, -987, 22050, AV_SAMPLE_FMT_S16);
  });
  REQUIRE(processor.Start(Streams(), cpu));
  auto video = processor.ProcessVideo(Video(123));
  auto audio = processor.ProcessAudio(Audio(1024, 123));
  CHECK(videos == 1);
  CHECK(audios == 1);
  CHECK(same_thread);
  CHECK(video->format == AV_PIX_FMT_RGB24);
  CHECK(video->width == 16);
  CHECK(video->height == 4);
  CHECK(video->pts == 444);
  CHECK(video->duration == 333);
  CHECK(video->time_base.den == 10000);
  CHECK(audio->format == AV_SAMPLE_FMT_S16);
  CHECK(audio->sample_rate == 22050);
  CHECK(audio->nb_samples == 1500);
  CHECK(audio->pts == -987);
  CHECK(audio->duration == 99);
  CHECK(audio->time_base.den == 1000);
  processor.Stop();
}

TEST_CASE("纯Processor的End与Stop幂等且重新初始化保留回调",
          "[processor][lifecycle]") {
  int ready = 0, ended = 0, stopped = 0, videos = 0;
  std::thread::id stop_thread;
  Processor processor;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  processor.SetOnReady([&](const auto&, const auto&) {
    ++ready;
    return true;
  });
  processor.SetOnVideo([&](const ffmpeg::Frame& frame) {
    ++videos;
    return frame.Ref();
  });
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
    const auto frame = processor.ProcessVideo(Video(session));
    CHECK(frame->pts == session);
    processor.End();
    processor.End();
    processor.Stop();
    processor.Stop();
    processor.End();
    CHECK(ready == session);
    CHECK(videos == session);
    CHECK(ended == session);
    CHECK(stopped == session);
    CHECK(stop_thread == std::this_thread::get_id());
  }
}

}  // namespace
