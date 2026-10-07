#include "mw/streamer/scheduler/scheduler_timing.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>

namespace {

using mw::streamer::internal::AudioNsToSamples;
using mw::streamer::internal::AudioOutputTiming;
using mw::streamer::internal::AudioSamplesToNs;
using mw::streamer::internal::SourceAudioTiming;
constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kMillisecond = 1'000'000;
constexpr int kRate = 48000;

TEST_CASE("调度音频时间转换按总采样数截断而不累计块舍入",
          "[scheduler][timing]") {
  CHECK(AudioSamplesToNs(1024, kRate) == 21'333'333);
  CHECK(AudioSamplesToNs(2048, kRate) == 42'666'666);
  CHECK(AudioSamplesToNs(3072, kRate) == 64'000'000);
  CHECK(AudioSamplesToNs(3072, kRate) != 3 * AudioSamplesToNs(1024, kRate));
  CHECK(AudioNsToSamples(64'000'000, kRate) == 3072);
  CHECK(AudioNsToSamples(20'000, kRate) == 0);
}

TEST_CASE("调度音频保留原PTS并使用视频到系统时钟的映射",
          "[scheduler][timing]") {
  SourceAudioTiming timing;
  timing.SetVideoTiming(10 * kSecond, 100 * kSecond);
  const auto first =
      timing.Map(10 * kSecond + 20 * kMillisecond, 480, kRate, 100 * kSecond);
  CHECK(first.timestamp_ns == 100 * kSecond + 20 * kMillisecond);
  CHECK_FALSE(first.append);
  CHECK_FALSE(first.reset_buffer);
  CHECK(timing.next_source_pts() == 10 * kSecond + 30 * kMillisecond);
  CHECK(timing.next_system_pts() == 100 * kSecond + 30 * kMillisecond);
  const auto second =
      timing.Map(10 * kSecond + 30 * kMillisecond, 480, kRate, 100 * kSecond);
  CHECK(second.timestamp_ns == 100 * kSecond + 30 * kMillisecond);
  CHECK(second.append);
  CHECK_FALSE(second.reset_buffer);
}

TEST_CASE("调度音频源PTS平滑使用严格70毫秒边界", "[scheduler][timing]") {
  const auto deviation = GENERATE(69'999'999LL, 70'000'000LL, 70'000'001LL);
  const auto sign = GENERATE(-1LL, 1LL);
  SourceAudioTiming timing;
  timing.Map(10 * kSecond, 480, kRate, 100 * kSecond);
  const auto expected = 10 * kSecond + 10 * kMillisecond;
  const auto supplied = expected + sign * deviation;
  const auto mapped = timing.Map(supplied, 480, kRate, 100 * kSecond);
  const bool smooth = deviation < 70 * kMillisecond;
  const auto selected = smooth ? expected : supplied;
  CHECK(mapped.timestamp_ns == selected + 90 * kSecond);
  CHECK(mapped.append == smooth);
  CHECK_FALSE(mapped.reset_buffer);
  CHECK(timing.next_source_pts() == selected + 10 * kMillisecond);
}

TEST_CASE("调度音频系统域连续检查使用严格70毫秒边界", "[scheduler][timing]") {
  const auto deviation = GENERATE(69'999'999LL, 70'000'000LL, 70'000'001LL);
  SourceAudioTiming timing;
  timing.Map(10 * kSecond, 480, kRate, 100 * kSecond);
  const auto source = 10 * kSecond + 10 * kMillisecond;
  const auto mapped = 100 * kSecond + 10 * kMillisecond + deviation;
  timing.SetVideoTiming(source, mapped);
  const auto result = timing.Map(source, 480, kRate, 100 * kSecond);
  CHECK(result.timestamp_ns == mapped);
  CHECK(result.append == (deviation < 70 * kMillisecond));
  CHECK_FALSE(result.reset_buffer);
}

TEST_CASE("调度音频源PTS跳变仅在严格超过两秒时清PCM", "[scheduler][timing]") {
  const auto deviation =
      GENERATE(1'999'999'999LL, 2'000'000'000LL, 2'000'000'001LL);
  const auto sign = GENERATE(-1LL, 1LL);
  SourceAudioTiming timing;
  timing.Map(10 * kSecond, 480, kRate, 100 * kSecond);
  const auto source = 10 * kSecond + 10 * kMillisecond + sign * deviation;
  const auto result = timing.Map(source, 480, kRate, 101 * kSecond);
  const bool jump = deviation > 2 * kSecond;
  CHECK(result.reset_buffer == jump);
  CHECK(result.append == jump);
  CHECK(result.timestamp_ns == (jump ? 101 * kSecond : source + 90 * kSecond));
  CHECK(timing.next_source_pts() == source + 10 * kMillisecond);
}

TEST_CASE("调度系统域跳变重建映射但不无条件清PCM", "[scheduler][timing]") {
  const auto deviation = GENERATE(2'000'000'000LL, 2'000'000'001LL);
  SourceAudioTiming timing;
  timing.Map(10 * kSecond, 480, kRate, 100 * kSecond);
  const auto source = 10 * kSecond + 10 * kMillisecond;
  const auto expected = 100 * kSecond + 10 * kMillisecond;
  timing.SetVideoTiming(source, expected + deviation);
  const auto result = timing.Map(source, 480, kRate, 101 * kSecond);
  CHECK_FALSE(result.reset_buffer);
  CHECK_FALSE(result.append);
  CHECK(result.timestamp_ns ==
        (deviation > 2 * kSecond ? 101 * kSecond : expected + deviation));
  CHECK(timing.next_source_pts() == source + 10 * kMillisecond);
  CHECK(timing.next_system_pts() == result.timestamp_ns + 10 * kMillisecond);
}

TEST_CASE("调度直接系统时间戳判定也使用严格两秒边界", "[scheduler][timing]") {
  const auto distance = GENERATE(1'999'999'999LL, 2'000'000'000LL);
  SourceAudioTiming timing;
  timing.SetVideoTiming(0, 5 * kSecond);
  const auto source = 100 * kSecond - distance;
  const auto result = timing.Map(source, 480, kRate, 100 * kSecond);
  CHECK(result.timestamp_ns ==
        (distance < 2 * kSecond ? source : source + 5 * kSecond));
  CHECK_FALSE(result.reset_buffer);
}

TEST_CASE("调度在连续性判断之后扣减重采样延迟", "[scheduler][timing]") {
  SourceAudioTiming timing;
  const auto first =
      timing.Map(10 * kSecond, 480, kRate, 100 * kSecond, 80 * kMillisecond);
  CHECK(first.timestamp_ns == 100 * kSecond - 80 * kMillisecond);
  CHECK(timing.next_system_pts() == 100 * kSecond + 10 * kMillisecond);
  const auto second = timing.Map(10 * kSecond + 10 * kMillisecond, 480, kRate,
                                 100 * kSecond, 90 * kMillisecond);
  CHECK(second.append);
  CHECK_FALSE(second.reset_buffer);
  CHECK(second.timestamp_ns == 100 * kSecond - 80 * kMillisecond);
  CHECK(timing.next_system_pts() == 100 * kSecond + 20 * kMillisecond);
}

TEST_CASE("调度清时钟映射与完全重置有不同的期待值语义", "[scheduler][timing]") {
  SourceAudioTiming timing;
  timing.Map(10 * kSecond, 480, kRate, 100 * kSecond);
  timing.ClearTiming();
  CHECK_FALSE(timing.timing_set());
  CHECK(timing.next_source_pts() == 10 * kSecond + 10 * kMillisecond);
  CHECK(timing.next_system_pts() == 100 * kSecond + 10 * kMillisecond);
  const auto result =
      timing.Map(10 * kSecond + 10 * kMillisecond, 480, kRate, 101 * kSecond);
  CHECK(result.timestamp_ns == 101 * kSecond);
  CHECK_FALSE(result.reset_buffer);
  timing.Reset();
  CHECK_FALSE(timing.timing_set());
  CHECK(timing.timing_adjust() == 0);
  CHECK(timing.next_source_pts() == 0);
  CHECK(timing.next_system_pts() == 0);
}

TEST_CASE("调度首次放置音频重置系统预测起点而非保留下一块结束时间",
          "[scheduler][timing]") {
  SourceAudioTiming timing;
  const auto first = timing.Map(10 * kSecond, 1024, kRate, 100 * kSecond);
  CHECK(timing.next_system_pts() == 100 * kSecond + 21'333'333);
  // Empty FIFO: QueueAudio resets its origin and predicted system timestamp
  // to this frame's actual origin after Map updates its end prediction.
  timing.ResetBufferTimestamp(first.timestamp_ns);
  CHECK(timing.next_system_pts() == 100 * kSecond);
  const auto source = 10 * kSecond + 21'333'333;
  timing.SetVideoTiming(source, 100 * kSecond + 21'333'333 + 59 * kMillisecond);
  const auto second = timing.Map(source, 1024, kRate, 100 * kSecond);
  CHECK(second.timestamp_ns == 100 * kSecond + 80'333'333);
  // Relative to the FIFO origin this is 80.333 ms, requiring placement.
  // Measuring relative to the old predicted end would wrongly call the 59 ms
  // change continuous and append, eliminating 2831 real gap samples at 48 kHz.
  CHECK_FALSE(second.append);
  CHECK_FALSE(second.reset_buffer);
  CHECK(AudioNsToSamples(second.timestamp_ns - first.timestamp_ns, kRate) -
            1024 ==
        2831);
}

TEST_CASE("调度回退放置音频只重设系统预测保留源PTS预测和映射",
          "[scheduler][timing]") {
  SourceAudioTiming timing;
  const auto first = timing.Map(10 * kSecond, 1024, kRate, 100 * kSecond);
  timing.ResetBufferTimestamp(first.timestamp_ns);
  const auto source = 10 * kSecond + 21'333'333;
  timing.SetVideoTiming(source, 100 * kSecond - 100 * kMillisecond);
  const auto backward = timing.Map(source, 1024, kRate, 100 * kSecond);
  CHECK_FALSE(backward.append);
  CHECK_FALSE(backward.reset_buffer);
  CHECK(backward.timestamp_ns < first.timestamp_ns);
  const auto source_prediction = timing.next_source_pts();
  const auto adjustment = timing.timing_adjust();
  timing.ResetBufferTimestamp(backward.timestamp_ns);
  CHECK(timing.next_system_pts() == backward.timestamp_ns);
  CHECK(timing.next_source_pts() == source_prediction);
  CHECK(timing.timing_adjust() == adjustment);
  CHECK(timing.timing_set());
}

TEST_CASE("调度源PTS跳变内部清FIFO后仍保留Map最终系统结束预测",
          "[scheduler][timing]") {
  SourceAudioTiming timing;
  const auto first = timing.Map(10 * kSecond, 1024, kRate, 100 * kSecond);
  timing.ResetBufferTimestamp(first.timestamp_ns);
  const auto jumped = timing.Map(20 * kSecond, 1024, kRate, 101 * kSecond);
  CHECK(jumped.reset_buffer);
  CHECK(jumped.append);
  CHECK(jumped.timestamp_ns == 101 * kSecond);
  // QueueAudio's physical clear for reset_buffer must not call the setter:
  // Map has already applied the reset before updating its final prediction.
  CHECK(timing.next_system_pts() == 101 * kSecond + 21'333'333);
  const auto next =
      timing.Map(20 * kSecond + 21'333'333, 1024, kRate, 101 * kSecond);
  CHECK(next.append);
  CHECK_FALSE(next.reset_buffer);
}

TEST_CASE("调度默认45块是上限而不是启动预缓冲", "[scheduler][timing]") {
  AudioOutputTiming timing;
  timing.Push(100 * kSecond, 100 * kSecond + 21'333'333);
  CHECK(timing.total_buffering_ticks() == 0);
  CHECK(timing.waiting_ticks() == 0);
  CHECK_FALSE(timing.maxed());
  CHECK(timing.window().start == 100 * kSecond);
  CHECK(timing.Finish());
}

TEST_CASE("调度迟到缓冲在历史窗口队首插入并跳过等待输出",
          "[scheduler][timing]") {
  // 1024 samples at this rate are exactly one millisecond.
  constexpr int kIntegralRate = 1024000;
  AudioOutputTiming timing;
  timing.Push(100 * kMillisecond, 101 * kMillisecond);
  timing.BufferTo(98 * kMillisecond, kIntegralRate, 1024);
  CHECK(timing.total_buffering_ticks() == 2);
  CHECK(timing.waiting_ticks() == 2);
  CHECK(timing.window().start == 98 * kMillisecond);
  CHECK(timing.window().end == 99 * kMillisecond);
  CHECK_FALSE(timing.Finish());
  CHECK(timing.total_buffering_ticks() == 2);
  CHECK(timing.waiting_ticks() == 1);
  timing.Push(101 * kMillisecond, 102 * kMillisecond);
  CHECK(timing.window().start == 99 * kMillisecond);
  CHECK_FALSE(timing.Finish());
  timing.Push(102 * kMillisecond, 103 * kMillisecond);
  CHECK(timing.waiting_ticks() == 0);
  CHECK(timing.window().start == 100 * kMillisecond);
  CHECK(timing.window().end == 101 * kMillisecond);
  CHECK(timing.Finish());
  CHECK(timing.total_buffering_ticks() == 2);
  CHECK(timing.window().start == 101 * kMillisecond);
}

TEST_CASE("调度等待期间继续增加缓冲沿用同一个历史基点", "[scheduler][timing]") {
  AudioOutputTiming timing;
  timing.Push(100 * kMillisecond, 101 * kMillisecond);
  timing.BufferTo(98 * kMillisecond, 1024000, 1024);
  timing.BufferTo(97 * kMillisecond, 1024000, 1024);
  CHECK(timing.total_buffering_ticks() == 3);
  CHECK(timing.waiting_ticks() == 3);
  CHECK(timing.window().start == 97 * kMillisecond);
  CHECK(timing.window().end == 98 * kMillisecond);
  CHECK_FALSE(timing.Finish());
  CHECK(timing.window().start == 98 * kMillisecond);
}

TEST_CASE("调度缓冲上限45块且等待结束后额度不恢复", "[scheduler][timing]") {
  AudioOutputTiming timing;
  timing.Push(100 * kSecond, 100 * kSecond + kMillisecond);
  timing.BufferTo(90 * kSecond, 1024000, 1024);
  REQUIRE(timing.total_buffering_ticks() == 45);
  REQUIRE(timing.waiting_ticks() == 45);
  CHECK(timing.maxed());
  CHECK(timing.window().start == 100 * kSecond - 45 * kMillisecond);
  for (int tick = 0; tick < 45; ++tick) {
    CHECK_FALSE(timing.Finish());
    timing.Push(100 * kSecond + (tick + 1) * kMillisecond,
                100 * kSecond + (tick + 2) * kMillisecond);
  }
  CHECK(timing.waiting_ticks() == 0);
  CHECK(timing.total_buffering_ticks() == 45);
  CHECK(timing.window().start == 100 * kSecond);
  timing.BufferTo(80 * kSecond, 1024000, 1024);
  CHECK(timing.waiting_ticks() == 0);
  CHECK(timing.window().start == 100 * kSecond);
  CHECK(timing.Finish());
  timing.Reset();
  CHECK_FALSE(timing.maxed());
  CHECK(timing.waiting_ticks() == 0);
  CHECK(timing.total_buffering_ticks() == 0);
  timing.Push(200 * kSecond, 200 * kSecond + kMillisecond);
  CHECK(timing.window().start == 200 * kSecond);
  CHECK(timing.Finish());
}

TEST_CASE("调度缓冲先截断时间到采样再向上取整块数", "[scheduler][timing]") {
  AudioOutputTiming timing;
  timing.Push(100 * kSecond, 100 * kSecond + 21'333'333);
  timing.BufferTo(100 * kSecond - 20'000, kRate, 1024);
  CHECK(timing.total_buffering_ticks() == 0);
  timing.BufferTo(100 * kSecond - AudioSamplesToNs(2048, kRate), kRate, 1024);
  CHECK(timing.total_buffering_ticks() == 2);
  CHECK(timing.window().start == 100 * kSecond - 42'666'666);
  CHECK(timing.window().end == 100 * kSecond - 21'333'333);
}

}  // namespace
