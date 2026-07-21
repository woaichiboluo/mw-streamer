#include "mw/streamer/version.h"

#include "gtest/gtest.h"

TEST(VersionTest, ReturnsProjectVersion) {
  EXPECT_EQ(mw::streamer::VersionString(), "0.1.0");
}
