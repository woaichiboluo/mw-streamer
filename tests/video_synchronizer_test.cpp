#include "mw/streamer/internal/video_synchronizer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

using namespace std::chrono_literals;

ffmpeg::Frame MakeFrame(std::int64_t pts,
                        AVRational time_base = AVRational{1, 1'000}) {
  ffmpeg::Frame frame;
  frame.get()->pts = pts;
  frame.get()->time_base = time_base;
  return frame;
}

std::int64_t FramePts(const ffmpeg::Frame& frame) { return frame.get()->pts; }

VideoSynchronizerConfig MakeConfig(std::size_t input_count) {
  VideoSynchronizerConfig config;
  config.input_count = input_count;
  return config;
}

TEST(VideoSynchronizerTest, SingleInputReturnsFrameImmediately) {
  VideoSynchronizer synchronizer(MakeConfig(1));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(0, MakeFrame(17), 1'000'000, start);

  std::optional<SynchronizedVideoFrames> group = synchronizer.Pop(start);

  ASSERT_TRUE(group.has_value());
  ASSERT_EQ(group->frames.size(), 1U);
  EXPECT_EQ(group->primary_pts_us, 1'000'000);
  EXPECT_EQ(FramePts(group->frames[0]), 17);
  EXPECT_EQ(synchronizer.queued_frame_count(0), 0U);
}

TEST(VideoSynchronizerTest, PreservesOriginalTimestampsAndInputOrder) {
  VideoSynchronizerConfig config = MakeConfig(3);
  config.primary_input_index = 1;
  VideoSynchronizer synchronizer(config);
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(2, MakeFrame(30, AVRational{1, 30}), 1'000'001, start);
  synchronizer.Push(0, MakeFrame(90'000, AVRational{1, 90'000}), 1'000'002,
                    start);
  synchronizer.Push(1, MakeFrame(1'000), 1'000'000, start);

  auto group = synchronizer.Pop(start);

  ASSERT_TRUE(group.has_value());
  ASSERT_EQ(group->frames.size(), 3U);
  EXPECT_EQ(group->primary_pts_us, 1'000'000);
  EXPECT_EQ(FramePts(group->frames[0]), 90'000);
  EXPECT_EQ(FramePts(group->frames[1]), 1'000);
  EXPECT_EQ(FramePts(group->frames[2]), 30);
  EXPECT_EQ(group->frames[0].get()->time_base.num, 1);
  EXPECT_EQ(group->frames[0].get()->time_base.den, 90'000);
}

TEST(VideoSynchronizerTest, AcceptsUnavailableRawPtsWithValidTimelinePts) {
  VideoSynchronizer synchronizer(MakeConfig(1));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(0, MakeFrame(AV_NOPTS_VALUE, AVRational{0, 1}), 42, start);

  auto group = synchronizer.Pop(start);

  ASSERT_TRUE(group.has_value());
  ASSERT_EQ(group->frames.size(), 1U);
  EXPECT_EQ(FramePts(group->frames[0]), AV_NOPTS_VALUE);
  EXPECT_EQ(group->primary_pts_us, 42);
}

TEST(VideoSynchronizerTest,
     SelectsNearestFrameAndBreaksTiesTowardEarlierTimelinePts) {
  VideoSynchronizer synchronizer(MakeConfig(2));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(1, MakeFrame(995), 995'000, start);
  synchronizer.Push(1, MakeFrame(1'003), 1'003'000, start);
  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);

  auto group = synchronizer.Pop(start);

  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(FramePts(group->frames[1]), 1'003);

  VideoSynchronizer tie_synchronizer(MakeConfig(2));
  tie_synchronizer.Push(1, MakeFrame(995), 995'000, start);
  tie_synchronizer.Push(1, MakeFrame(1'005), 1'005'000, start);
  tie_synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);

  group = tie_synchronizer.Pop(start);

  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(FramePts(group->frames[1]), 995);
}

TEST(VideoSynchronizerTest, WaitsForEveryInputToReachPrimaryTimeline) {
  VideoSynchronizer synchronizer(MakeConfig(3));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(1, MakeFrame(996), 996'000, start);
  synchronizer.Push(2, MakeFrame(1'000), 1'000'000, start);
  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);

  EXPECT_FALSE(synchronizer.Pop(start + 99ms).has_value());

  auto group = synchronizer.Pop(start + 100ms);

  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(FramePts(group->frames[1]), 996);
}

TEST(VideoSynchronizerTest, MissingInputDropsWholeGroupAfterMaximumWait) {
  VideoSynchronizer synchronizer(MakeConfig(3));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(1, MakeFrame(1'000), 1'000'000, start);
  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);

  EXPECT_FALSE(synchronizer.Pop(start + 99ms).has_value());
  EXPECT_EQ(synchronizer.queued_frame_count(0), 1U);
  EXPECT_FALSE(synchronizer.Pop(start + 100ms).has_value());
  EXPECT_EQ(synchronizer.queued_frame_count(0), 0U);
  EXPECT_EQ(synchronizer.queued_frame_count(1), 1U);
  EXPECT_EQ(synchronizer.queued_frame_count(2), 0U);
}

TEST(VideoSynchronizerTest,
     DropsUnmatchedPrimaryWithoutConsumingOtherInputsFutureFrames) {
  VideoSynchronizer synchronizer(MakeConfig(3));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(1, MakeFrame(1'000), 1'000'000, start);
  synchronizer.Push(2, MakeFrame(1'010), 1'010'000, start);
  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);

  EXPECT_FALSE(synchronizer.Pop(start).has_value());
  EXPECT_EQ(synchronizer.queued_frame_count(0), 0U);
  EXPECT_EQ(synchronizer.queued_frame_count(1), 1U);
  EXPECT_EQ(synchronizer.queued_frame_count(2), 1U);

  synchronizer.Push(1, MakeFrame(1'010), 1'010'000, start + 1ms);
  synchronizer.Push(0, MakeFrame(1'010), 1'010'000, start + 1ms);
  auto group = synchronizer.Pop(start + 1ms);

  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(FramePts(group->frames[0]), 1'010);
  EXPECT_EQ(FramePts(group->frames[1]), 1'010);
  EXPECT_EQ(FramePts(group->frames[2]), 1'010);
}

TEST(VideoSynchronizerTest, NeverReusesAFrameFromAnEmittedGroup) {
  VideoSynchronizer synchronizer(MakeConfig(2));
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(1, MakeFrame(1'000), 1'000'000, start);
  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);
  ASSERT_TRUE(synchronizer.Pop(start).has_value());

  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start + 1ms);

  EXPECT_FALSE(synchronizer.Pop(start + 101ms).has_value());
  EXPECT_EQ(synchronizer.queued_frame_count(0), 0U);
  EXPECT_EQ(synchronizer.queued_frame_count(1), 0U);
}

TEST(VideoSynchronizerTest, DropsOldestFrameWhenQueueIsFull) {
  VideoSynchronizerConfig config = MakeConfig(2);
  config.queue_capacity = 2;
  VideoSynchronizer synchronizer(config);
  const VideoSynchronizer::TimePoint start{};
  synchronizer.Push(1, MakeFrame(1'000), 1'000'000, start);
  synchronizer.Push(1, MakeFrame(1'010), 1'010'000, start);
  synchronizer.Push(1, MakeFrame(1'020), 1'020'000, start);

  EXPECT_EQ(synchronizer.queued_frame_count(1), 2U);
  synchronizer.Push(0, MakeFrame(1'000), 1'000'000, start);
  EXPECT_FALSE(synchronizer.Pop(start).has_value());

  synchronizer.Push(0, MakeFrame(1'010), 1'010'000, start);
  auto group = synchronizer.Pop(start);

  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(FramePts(group->frames[1]), 1'010);
}

TEST(VideoSynchronizerTest, RejectsInvalidConfigurationAndPushArguments) {
  VideoSynchronizerConfig config = MakeConfig(0);
  EXPECT_THROW(VideoSynchronizer synchronizer(config), Error);

  config = MakeConfig(2);
  config.primary_input_index = 2;
  EXPECT_THROW(VideoSynchronizer synchronizer(config), Error);
  config.primary_input_index = 0;
  config.pts_tolerance = -1us;
  EXPECT_THROW(VideoSynchronizer synchronizer(config), Error);
  config.pts_tolerance = 5ms;
  config.maximum_wait = 0ms;
  EXPECT_THROW(VideoSynchronizer synchronizer(config), Error);
  config.maximum_wait = 100ms;
  config.queue_capacity = 0;
  EXPECT_THROW(VideoSynchronizer synchronizer(config), Error);

  VideoSynchronizer synchronizer(MakeConfig(2));
  const VideoSynchronizer::TimePoint start{};
  EXPECT_THROW(synchronizer.Push(2, MakeFrame(0), 0, start), Error);
  EXPECT_THROW(synchronizer.Push(0, MakeFrame(0), AV_NOPTS_VALUE, start),
               Error);

  ffmpeg::Frame frame;
  ffmpeg::Frame owner = std::move(frame);
  EXPECT_NE(owner.get(), nullptr);
  EXPECT_THROW(synchronizer.Push(0, std::move(frame), 0, start), Error);
}

}  // namespace
}  // namespace mw::streamer::internal
