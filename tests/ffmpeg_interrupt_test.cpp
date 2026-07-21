#include "mw/streamer/internal/common/ffmpeg_interrupt.h"

#include <chrono>
#include <thread>

#include "gtest/gtest.h"

namespace mw::streamer::internal {
namespace {

TEST(FfmpegInterruptTest, RemainsInactiveWhileDisarmed) {
  FfmpegInterrupt interrupt;
  AVIOInterruptCB callback = interrupt.callback();
  ASSERT_NE(callback.callback, nullptr);
  EXPECT_EQ(callback.callback(callback.opaque), 0);
  EXPECT_FALSE(interrupt.is_cancelled());
  EXPECT_FALSE(interrupt.timed_out());
}

TEST(FfmpegInterruptTest, InterruptsAfterArmedDeadline) {
  FfmpegInterrupt interrupt;
  interrupt.Arm(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  AVIOInterruptCB callback = interrupt.callback();
  EXPECT_EQ(callback.callback(callback.opaque), 1);
  EXPECT_TRUE(interrupt.timed_out());
  EXPECT_FALSE(interrupt.is_cancelled());
}

TEST(FfmpegInterruptTest, CancellationRemainsActiveAfterDisarm) {
  FfmpegInterrupt interrupt;
  interrupt.Cancel();
  interrupt.Disarm();

  AVIOInterruptCB callback = interrupt.callback();
  EXPECT_EQ(callback.callback(callback.opaque), 1);
  EXPECT_TRUE(interrupt.is_cancelled());
}

}  // namespace
}  // namespace mw::streamer::internal
