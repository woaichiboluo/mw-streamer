#include "mw/streamer/internal/common/reconnect_backoff.h"

#include <chrono>

#include "gtest/gtest.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {
namespace {

TEST(ReconnectBackoffTest, DoublesAndCapsTheBaseDelay) {
  ReconnectConfig config;
  config.initial_backoff = std::chrono::seconds(1);
  config.maximum_backoff = std::chrono::seconds(30);
  config.jitter_ratio = 0.0;
  ReconnectBackoff backoff(config);

  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(1));
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(2));
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(4));
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(8));
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(16));
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(30));
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(30));
}

TEST(ReconnectBackoffTest, AppliesConfiguredJitterAndResets) {
  ReconnectConfig config;
  config.jitter_ratio = 0.1;
  ReconnectBackoff backoff(config);

  EXPECT_EQ(backoff.NextDelay(-1.0), std::chrono::milliseconds(900));
  EXPECT_EQ(backoff.NextDelay(1.0), std::chrono::milliseconds(2'200));
  backoff.Reset();
  EXPECT_EQ(backoff.retry_count(), 0U);
  EXPECT_EQ(backoff.NextDelay(0.0), std::chrono::seconds(1));
}

TEST(ReconnectBackoffTest, HonorsFiniteRetryCount) {
  ReconnectConfig config;
  config.max_retries = 2;
  ReconnectBackoff backoff(config);

  EXPECT_TRUE(backoff.CanRetry());
  static_cast<void>(backoff.NextDelay(0.0));
  static_cast<void>(backoff.NextDelay(0.0));
  EXPECT_FALSE(backoff.CanRetry());
  EXPECT_THROW(static_cast<void>(backoff.NextDelay(0.0)), Error);
}

}  // namespace
}  // namespace mw::streamer::internal
