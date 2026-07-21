#include "mw/streamer/status.h"

#include "gtest/gtest.h"

namespace mw::streamer {
namespace {

TEST(StatusTest, DefaultStatusIsOk) {
  const Status status;

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kOk);
  EXPECT_TRUE(status.message().empty());
}

TEST(StatusTest, PreservesErrorCodeAndMessage) {
  const Status status(StatusCode::kUnsupported, "unsupported codec");

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kUnsupported);
  EXPECT_EQ(status.message(), "unsupported codec");
}

}  // namespace
}  // namespace mw::streamer
