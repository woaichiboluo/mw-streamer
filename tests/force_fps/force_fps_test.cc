#include "mw/streamer/filter/force_fps.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace {

using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::ForceFps;
using mw::streamer::ForceFpsConfig;
namespace ffmpeg = mw::streamer::ffmpeg;

AvDecoderReadResult Video(std::int64_t pts, std::uint8_t pixel = 1,
                          std::uint64_t generation = 1,
                          AVRational time_base = {1, 1000}) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = 8;
  frame->height = 8;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate video frame");
  }
  frame->data[0][0] = pixel;
  frame->pts = pts;
  frame->best_effort_timestamp = pts + 2;
  frame->pkt_dts = pts - 2;
  frame->time_base = time_base;
  frame->duration = 40;
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  result.time_base = time_base;
  result.generation = generation;
  result.stream_index = 3;
  return result;
}

void Check(const AvDecoderReadResult& result, std::int64_t pts,
           std::uint8_t pixel, std::uint64_t generation = 1,
           AVRational time_base = {1, 25}) {
  REQUIRE(result.status == AvDecoderReadStatus::kFrame);
  REQUIRE(result.frame.has_value());
  REQUIRE(result.generation == generation);
  REQUIRE(result.stream_index == 3);
  REQUIRE(result.time_base.num == time_base.num);
  REQUIRE(result.time_base.den == time_base.den);
  REQUIRE((*result.frame)->pts == pts);
  REQUIRE((*result.frame)->duration == 1);
  REQUIRE((*result.frame)->best_effort_timestamp == pts);
  REQUIRE((*result.frame)->pkt_dts == AV_NOPTS_VALUE);
  REQUIRE((*result.frame)->time_base.num == time_base.num);
  REQUIRE((*result.frame)->time_base.den == time_base.den);
  REQUIRE((*result.frame)->data[0][0] == pixel);
}

TEST_CASE("ForceFps lazily repeats pictures on the output frame grid",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
  REQUIRE(fps.TryPushFrame(Video(0, 11)));
  REQUIRE(fps.TryPushFrame(Video(120, 22)));
  Check(fps.TryReadFrame(), 0, 11);
  Check(fps.TryReadFrame(), 1, 11);
  Check(fps.TryReadFrame(), 2, 11);
  Check(fps.TryReadFrame(), 3, 22);
  // A cached picture alone cannot generate an unbounded tail.
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps drops surplus inputs but retains their newest picture",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  REQUIRE(fps.TryPushFrame(Video(10, 2)));
  REQUIRE(fps.TryPushFrame(Video(80, 3)));
  Check(fps.TryReadFrame(), 0, 1);
  Check(fps.TryReadFrame(), 1, 2);
  Check(fps.TryReadFrame(), 2, 3);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps equal source timestamps do not emit duplicate slots",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  REQUIRE(fps.TryPushFrame(Video(0, 2)));
  REQUIRE(fps.TryPushFrame(Video(80, 3)));
  Check(fps.TryReadFrame(), 0, 1);
  Check(fps.TryReadFrame(), 1, 2);
  Check(fps.TryReadFrame(), 2, 3);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps rational frame rates preserve exact grid over long runs",
          "[force-fps]") {
  ForceFps fps(ForceFpsConfig{AVRational{30000, 1001}, 2});
  REQUIRE(fps.time_base().num == 1001);
  REQUIRE(fps.time_base().den == 30000);
  for (std::int64_t i = 0; i < 10000; ++i) {
    REQUIRE(fps.TryPushFrame(Video(i * 3003, 7, 1, AVRational{1, 90000})));
    Check(fps.TryReadFrame(), i, 7, 1, AVRational{1001, 30000});
  }
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps reanchors large forward jumps and timestamp regressions",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(Video(1000, 1)));
  REQUIRE(fps.TryPushFrame(Video(300000, 2)));
  REQUIRE(fps.TryPushFrame(Video(-4000, 3)));
  Check(fps.TryReadFrame(), 25, 1);
  Check(fps.TryReadFrame(), 7500, 2);
  Check(fps.TryReadFrame(), -100, 3);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

// Keep a regression to blocking admission from hanging the whole test runner.
bool PushWithoutWaiting(ForceFps& fps, const AvDecoderReadResult& input) {
  auto pending =
      std::async(std::launch::async, [&] { return fps.TryPushFrame(input); });
  const auto status = pending.wait_for(std::chrono::milliseconds(250));
  if (status != std::future_status::ready) {
    fps.Stop();
  }
  const bool accepted = pending.get();
  REQUIRE(status == std::future_status::ready);
  return accepted;
}

TEST_CASE("ForceFps rejects a full queue without consuming the retry frame",
          "[force-fps]") {
  ForceFps fps(ForceFpsConfig{AVRational{25, 1}, 1});
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  const auto pending = Video(120, 2);
  const auto* header = pending.frame->get();
  const auto* pixels = (*pending.frame)->data[0];
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  REQUIRE(pending.frame->get() == header);
  REQUIRE((*pending.frame)->data[0] == pixels);
  REQUIRE((*pending.frame)->pts == 120);
  REQUIRE((*pending.frame)->pkt_dts == 118);
  REQUIRE((*pending.frame)->best_effort_timestamp == 122);
  REQUIRE((*pending.frame)->duration == 40);
  REQUIRE(pending.generation == 1);
  Check(fps.TryReadFrame(), 0, 1);
  REQUIRE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 1, 1);
  Check(fps.TryReadFrame(), 2, 1);
  Check(fps.TryReadFrame(), 3, 2);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps repeated outputs do not release the expanding input slot",
          "[force-fps]") {
  ForceFps fps(ForceFpsConfig{AVRational{25, 1}, 1});
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  Check(fps.TryReadFrame(), 0, 1);
  const auto gap = Video(120, 2);
  REQUIRE(fps.TryPushFrame(gap));
  const auto pending = Video(160, 3);
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 1, 1);
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 2, 1);
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 3, 2);
  REQUIRE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 4, 3);
  REQUIRE((*gap.frame)->pts == 120);
}

TEST_CASE("ForceFps Stop rejects input and Reset permits retrying it",
          "[force-fps]") {
  ForceFps fps(ForceFpsConfig{AVRational{25, 1}, 1});
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  const auto pending = Video(40, 2);
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  fps.Stop();
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
  REQUIRE((*pending.frame)->pts == 40);
  fps.Reset();
  REQUIRE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 1, 2);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps rejected EOF does not close admission before it is retried",
          "[force-fps]") {
  ForceFps fps(ForceFpsConfig{AVRational{25, 1}, 1});
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  AvDecoderReadResult eof;
  eof.status = AvDecoderReadStatus::kEof;
  eof.generation = 1;
  eof.end_pts = 80;
  eof.time_base = AVRational{1, 1000};
  REQUIRE_FALSE(PushWithoutWaiting(fps, eof));
  REQUIRE(eof.status == AvDecoderReadStatus::kEof);
  REQUIRE(eof.end_pts == 80);
  Check(fps.TryReadFrame(), 0, 1);
  REQUIRE(fps.TryPushFrame(Video(40, 2)));
  Check(fps.TryReadFrame(), 1, 2);
  REQUIRE(PushWithoutWaiting(fps, eof));
  const auto output = fps.TryReadFrame();
  REQUIRE(output.status == AvDecoderReadStatus::kEof);
  REQUIRE(output.generation == 1);
  REQUIRE(output.end_pts == 2);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps rejected new generations do not advance admission state",
          "[force-fps]") {
  ForceFps fps(ForceFpsConfig{AVRational{25, 1}, 1});
  REQUIRE(fps.TryPushFrame(Video(0, 1, 7)));
  auto pending = Video(400, 3, 8);
  SECTION("A new generation frame is rejected") {}
  SECTION("A new generation EOF is rejected") {
    pending.frame.reset();
    pending.status = AvDecoderReadStatus::kEof;
  }
  REQUIRE_FALSE(PushWithoutWaiting(fps, pending));
  Check(fps.TryReadFrame(), 0, 1, 7);
  REQUIRE(fps.TryPushFrame(Video(40, 2, 7)));
  Check(fps.TryReadFrame(), 1, 2, 7);
  REQUIRE(PushWithoutWaiting(fps, pending));
  if (pending.status == AvDecoderReadStatus::kFrame) {
    Check(fps.TryReadFrame(), 10, 3, 8);
  } else {
    const auto output = fps.TryReadFrame();
    REQUIRE(output.status == AvDecoderReadStatus::kEof);
    REQUIRE(output.generation == 8);
  }
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps drains earlier frames before EOF and rescales its endpoint",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  REQUIRE(fps.TryPushFrame(Video(120, 2)));
  AvDecoderReadResult eof;
  eof.status = AvDecoderReadStatus::kEof;
  eof.generation = 1;
  eof.stream_index = 3;
  eof.time_base = AVRational{1, 1000};
  eof.end_pts = 160;
  REQUIRE(fps.TryPushFrame(eof));
  REQUIRE_THROWS_AS(fps.TryPushFrame(Video(160)), std::invalid_argument);
  Check(fps.TryReadFrame(), 0, 1);
  Check(fps.TryReadFrame(), 1, 1);
  Check(fps.TryReadFrame(), 2, 1);
  Check(fps.TryReadFrame(), 3, 2);
  auto output = fps.TryReadFrame();
  REQUIRE(output.status == AvDecoderReadStatus::kEof);
  REQUIRE_FALSE(output.frame.has_value());
  REQUIRE(output.generation == 1);
  REQUIRE(output.stream_index == 3);
  REQUIRE(output.time_base.num == 1);
  REQUIRE(output.time_base.den == 25);
  REQUIRE(output.end_pts == 4);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps changes generations only after queued old frames finish",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(Video(0, 1, 7)));
  REQUIRE(fps.TryPushFrame(Video(120, 2, 7)));
  REQUIRE(fps.TryPushFrame(Video(400, 3, 8)));
  REQUIRE_THROWS_AS(fps.TryPushFrame(Video(440, 4, 7)), std::invalid_argument);
  Check(fps.TryReadFrame(), 0, 1, 7);
  Check(fps.TryReadFrame(), 1, 1, 7);
  Check(fps.TryReadFrame(), 2, 1, 7);
  Check(fps.TryReadFrame(), 3, 2, 7);
  // A new generation must not repeat old-generation pictures to fill the gap.
  Check(fps.TryReadFrame(), 10, 3, 8);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("ForceFps accepts a queued new generation after old EOF",
          "[force-fps]") {
  ForceFps fps;
  AvDecoderReadResult eof;
  eof.status = AvDecoderReadStatus::kEof;
  eof.generation = 1;
  REQUIRE(fps.TryPushFrame(Video(0, 1)));
  REQUIRE(fps.TryPushFrame(eof));
  REQUIRE(fps.TryPushFrame(Video(80, 2, 2)));
  Check(fps.TryReadFrame(), 0, 1);
  const auto output = fps.TryReadFrame();
  REQUIRE(output.status == AvDecoderReadStatus::kEof);
  REQUIRE(output.end_pts == AV_NOPTS_VALUE);
  Check(fps.TryReadFrame(), 2, 2, 2);
}

TEST_CASE("ForceFps Reset discards pending pictures and allows a new session",
          "[force-fps]") {
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(Video(0, 1, 9)));
  Check(fps.TryReadFrame(), 0, 1, 9);
  REQUIRE(fps.TryPushFrame(Video(120, 2, 9)));
  Check(fps.TryReadFrame(), 1, 1, 9);
  fps.Stop();
  REQUIRE_FALSE(fps.TryPushFrame(Video(160, 4, 9)));
  fps.Reset();
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
  REQUIRE(fps.TryPushFrame(Video(400, 3, 0)));
  Check(fps.TryReadFrame(), 10, 3, 0);
}

TEST_CASE(
    "ForceFps timing edits leave input and previous output headers intact",
    "[force-fps]") {
  ForceFps fps;
  const auto first = Video(1000, 7);
  const auto* pixels = (*first.frame)->data[0];
  REQUIRE(fps.TryPushFrame(first));
  const auto returned = fps.TryReadFrame();
  REQUIRE(fps.TryPushFrame(Video(1120, 8)));
  Check(fps.TryReadFrame(), 26, 7);
  Check(fps.TryReadFrame(), 27, 7);
  Check(fps.TryReadFrame(), 28, 8);
  Check(returned, 25, 7);
  REQUIRE((*returned.frame)->data[0] == pixels);
  REQUIRE((*returned.frame).get() != (*first.frame).get());
  REQUIRE((*first.frame)->pts == 1000);
  REQUIRE((*first.frame)->pkt_dts == 998);
  REQUIRE((*first.frame)->best_effort_timestamp == 1002);
  REQUIRE((*first.frame)->duration == 40);
  REQUIRE((*first.frame)->time_base.den == 1000);
}

TEST_CASE("ForceFps rejects invalid configuration and malformed inputs",
          "[force-fps]") {
  REQUIRE_THROWS_AS(ForceFps(ForceFpsConfig{AVRational{0, 1}, 1}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ForceFps(ForceFpsConfig{AVRational{25, -1}, 1}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ForceFps(ForceFpsConfig{AVRational{25, 1}, 0}),
                    std::invalid_argument);
  ForceFps fps;
  REQUIRE_THROWS_AS(fps.TryPushFrame(AvDecoderReadResult{}),
                    std::invalid_argument);
  auto input = Video(0);
  SECTION("Missing PTS") { (*input.frame)->pts = AV_NOPTS_VALUE; }
  SECTION("Invalid time base") { input.time_base = AVRational{0, 1}; }
  SECTION("Audio frame") { (*input.frame)->nb_samples = 128; }
  SECTION("Absent frame") { input.frame.reset(); }
  REQUIRE_THROWS_AS(fps.TryPushFrame(input), std::invalid_argument);
  REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
  REQUIRE(fps.TryPushFrame(Video(0)));
  Check(fps.TryReadFrame(), 0, 1);
}

TEST_CASE(
    "ForceFps repeats CUDA surfaces without downloading or copying pixels",
    "[.][force-fps][cuda]") {
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
  frame->pts = 1000;
  const auto* surface = frame->data[0];
  const auto* frame_context = frame->hw_frames_ctx->data;
  auto input = Video(1000);
  input.frame = std::move(frame);
  auto next = input;
  (*next.frame)->pts = 1120;
  ForceFps fps;
  REQUIRE(fps.TryPushFrame(input));
  REQUIRE(fps.TryPushFrame(next));
  for (std::int64_t pts = 25; pts <= 28; ++pts) {
    const auto output = fps.TryReadFrame();
    REQUIRE(output.status == AvDecoderReadStatus::kFrame);
    REQUIRE((*output.frame)->pts == pts);
    REQUIRE((*output.frame)->format == AV_PIX_FMT_CUDA);
    REQUIRE((*output.frame)->data[0] == surface);
    REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
  }
  REQUIRE((*input.frame)->pts == 1000);
  REQUIRE((*next.frame)->pts == 1120);
}

}  // namespace
