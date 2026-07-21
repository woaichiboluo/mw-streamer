#include <algorithm>
#include <cstdint>
#include <type_traits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

#include "gtest/gtest.h"
#include "mw/streamer/ffmpeg/channel_layout.h"
#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/dictionary.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"

namespace mw::streamer::ffmpeg {
namespace {

static_assert(!std::is_copy_constructible_v<Packet>);
static_assert(std::is_nothrow_move_constructible_v<Packet>);
static_assert(!std::is_copy_constructible_v<Frame>);
static_assert(std::is_nothrow_move_constructible_v<Frame>);
static_assert(!std::is_copy_constructible_v<CodecParameters>);
static_assert(std::is_nothrow_move_constructible_v<CodecParameters>);
static_assert(!std::is_copy_constructible_v<ChannelLayout>);
static_assert(std::is_nothrow_move_constructible_v<ChannelLayout>);

TEST(FfmpegRaiiTest, PacketRefSharesReferencedPayload) {
  Packet packet;
  ASSERT_EQ(av_new_packet(packet.get(), 16), 0);
  packet.get()->data[0] = 42;

  Packet reference = packet.Ref();

  EXPECT_NE(reference.get()->buf, nullptr);
  EXPECT_EQ(reference.get()->data, packet.get()->data);
  EXPECT_EQ(reference.get()->data[0], 42);
}

TEST(FfmpegRaiiTest, FrameRefSharesReferencedBuffer) {
  Frame frame;
  frame.get()->format = AV_PIX_FMT_GRAY8;
  frame.get()->width = 16;
  frame.get()->height = 16;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 0), 0);
  frame.get()->data[0][0] = 23;

  Frame reference = frame.Ref();

  EXPECT_EQ(reference.get()->data[0], frame.get()->data[0]);
  EXPECT_EQ(reference.get()->data[0][0], 23);
}

TEST(FfmpegRaiiTest, CodecParametersCloneOwnsIndependentExtradata) {
  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters.get()->codec_id = AV_CODEC_ID_H264;
  parameters.get()->extradata_size = 4;
  parameters.get()->extradata = static_cast<std::uint8_t*>(av_mallocz(
      parameters.get()->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
  ASSERT_NE(parameters.get()->extradata, nullptr);
  parameters.get()->extradata[0] = 7;

  CodecParameters clone = parameters.Clone();

  ASSERT_NE(clone.get()->extradata, nullptr);
  EXPECT_NE(clone.get()->extradata, parameters.get()->extradata);
  EXPECT_EQ(clone.get()->extradata[0], 7);
}

TEST(FfmpegRaiiTest, ChannelLayoutCloneOwnsEquivalentLayout) {
  ChannelLayout layout(2);

  ChannelLayout clone = layout.Clone();

  EXPECT_EQ(av_channel_layout_compare(layout.get(), clone.get()), 0);
  EXPECT_EQ(clone.get()->nb_channels, 2);
}

TEST(FfmpegRaiiTest, DictionaryPreservesExplicitEntries) {
  Dictionary dictionary;
  dictionary.Set("rtsp_transport", "tcp");
  dictionary.Set("timeout", "1000");

  const AVDictionaryEntry* transport =
      av_dict_get(dictionary.get(), "rtsp_transport", nullptr, 0);
  ASSERT_NE(transport, nullptr);
  EXPECT_STREQ(transport->value, "tcp");
  const auto keys = dictionary.keys();
  EXPECT_NE(std::find(keys.begin(), keys.end(), "rtsp_transport"), keys.end());
  EXPECT_NE(std::find(keys.begin(), keys.end(), "timeout"), keys.end());
}

}  // namespace
}  // namespace mw::streamer::ffmpeg
