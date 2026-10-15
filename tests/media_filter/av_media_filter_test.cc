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

AvDecoderReadResult Input(ffmpeg::Frame frame, std::uint64_t generation = 1,
                          std::int64_t pts = 12000) {
  frame->pts = pts;
  frame->pkt_dts = pts - 2;
  frame->best_effort_timestamp = pts + 1;
  frame->time_base = AVRational{1, 48000};
  frame->duration = 128;
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  result.generation = generation;
  result.stream_index = 7;
  result.time_base = AVRational{1, 90000};
  result.end_pts = 98765;
  return result;
}

void CheckTiming(const AvDecoderReadResult& output,
                 std::uint64_t generation = 1, std::int64_t pts = 12000) {
  REQUIRE(output.status == AvDecoderReadStatus::kFrame);
  REQUIRE(output.generation == generation);
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

TEST_CASE("Generation reset is shared across tracks and precedes new content",
          "[media_filter]") {
  std::vector<std::string> calls;
  AvMediaFilter filter;
  filter.SetOnStart([&] { calls.emplace_back("start"); });
  filter.SetOnStop([&] { calls.emplace_back("stop"); });
  filter.SetOnReset([&] { calls.emplace_back("reset"); });
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("video");
    return frame.Ref();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("audio");
    return frame.Ref();
  });
  filter.Start();
  filter.ProcessVideo(Input(Video(), 4));
  filter.ProcessAudio(Input(Audio(), 4));
  filter.ProcessAudio(Input(Audio(), 5));
  filter.ProcessVideo(Input(Video(), 5));
  REQUIRE_THROWS_AS(filter.ProcessAudio(Input(Audio(), 4)),
                    std::invalid_argument);
  filter.Stop();
  filter.Stop();
  filter.Start();
  filter.ProcessVideo(Input(Video(), 1));
  filter.Stop();
  REQUIRE(calls == std::vector<std::string>{"start", "video", "audio", "reset",
                                            "audio", "video", "stop", "start",
                                            "video", "stop"});
}

TEST_CASE("PTS jumps do not reset or repair business-filter timestamps",
          "[media_filter]") {
  int resets = 0;
  AvMediaFilter filter;
  filter.SetOnReset([&] { ++resets; });
  filter.Start();
  for (const std::int64_t pts : {12000, 90000000, -300000, 100}) {
    auto output = filter.ProcessVideo(Input(Video(), 1, pts));
    CheckTiming(output, 1, pts);
  }
  REQUIRE(resets == 0);
}

TEST_CASE("EOF and underflow pass through without resetting or stopping",
          "[media_filter]") {
  int resets = 0;
  int stops = 0;
  AvMediaFilter filter;
  filter.SetOnReset([&] { ++resets; });
  filter.SetOnStop([&] { ++stops; });
  filter.Start();
  filter.ProcessVideo(Input(Video(), 1));
  for (auto status : {AvDecoderReadStatus::kEof, AvDecoderReadStatus::kNull}) {
    AvDecoderReadResult input;
    input.status = status;
    input.generation = 200;
    input.stream_index = 3;
    input.end_pts = 600;
    input.time_base = AVRational{1, 25};
    auto output = filter.ProcessAudio(std::move(input));
    REQUIRE(output.status == status);
    REQUIRE(output.generation == 200);
    REQUIRE(output.stream_index == 3);
    REQUIRE(output.end_pts == 600);
    REQUIRE(output.time_base.den == 25);
    REQUIRE_FALSE(output.frame.has_value());
  }
  auto output = filter.ProcessVideo(Input(Video(), 1));
  CheckTiming(output);
  REQUIRE(resets == 0);
  REQUIRE(stops == 0);
  filter.Stop();
  REQUIRE(stops == 1);
}

TEST_CASE("Callback failures require a fresh filter session",
          "[media_filter]") {
  int stops = 0;
  AvMediaFilter filter;
  filter.SetOnStop([&] { ++stops; });
  SECTION("Content callback fails") {
    filter.SetOnVideo([](const ffmpeg::Frame&) -> ffmpeg::Frame {
      throw std::runtime_error("Business processing failed");
    });
    filter.Start();
    REQUIRE_THROWS_AS(filter.ProcessVideo(Input(Video())), std::runtime_error);
  }
  SECTION("Reset callback fails") {
    filter.SetOnReset([] { throw std::runtime_error("Reset failed"); });
    filter.Start();
    filter.ProcessVideo(Input(Video(), 1));
    REQUIRE_THROWS_AS(filter.ProcessAudio(Input(Audio(), 2)),
                      std::runtime_error);
  }
  REQUIRE_THROWS_AS(filter.ProcessAudio(Input(Audio(), 2)), std::logic_error);
  filter.Stop();
  REQUIRE(stops == 1);
  filter.SetOnVideo({});
  filter.SetOnReset({});
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

TEST_CASE("Slow business processing serializes Reset and Stop",
          "[media_filter]") {
  bool reset = false;
  SECTION("Reset waits for the old generation") { reset = true; }
  SECTION("Stop waits for processing") { reset = false; }

  std::promise<void> entered;
  std::atomic<bool> signaled = false;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::vector<std::string> calls;
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("video begin");
    signaled = true;
    entered.set_value();
    released.wait();
    calls.emplace_back("video end");
    return frame.Ref();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("audio");
    return frame.Ref();
  });
  filter.SetOnReset([&] { calls.emplace_back("reset"); });
  filter.SetOnStop([&] { calls.emplace_back("stop"); });
  filter.Start();
  std::exception_ptr processing_error;
  std::thread processing([&] {
    try {
      filter.ProcessVideo(Input(Video(), 1));
    } catch (...) {
      processing_error = std::current_exception();
      if (!signaled.exchange(true)) entered.set_value();
    }
  });
  entered.get_future().wait();
  std::promise<void> attempting;
  std::promise<void> completed;
  auto completion = completed.get_future();
  std::exception_ptr control_error;
  std::thread control([&] {
    attempting.set_value();
    try {
      if (reset) {
        filter.ProcessAudio(Input(Audio(), 2));
      } else {
        filter.Stop();
      }
    } catch (...) {
      control_error = std::current_exception();
    }
    completed.set_value();
  });
  attempting.get_future().wait();
  const auto while_blocked = completion.wait_for(40ms);
  release.set_value();
  processing.join();
  control.join();
  REQUIRE(while_blocked == std::future_status::timeout);
  REQUIRE_FALSE(processing_error);
  REQUIRE_FALSE(control_error);
  if (reset) {
    REQUIRE(calls == std::vector<std::string>{"video begin", "video end",
                                              "reset", "audio"});
  } else {
    REQUIRE(calls ==
            std::vector<std::string>{"video begin", "video end", "stop"});
  }
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
  int resets = 0;
  filter.SetOnVideo([](const ffmpeg::Frame& input) { return input.Ref(); });
  filter.SetOnReset([&] { ++resets; });
  filter.Start();
  auto output = filter.ProcessVideo(Input(std::move(frame)));
  CheckTiming(output);
  REQUIRE((*output.frame)->format == AV_PIX_FMT_CUDA);
  REQUIRE((*output.frame)->data[0] == surface);
  REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
  output.generation = 2;
  output = filter.ProcessVideo(std::move(output));
  REQUIRE(resets == 1);
  filter.Stop();
  CheckTiming(output, 2);
  REQUIRE((*output.frame)->data[0] == surface);
  REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
}

}  // namespace
