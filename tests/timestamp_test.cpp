#include "mw/streamer/internal/common/timestamp.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace mw::streamer::internal {
namespace {

TEST(TimestampTest, ValidatesPositiveTimeBase) {
  EXPECT_TRUE(IsValidTimeBase(AVRational{1, 48'000}));
  EXPECT_FALSE(IsValidTimeBase(AVRational{0, 1}));
  EXPECT_FALSE(IsValidTimeBase(AVRational{1, 0}));
  EXPECT_FALSE(IsValidTimeBase(AVRational{-1, 48'000}));
}

TEST(TimestampTest, RescalesTimestampAndPreservesUnavailableValue) {
  EXPECT_EQ(RescaleTimestamp(480, AVRational{1, 48'000}, kMicrosecondsTimeBase),
            10'000);
  EXPECT_EQ(RescaleTimestamp(AV_NOPTS_VALUE, AVRational{1, 48'000},
                             kMicrosecondsTimeBase),
            AV_NOPTS_VALUE);
  EXPECT_EQ(RescaleTimestamp(480, AVRational{0, 1}, kMicrosecondsTimeBase),
            AV_NOPTS_VALUE);
}

TEST(TimestampTest, ConvertsToMicroseconds) {
  EXPECT_EQ(ToMicroseconds(90'000, AVRational{1, 90'000}), 1'000'000);
}

TEST(TimestampTest, AddsOffsetAcrossTimeBases) {
  EXPECT_EQ(AddTimestampOffset(90'000, AVRational{1, 90'000}, 24'000,
                               AVRational{1, 48'000}),
            135'000);
  EXPECT_EQ(AddTimestampOffset(AV_NOPTS_VALUE, AVRational{1, 90'000}, 1,
                               AVRational{1, 48'000}),
            AV_NOPTS_VALUE);
}

}  // namespace
}  // namespace mw::streamer::internal
