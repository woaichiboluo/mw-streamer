#include "mw/streamer/internal/output_worker.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
}

namespace mw::streamer::internal {
namespace {

EncodedPacket MakePacket(EncodedMediaType media_type, std::int64_t dts,
                         bool is_keyframe = false) {
  ffmpeg::Packet packet;
  packet.get()->dts = dts;
  packet.get()->pts = dts;
  packet.get()->time_base = AVRational{1, 1'000};
  if (is_keyframe) {
    packet.get()->flags |= AV_PKT_FLAG_KEY;
  }
  return EncodedPacket(media_type, std::move(packet));
}

ffmpeg::CodecParameters MakeH264Parameters() {
  ffmpeg::CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters.get()->codec_id = AV_CODEC_ID_H264;
  parameters.get()->width = 160;
  parameters.get()->height = 144;
  return parameters;
}

TEST(OutputPacketGateTest, StartsEachSessionAtVideoKeyframe) {
  OutputPacketGate gate;

  EXPECT_FALSE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kAudio, 90, false)));
  EXPECT_FALSE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kVideo, 100, false)));
  EXPECT_TRUE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kVideo, 120, true)));
  EXPECT_FALSE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kAudio, 119, false)));
  EXPECT_TRUE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kAudio, 120, false)));
  EXPECT_TRUE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kVideo, 140, false)));
}

TEST(OutputPacketGateTest, ResetRequiresANewVideoKeyframe) {
  OutputPacketGate gate;
  ASSERT_TRUE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kVideo, 100, true)));

  gate.Reset();

  EXPECT_FALSE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kAudio, 110, false)));
  EXPECT_FALSE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kVideo, 120, false)));
  EXPECT_TRUE(
      gate.ShouldWrite(MakePacket(EncodedMediaType::kVideo, 140, true)));
}

TEST(OutputWorkerTest, UnreachableNetworkOutputQueuesWithoutBlockingAndStops) {
  OutputConfig config;
  config.id = "unreachable";
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:1/missing";
  config.packet_queue_capacity = 1;
  config.reconnect.max_retries = 1;
  config.reconnect.initial_backoff = std::chrono::seconds(30);
  config.reconnect.maximum_backoff = std::chrono::seconds(30);
  config.reconnect.jitter_ratio = 0.0;

  std::mutex events_mutex;
  std::vector<EndpointEventType> events;
  ffmpeg::CodecParameters parameters = MakeH264Parameters();
  OutputWorker worker(
      config, parameters, AVRational{1, 25}, nullptr, AVRational{0, 1},
      [&events_mutex, &events](EndpointEventType type, const Status&) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(type);
      });
  ASSERT_NO_THROW(worker.Start());

  EncodedPacket first = MakePacket(EncodedMediaType::kVideo, 0, true);
  EncodedPacket second = MakePacket(EncodedMediaType::kVideo, 40, false);
  const auto enqueue_start = std::chrono::steady_clock::now();
  EXPECT_TRUE(worker.Enqueue(first));
  EXPECT_FALSE(worker.Enqueue(second));
  const auto enqueue_duration =
      std::chrono::steady_clock::now() - enqueue_start;
  EXPECT_LT(enqueue_duration, std::chrono::milliseconds(100));

  const auto stop_start = std::chrono::steady_clock::now();
  worker.Stop();
  const auto stop_duration = std::chrono::steady_clock::now() - stop_start;
  EXPECT_LT(stop_duration, std::chrono::seconds(1));
  EXPECT_FALSE(worker.Enqueue(first));

  std::lock_guard<std::mutex> lock(events_mutex);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front(), EndpointEventType::kReconnecting);
}

}  // namespace
}  // namespace mw::streamer::internal
