#include "mw/streamer/internal/compressed_packet_delay_buffer.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
}

#include "gtest/gtest.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

ScheduledPacket MakePacket(std::size_t input_index, std::int64_t dts_us,
                           std::uint64_t sequence) {
  ffmpeg::Packet packet;
  packet.get()->dts = dts_us;
  packet.get()->pts = dts_us;
  packet.get()->time_base = {1, 1'000'000};
  ScheduledPacket result;
  result.input_index = input_index;
  result.packet = std::move(packet);
  result.source_dts_us = dts_us;
  result.timeline_dts_us = dts_us;
  result.sequence = sequence;
  return result;
}

TEST(CompressedPacketDelayBufferTest, WaitsUntilEveryInputIsPreheated) {
  CompressedPacketDelayBuffer buffer(2, std::chrono::milliseconds(10));
  ASSERT_TRUE(buffer.Push(MakePacket(0, 0, 0)));
  ASSERT_TRUE(buffer.Push(MakePacket(0, 10'000, 1)));
  ASSERT_TRUE(buffer.Push(MakePacket(1, 2'000, 0)));
  auto pop =
      std::async(std::launch::async, [&buffer] { return buffer.WaitPop(0); });
  EXPECT_EQ(pop.wait_for(std::chrono::milliseconds(10)),
            std::future_status::timeout);

  ASSERT_TRUE(buffer.Push(MakePacket(1, 12'000, 1)));
  const std::optional<ScheduledPacket> first = pop.get();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->input_index, 0U);
  EXPECT_EQ(first->packet.get()->dts, 0);
  EXPECT_TRUE(buffer.is_started());
}

TEST(CompressedPacketDelayBufferTest,
     KeepsOriginalDtsAndRejectsUnavailableDts) {
  CompressedPacketDelayBuffer buffer(1, std::chrono::milliseconds(0));
  ScheduledPacket valid = MakePacket(0, 7'000, 0);
  valid.timeline_dts_us = 20'000;
  ASSERT_TRUE(buffer.Push(std::move(valid)));
  const std::optional<ScheduledPacket> output = buffer.WaitPop(0);
  ASSERT_TRUE(output.has_value());
  EXPECT_EQ(output->packet.get()->dts, 7'000);
  EXPECT_EQ(output->timeline_dts_us, 20'000);

  ScheduledPacket invalid = MakePacket(0, 8'000, 1);
  invalid.packet.get()->dts = AV_NOPTS_VALUE;
  EXPECT_THROW(static_cast<void>(buffer.Push(std::move(invalid))), Error);
}

TEST(CompressedPacketDelayBufferTest, OrdersPacketsByDtsThenSequence) {
  CompressedPacketDelayBuffer buffer(1, std::chrono::milliseconds(10));
  ASSERT_TRUE(buffer.Push(MakePacket(0, 10'000, 2)));
  ASSERT_TRUE(buffer.Push(MakePacket(0, 0, 0)));
  ASSERT_TRUE(buffer.Push(MakePacket(0, 10'000, 1)));

  const std::optional<ScheduledPacket> first = buffer.WaitPop(0);
  const std::optional<ScheduledPacket> second = buffer.WaitPop(0);
  const std::optional<ScheduledPacket> third = buffer.WaitPop(0);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(first->packet.get()->dts, 0);
  EXPECT_EQ(second->sequence, 1U);
  EXPECT_EQ(third->sequence, 2U);
}

TEST(CompressedPacketDelayBufferTest, CancelWakesAWaitingConsumer) {
  CompressedPacketDelayBuffer buffer(1, std::chrono::seconds(1));
  auto pop =
      std::async(std::launch::async, [&buffer] { return buffer.WaitPop(0); });

  buffer.Cancel();
  EXPECT_FALSE(pop.get().has_value());
}

}  // namespace
}  // namespace mw::streamer::internal
