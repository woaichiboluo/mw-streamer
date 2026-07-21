#include "mw/streamer/internal/common/blocking_queue.h"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <utility>

#include "gtest/gtest.h"

namespace mw::streamer::internal {
namespace {

TEST(BlockingQueueTest, BlocksProducerUntilConsumerReleasesCapacity) {
  BlockingQueue<int> queue(1);
  ASSERT_TRUE(queue.Push(1));

  std::promise<void> second_push_started;
  std::future<void> started = second_push_started.get_future();
  auto producer =
      std::async(std::launch::async, [&queue, &second_push_started] {
        second_push_started.set_value();
        return queue.Push(2);
      });
  started.get();
  EXPECT_EQ(producer.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  ASSERT_EQ(queue.Pop(), std::optional<int>(1));
  EXPECT_TRUE(producer.get());
  EXPECT_EQ(queue.Pop(), std::optional<int>(2));
}

TEST(BlockingQueueTest, CloseAllowsConsumerToDrainQueuedValues) {
  BlockingQueue<std::unique_ptr<int>> queue(2);
  ASSERT_TRUE(queue.Push(std::make_unique<int>(7)));
  queue.Close();

  auto value = queue.Pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(**value, 7);
  EXPECT_FALSE(queue.Pop().has_value());
  EXPECT_FALSE(queue.Push(std::make_unique<int>(8)));
}

TEST(BlockingQueueTest, CancelDiscardsValuesAndWakesBlockedProducer) {
  BlockingQueue<int> queue(1);
  ASSERT_TRUE(queue.Push(1));
  auto producer =
      std::async(std::launch::async, [&queue] { return queue.Push(2); });

  queue.Cancel();
  EXPECT_FALSE(producer.get());
  EXPECT_FALSE(queue.Pop().has_value());
  EXPECT_EQ(queue.size(), 0U);
}

TEST(BlockingQueueTest, LivePushDropsOldestValueAtCapacity) {
  BlockingQueue<int> queue(2);
  ASSERT_TRUE(queue.PushDroppingOldest(1));
  ASSERT_TRUE(queue.PushDroppingOldest(2));

  EXPECT_TRUE(queue.PushDroppingOldest(3));
  EXPECT_EQ(queue.size(), 2U);
  EXPECT_EQ(queue.Pop(), 2);
  EXPECT_EQ(queue.Pop(), 3);
}

TEST(BlockingQueueTest, TimedPopReturnsWhenValueArrivesOrDeadlineExpires) {
  BlockingQueue<int> queue(1);
  EXPECT_FALSE(queue.PopFor(std::chrono::milliseconds(1)).has_value());

  ASSERT_TRUE(queue.Push(9));
  EXPECT_EQ(queue.PopFor(std::chrono::seconds(1)), std::optional<int>(9));
}

TEST(BlockingQueueTest, TryPushNeverBlocksAndRejectsFullOrClosedQueue) {
  BlockingQueue<int> queue(1);
  EXPECT_TRUE(queue.TryPush(1));
  EXPECT_FALSE(queue.TryPush(2));
  EXPECT_EQ(queue.Pop(), std::optional<int>(1));

  queue.Close();
  EXPECT_FALSE(queue.TryPush(3));
}

TEST(BlockingQueueTest, ClearDiscardsValuesWithoutClosingQueue) {
  BlockingQueue<int> queue(2);
  ASSERT_TRUE(queue.TryPush(1));
  ASSERT_TRUE(queue.TryPush(2));

  queue.Clear();
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_TRUE(queue.TryPush(3));
  EXPECT_EQ(queue.Pop(), std::optional<int>(3));
}

}  // namespace
}  // namespace mw::streamer::internal
