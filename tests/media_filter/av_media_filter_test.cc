#include "mw/streamer/filter/av_media_filter.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::AvMediaFilter;
namespace ffmpeg = mw::streamer::ffmpeg;

ffmpeg::Frame Video(std::uint8_t pixel = 12) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = 8;
  frame->height = 8;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate video frame");
  }
  frame->data[0][0] = pixel;
  return frame;
}

ffmpeg::Frame Audio(std::int16_t sample = 34) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_S16;
  frame->sample_rate = 48000;
  frame->nb_samples = 128;
  av_channel_layout_default(&frame->ch_layout, 1);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate audio frame");
  }
  reinterpret_cast<std::int16_t*>(frame->data[0])[0] = sample;
  return frame;
}

AvDecoderReadResult Input(ffmpeg::Frame frame, std::int64_t pts = 12000) {
  frame->pts = pts;
  frame->pkt_dts = pts - 2;
  frame->best_effort_timestamp = pts + 1;
  frame->time_base = AVRational{1, 48000};
  frame->duration = 128;
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  result.stream_index = 7;
  result.time_base = AVRational{1, 90000};
  result.end_pts = 98765;
  return result;
}

void CheckTiming(const AvDecoderReadResult& output, std::int64_t pts = 12000) {
  REQUIRE(output.status == AvDecoderReadStatus::kFrame);
  REQUIRE(output.stream_index == 7);
  REQUIRE(output.time_base.num == 1);
  REQUIRE(output.time_base.den == 90000);
  REQUIRE(output.end_pts == 98765);
  REQUIRE(output.frame.has_value());
  REQUIRE((*output.frame)->pts == pts);
  REQUIRE((*output.frame)->pkt_dts == pts - 2);
  REQUIRE((*output.frame)->best_effort_timestamp == pts + 1);
  REQUIRE((*output.frame)->time_base.num == 1);
  REQUIRE((*output.frame)->time_base.den == 48000);
  REQUIRE((*output.frame)->duration == 128);
}

TEST_CASE("Business filters change content while preserving source timing",
          "[media_filter]") {
  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& frame) {
    return Video(static_cast<std::uint8_t>(frame->data[0][0] + 3));
  });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) {
    return Audio(reinterpret_cast<const std::int16_t*>(frame->data[0])[0] * 2);
  });
  filter.Start();
  auto video = filter.ProcessVideo(Input(Video()));
  auto audio = filter.ProcessAudio(Input(Audio()));
  CheckTiming(video);
  CheckTiming(audio);
  REQUIRE((*video.frame)->data[0][0] == 15);
  REQUIRE(reinterpret_cast<const std::int16_t*>((*audio.frame)->data[0])[0] ==
          68);
}

TEST_CASE("Missing business callbacks pass through the original frames",
          "[media_filter]") {
  AvMediaFilter filter;
  filter.Start();
  auto video = Input(Video());
  auto audio = Input(Audio());
  auto* video_frame = video.frame->get();
  auto* audio_frame = audio.frame->get();
  auto video_output = filter.ProcessVideo(std::move(video));
  auto audio_output = filter.ProcessAudio(std::move(audio));
  REQUIRE(video_output.frame->get() == video_frame);
  REQUIRE(audio_output.frame->get() == audio_frame);
  CheckTiming(video_output);
  CheckTiming(audio_output);
}

TEST_CASE("Interleaved tracks retain business state until Stop and Start",
          "[media_filter]") {
  std::vector<std::string> calls;
  AvMediaFilter filter;
  filter.SetOnStart([&] { calls.emplace_back("start"); });
  filter.SetOnStop([&] { calls.emplace_back("stop"); });
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("video");
    return frame.Ref();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("audio");
    return frame.Ref();
  });
  filter.Start();
  filter.ProcessVideo(Input(Video()));
  filter.ProcessAudio(Input(Audio()));
  filter.ProcessAudio(Input(Audio()));
  filter.ProcessVideo(Input(Video()));
  REQUIRE_NOTHROW(filter.ProcessAudio(Input(Audio())));
  filter.Stop();
  filter.Stop();
  filter.Start();
  filter.ProcessVideo(Input(Video()));
  filter.Stop();
  REQUIRE(calls == std::vector<std::string>{"start", "video", "audio", "audio",
                                            "video", "audio", "stop", "start",
                                            "video", "stop"});
}

TEST_CASE("PTS jumps do not reset or repair business-filter timestamps",
          "[media_filter]") {
  AvMediaFilter filter;
  int processed = 0;
  filter.SetOnVideo([&](const ffmpeg::Frame&) {
    return Video(static_cast<std::uint8_t>(++processed));
  });
  filter.Start();
  for (const std::int64_t pts : {12000, 90000000, -300000, 100}) {
    auto output = filter.ProcessVideo(Input(Video(), pts));
    CheckTiming(output, pts);
    REQUIRE((*output.frame)->data[0][0] == processed);
  }
  REQUIRE(processed == 4);
}

TEST_CASE("EOF and underflow pass through without resetting or stopping",
          "[media_filter]") {
  int stops = 0;
  AvMediaFilter filter;
  filter.SetOnStop([&] { ++stops; });
  filter.Start();
  filter.ProcessVideo(Input(Video()));
  for (auto status : {AvDecoderReadStatus::kEof, AvDecoderReadStatus::kNull}) {
    AvDecoderReadResult input;
    input.status = status;
    input.stream_index = 3;
    input.end_pts = 600;
    input.time_base = AVRational{1, 25};
    auto output = filter.ProcessAudio(std::move(input));
    REQUIRE(output.status == status);
    REQUIRE(output.stream_index == 3);
    REQUIRE(output.end_pts == 600);
    REQUIRE(output.time_base.den == 25);
    REQUIRE_FALSE(output.frame.has_value());
  }
  auto output = filter.ProcessVideo(Input(Video()));
  CheckTiming(output);
  REQUIRE(stops == 0);
  filter.Stop();
  REQUIRE(stops == 1);
}

TEST_CASE("Callback failures require a fresh filter session",
          "[media_filter]") {
  int stops = 0;
  AvMediaFilter filter;
  filter.SetOnStop([&] { ++stops; });
  filter.SetOnVideo([](const ffmpeg::Frame&) -> ffmpeg::Frame {
    throw std::runtime_error("Business processing failed");
  });
  filter.Start();
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input(Video())), std::runtime_error);
  REQUIRE_THROWS_AS(filter.ProcessAudio(Input(Audio())), std::logic_error);
  filter.Stop();
  REQUIRE(stops == 1);
  filter.SetOnVideo({});
  filter.Start();
  auto output = filter.ProcessVideo(Input(Video()));
  CheckTiming(output);
}

TEST_CASE("Session callbacks have explicit lifecycle semantics",
          "[media_filter]") {
  AvMediaFilter filter;
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input(Video())), std::logic_error);
  filter.SetOnStart([] { throw std::runtime_error("Start failed"); });
  REQUIRE_THROWS_AS(filter.Start(), std::runtime_error);
  filter.SetOnStart({});
  filter.Start();
  REQUIRE_THROWS_AS(filter.Start(), std::logic_error);
  REQUIRE_THROWS_AS(filter.SetOnVideo({}), std::logic_error);
  filter.Stop();

  int stops = 0;
  filter.SetOnStop([&] {
    ++stops;
    throw std::runtime_error("Stop failed");
  });
  filter.Start();
  REQUIRE_THROWS_AS(filter.Stop(), std::runtime_error);
  REQUIRE_NOTHROW(filter.Stop());
  REQUIRE(stops == 1);
}

TEST_CASE("Destruction stops an active session even when Stop throws",
          "[media_filter]") {
  int stops = 0;
  {
    AvMediaFilter filter;
    filter.SetOnStop([&] {
      ++stops;
      throw std::runtime_error("Stop failed");
    });
    filter.Start();
  }
  REQUIRE(stops == 1);
}

TEST_CASE("Audio processing completes while the video callback is blocked",
          "[media_filter]") {
  std::promise<void> video_entered;
  auto entered = video_entered.get_future();
  std::promise<void> release_video;
  auto released = release_video.get_future().share();
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    video_entered.set_value();
    released.wait();
    return frame.Ref();
  });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.Start();
  std::exception_ptr video_error;
  std::thread video([&] {
    try {
      filter.ProcessVideo(Input(Video()));
    } catch (...) {
      video_error = std::current_exception();
    }
  });
  const auto video_started = entered.wait_for(2s);
  std::promise<void> audio_completed;
  auto audio_completion = audio_completed.get_future();
  std::exception_ptr audio_error;
  std::thread audio([&] {
    try {
      filter.ProcessAudio(Input(Audio()));
    } catch (...) {
      audio_error = std::current_exception();
    }
    audio_completed.set_value();
  });
  const auto audio_while_video_blocked = audio_completion.wait_for(2s);
  release_video.set_value();
  video.join();
  audio.join();
  REQUIRE(video_started == std::future_status::ready);
  REQUIRE(audio_while_video_blocked == std::future_status::ready);
  REQUIRE_FALSE(video_error);
  REQUIRE_FALSE(audio_error);
}

TEST_CASE("Stop waits for both concurrent callbacks before OnStop",
          "[media_filter]") {
  std::promise<void> video_entered;
  std::promise<void> audio_entered;
  auto video_entry = video_entered.get_future();
  auto audio_entry = audio_entered.get_future();
  std::promise<void> release_video;
  std::promise<void> release_audio;
  auto video_release = release_video.get_future().share();
  auto audio_release = release_audio.get_future().share();
  std::atomic<int> completed_callbacks = 0;
  int callbacks_when_stopped = -1;
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    video_entered.set_value();
    video_release.wait();
    ++completed_callbacks;
    return frame.Ref();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    audio_entered.set_value();
    audio_release.wait();
    ++completed_callbacks;
    return frame.Ref();
  });
  filter.SetOnStop(
      [&] { callbacks_when_stopped = completed_callbacks.load(); });
  filter.Start();
  std::exception_ptr video_error;
  std::exception_ptr audio_error;
  std::exception_ptr stop_error;
  std::thread video([&] {
    try {
      filter.ProcessVideo(Input(Video()));
    } catch (...) {
      video_error = std::current_exception();
    }
  });
  const auto video_started = video_entry.wait_for(2s);
  std::thread audio([&] {
    try {
      filter.ProcessAudio(Input(Audio()));
    } catch (...) {
      audio_error = std::current_exception();
    }
  });
  const auto audio_started = audio_entry.wait_for(2s);
  std::promise<void> stop_attempted;
  auto stop_attempt = stop_attempted.get_future();
  std::promise<void> stop_completed;
  auto stop_completion = stop_completed.get_future();
  std::thread stop([&] {
    stop_attempted.set_value();
    try {
      filter.Stop();
    } catch (...) {
      stop_error = std::current_exception();
    }
    stop_completed.set_value();
  });
  stop_attempt.wait();
  const auto both_blocked = stop_completion.wait_for(40ms);
  bool new_process_rejected = false;
  std::exception_ptr new_process_error;
  std::promise<void> new_process_completed;
  auto new_process_completion = new_process_completed.get_future();
  std::thread new_process([&] {
    try {
      filter.ProcessAudio(AvDecoderReadResult{});
    } catch (const std::logic_error&) {
      new_process_rejected = true;
    } catch (...) {
      new_process_error = std::current_exception();
    }
    new_process_completed.set_value();
  });
  const auto rejected_while_stopping = new_process_completion.wait_for(2s);
  release_video.set_value();
  video.join();
  const auto audio_still_blocked = stop_completion.wait_for(40ms);
  release_audio.set_value();
  audio.join();
  stop.join();
  new_process.join();
  REQUIRE(video_started == std::future_status::ready);
  REQUIRE(audio_started == std::future_status::ready);
  REQUIRE(both_blocked == std::future_status::timeout);
  REQUIRE(audio_still_blocked == std::future_status::timeout);
  REQUIRE(rejected_while_stopping == std::future_status::ready);
  REQUIRE(new_process_rejected);
  REQUIRE_FALSE(new_process_error);
  REQUIRE_FALSE(video_error);
  REQUIRE_FALSE(audio_error);
  REQUIRE_FALSE(stop_error);
  REQUIRE(callbacks_when_stopped == 2);
}

TEST_CASE("A failing callback leaves the concurrent callback able to finish",
          "[media_filter]") {
  std::promise<void> audio_entered;
  auto audio_entry = audio_entered.get_future();
  std::promise<void> release_audio;
  auto audio_release = release_audio.get_future().share();
  AvMediaFilter filter;
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    audio_entered.set_value();
    audio_release.wait();
    return frame.Ref();
  });
  filter.SetOnVideo([](const ffmpeg::Frame&) -> ffmpeg::Frame {
    throw std::runtime_error("Video callback failed");
  });
  filter.Start();
  std::exception_ptr audio_error;
  std::exception_ptr video_error;
  std::exception_ptr stop_error;
  std::thread audio([&] {
    try {
      filter.ProcessAudio(Input(Audio()));
    } catch (...) {
      audio_error = std::current_exception();
    }
  });
  const auto audio_started = audio_entry.wait_for(2s);
  std::promise<void> video_completed;
  auto video_completion = video_completed.get_future();
  std::thread video([&] {
    try {
      filter.ProcessVideo(Input(Video()));
    } catch (...) {
      video_error = std::current_exception();
    }
    video_completed.set_value();
  });
  const auto video_failed_while_audio_blocked = video_completion.wait_for(2s);
  std::promise<void> stop_completed;
  auto stop_completion = stop_completed.get_future();
  std::thread stop([&] {
    try {
      filter.Stop();
    } catch (...) {
      stop_error = std::current_exception();
    }
    stop_completed.set_value();
  });
  const auto stop_while_audio_blocked = stop_completion.wait_for(40ms);
  release_audio.set_value();
  audio.join();
  video.join();
  stop.join();
  REQUIRE(audio_started == std::future_status::ready);
  REQUIRE(video_failed_while_audio_blocked == std::future_status::ready);
  REQUIRE(stop_while_audio_blocked == std::future_status::timeout);
  REQUIRE_FALSE(audio_error);
  REQUIRE(video_error);
  REQUIRE_FALSE(stop_error);
  filter.SetOnAudio({});
  REQUIRE_NOTHROW(filter.Start());
  REQUIRE_NOTHROW(filter.ProcessAudio(Input(Audio())));
}

TEST_CASE("CUDA business filters retain hardware surfaces and input timing",
          "[.][media_filter][cuda]") {
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
  ffmpeg::Frame frame;
  REQUIRE(av_hwframe_get_buffer(frames.get(), frame.get(), 0) >= 0);
  const auto* surface = frame->data[0];
  const auto* frame_context = frame->hw_frames_ctx->data;

  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& input) { return input.Ref(); });
  filter.Start();
  auto output = filter.ProcessVideo(Input(std::move(frame)));
  CheckTiming(output);
  REQUIRE((*output.frame)->format == AV_PIX_FMT_CUDA);
  REQUIRE((*output.frame)->data[0] == surface);
  REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
  output = filter.ProcessVideo(std::move(output));
  filter.Stop();
  CheckTiming(output);
  REQUIRE((*output.frame)->data[0] == surface);
  REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
}

}  // namespace
