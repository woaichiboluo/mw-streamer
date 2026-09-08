#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Common/MediaSource.h"
#include "Rtmp/RtmpMediaSourceMuxer.h"
#include "TS/TSMediaSourceMuxer.h"
#include "ext-codec/H264.h"
#include "ext-codec/H265.h"
#include "srt/SrtPusher.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {

template <typename Source>
auto CachedPackets(const Source& source) {
  using Packet = typename Source::RingDataType::element_type::value_type;
  std::vector<Packet> packets;
  source.getRing()->flushGop([&](const auto& batch) {
    batch->for_each([&](const auto& packet) { packets.push_back(packet); });
  });
  return packets;
}

mediakit::RtmpPacket::Ptr AudioPacket(uint32_t timestamp) {
  auto packet = mediakit::RtmpPacket::create();
  packet->type_id = MSG_AUDIO;
  packet->time_stamp = timestamp;
  packet->buffer.assign("\xaf\x01\x00", 3);
  return packet;
}

mediakit::RtmpPacket::Ptr KeyPacket(uint32_t timestamp, bool hevc) {
  auto packet = mediakit::RtmpPacket::create();
  packet->type_id = MSG_VIDEO;
  packet->time_stamp = timestamp;
  // Only the FLV packet header is interpreted by the source's GOP cache.
  packet->buffer.assign(hevc ? "\x1c\x01\x00\x00\x00" : "\x17\x01\x00\x00\x00",
                        5);
  return packet;
}

mediakit::TSPacket::Ptr TsPacket(uint64_t timestamp) {
  auto buffer = std::make_shared<toolkit::BufferString>(std::string(188, '\0'));
  buffer->data()[0] = '\x47';
  auto packet = std::make_shared<mediakit::TSPacket>(std::move(buffer));
  packet->time_stamp = timestamp;
  return packet;
}

class InspectableSrtPusher final : public mediakit::SrtPusherImp {
 public:
  using mediakit::SrtPusherImp::SrtPusherImp;

  bool waiting_for_key() const { return _wait_for_key; }
};

}  // namespace

TEST_CASE("RTMP output preserves audio preceding the first video GOP",
          "[remux][startup_cache]") {
  const bool preserve = GENERATE(false, true);
  const bool hevc = GENERATE(false, true);
  CAPTURE(preserve, hevc);
  mediakit::ProtocolOption option;
  option.preserve_startup_packets = preserve;
  auto muxer = std::make_shared<mediakit::RtmpMediaSourceMuxer>(
      mediakit::MediaTuple("__defaultVhost__", "test", "rtmp-startup"), option);
  auto source = std::static_pointer_cast<mediakit::RtmpMediaSource>(
      muxer->getMediaSource());
  AMFValue metadata(AMF_OBJECT);
  metadata.set("videocodecid", hevc ? 12 : 7);
  metadata.set("audiocodecid", 10);
  source->setMetaData(metadata);

  std::vector<mediakit::RtmpPacket::Ptr> expected;
  for (const uint32_t timestamp : {0, 21, 42, 64, 85}) {
    expected.push_back(AudioPacket(timestamp));
    source->onWrite(expected.back());
  }
  REQUIRE(CachedPackets(*source) == expected);
  expected.push_back(KeyPacket(21, hevc));
  source->onWrite(expected.back());
  if (preserve) {
    REQUIRE(CachedPackets(*source) == expected);
  } else {
    REQUIRE(CachedPackets(*source) ==
            std::vector<mediakit::RtmpPacket::Ptr>{expected.back()});
  }

  // Only the initial boundary changes; a later GOP still replaces the old one.
  auto next_key = KeyPacket(10021, hevc);
  source->onWrite(next_key);
  REQUIRE(CachedPackets(*source) ==
          std::vector<mediakit::RtmpPacket::Ptr>{next_key});
}

TEST_CASE("TS output knows its video track before the first video packet",
          "[remux][startup_cache]") {
  const bool preserve = GENERATE(false, true);
  const bool hevc = GENERATE(false, true);
  CAPTURE(preserve, hevc);
  mediakit::ProtocolOption option;
  option.preserve_startup_packets = preserve;
  auto muxer = std::make_shared<mediakit::TSMediaSourceMuxer>(
      mediakit::MediaTuple("__defaultVhost__", "test", "ts-startup"), option);
  mediakit::Track::Ptr track;
  if (hevc) {
    track = std::make_shared<mediakit::H265Track>();
  } else {
    track = std::make_shared<mediakit::H264Track>();
  }
  REQUIRE(muxer->addTrack(track));
  auto source = std::static_pointer_cast<mediakit::TSMediaSource>(
      muxer->getMediaSource());

  // Inject already-muxed packets: no video frame or key packet has arrived yet.
  std::vector<mediakit::TSPacket::Ptr> expected;
  for (const uint64_t timestamp : {0, 21, 42, 64, 85}) {
    expected.push_back(TsPacket(timestamp));
    source->onWrite(expected.back(), false);
  }
  if (preserve) {
    REQUIRE(CachedPackets(*source) == expected);
  } else {
    REQUIRE(CachedPackets(*source) ==
            std::vector<mediakit::TSPacket::Ptr>{expected.back()});
  }
  expected.push_back(TsPacket(21));
  source->onWrite(expected.back(), true);
  if (preserve) {
    REQUIRE(CachedPackets(*source) == expected);
  } else {
    REQUIRE(CachedPackets(*source) ==
            std::vector<mediakit::TSPacket::Ptr>{expected.back()});
  }
  auto next_key = TsPacket(10021);
  source->onWrite(next_key, true);
  REQUIRE(CachedPackets(*source) ==
          std::vector<mediakit::TSPacket::Ptr>{next_key});
}

TEST_CASE("Audio-only TS output retains the existing rolling cache behavior",
          "[remux][startup_cache]") {
  mediakit::ProtocolOption option;
  option.preserve_startup_packets = true;
  auto muxer = std::make_shared<mediakit::TSMediaSourceMuxer>(
      mediakit::MediaTuple("__defaultVhost__", "test", "ts-audio-startup"),
      option);
  auto source = std::static_pointer_cast<mediakit::TSMediaSource>(
      muxer->getMediaSource());
  for (const uint64_t timestamp : {0, 21, 42, 64, 85}) {
    auto packet = TsPacket(timestamp);
    source->onWrite(packet, true);
    REQUIRE(CachedPackets(*source) ==
            std::vector<mediakit::TSPacket::Ptr>{packet});
  }
}

TEST_CASE("SRT publisher accepts a retained audio prefix before the first key",
          "[remux][startup_cache]") {
  const bool preserve = GENERATE(false, true);
  const bool clear_cache = GENERATE(false, true);
  CAPTURE(preserve, clear_cache);
  auto source = std::make_shared<mediakit::TSMediaSource>(
      mediakit::MediaTuple("__defaultVhost__", "test", "srt-startup"),
      TS_GOP_SIZE, preserve);
  source->setHaveVideo(true);
  for (const uint64_t timestamp : {0, 21, 42, 64, 85}) {
    source->onWrite(TsPacket(timestamp), false);
  }
  if (clear_cache) {
    source->clearCache();
  }

  auto poller = toolkit::EventPollerPool::Instance().getPoller();
  auto pusher = std::make_shared<InspectableSrtPusher>(poller, source);
  bool waiting_for_key = true;
  poller->sync([&] {
    // Consume the real startup cache/read callback without opening a socket.
    // SrtCaller rejects network writes while disconnected; this checks the
    // publisher's preceding key-frame gate independently of network behavior.
    pusher->doPublish();
    waiting_for_key = pusher->waiting_for_key();
    pusher->teardown();
  });
  REQUIRE(waiting_for_key == (!preserve || clear_cache));
}
