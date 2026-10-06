#include "mw/streamer/ffmpeg/stream_info.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;

ffmpeg::StreamInfo VideoStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = {1, 90000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->width = 1920;
  parameters->height = 1080;
  parameters->framerate = {30000, 1001};
  return stream;
}

ffmpeg::StreamInfo AudioStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 1;
  stream.time_base = {1, 48000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_AAC;
  parameters->format = AV_SAMPLE_FMT_FLTP;
  parameters->sample_rate = 48000;
  av_channel_layout_default(&parameters->ch_layout, 2);
  return stream;
}

void SetExtradata(ffmpeg::StreamInfo& stream,
                  std::initializer_list<std::uint8_t> bytes) {
  auto* parameters = stream.codec_parameters.get();
  av_freep(&parameters->extradata);
  parameters->extradata = static_cast<std::uint8_t*>(
      av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  REQUIRE(parameters->extradata != nullptr);
  std::memcpy(parameters->extradata, bytes.begin(), bytes.size());
  parameters->extradata_size = static_cast<int>(bytes.size());
}

}  // namespace

TEST_CASE("StreamInfo比较公共轨道信息") {
  const auto original = VideoStream();
  auto changed = original;
  REQUIRE(original == changed);
  SECTION("轨道索引") { ++changed.stream_index; }
  SECTION("时间基") { changed.time_base = {1, 1000}; }
  SECTION("媒体类型") {
    changed.codec_parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  }
  SECTION("编解码类型") {
    changed.codec_parameters.get()->codec_id = AV_CODEC_ID_HEVC;
  }
  CHECK_FALSE(original == changed);
  CHECK_FALSE(changed == original);
}

TEST_CASE("StreamInfo比较视频格式和尺寸") {
  const auto original = VideoStream();
  auto changed = original;
  auto* parameters = changed.codec_parameters.get();
  SECTION("像素格式") { parameters->format = AV_PIX_FMT_NV12; }
  SECTION("宽度") { parameters->width = 1280; }
  SECTION("高度") { parameters->height = 720; }
  CHECK_FALSE(original == changed);
}

TEST_CASE("StreamInfo按有理数语义比较时间基和帧率") {
  auto original = VideoStream();
  auto changed = original;
  SECTION("等值时间基") {
    changed.time_base = {2, 180000};
    CHECK(original == changed);
  }
  SECTION("等值帧率") {
    changed.codec_parameters.get()->framerate = {60000, 2002};
    CHECK(original == changed);
  }
  SECTION("不同帧率") {
    changed.codec_parameters.get()->framerate = {30, 1};
    CHECK_FALSE(original == changed);
  }
  SECTION("未知帧率的不同表示") {
    original.codec_parameters.get()->framerate = {0, 0};
    changed.codec_parameters.get()->framerate = {0, 1};
    CHECK(original == changed);
    changed.codec_parameters.get()->framerate = {0, 1000};
    CHECK(original == changed);
  }
  SECTION("未知与已知帧率") {
    changed.codec_parameters.get()->framerate = {0, 1};
    CHECK_FALSE(original == changed);
  }
}

TEST_CASE("StreamInfo比较音频格式采样率和声道布局") {
  const auto original = AudioStream();
  auto changed = original;
  REQUIRE(original == changed);
  auto* parameters = changed.codec_parameters.get();
  SECTION("样本格式") { parameters->format = AV_SAMPLE_FMT_FLT; }
  SECTION("采样率") { parameters->sample_rate = 44100; }
  SECTION("声道数量") {
    av_channel_layout_uninit(&parameters->ch_layout);
    av_channel_layout_default(&parameters->ch_layout, 1);
  }
  CHECK_FALSE(original == changed);

  auto surround = AudioStream();
  auto back_surround = AudioStream();
  const AVChannelLayout side_layout = AV_CHANNEL_LAYOUT_5POINT1;
  const AVChannelLayout back_layout = AV_CHANNEL_LAYOUT_5POINT1_BACK;
  REQUIRE(av_channel_layout_copy(&surround.codec_parameters.get()->ch_layout,
                                 &side_layout) == 0);
  REQUIRE(av_channel_layout_copy(
              &back_surround.codec_parameters.get()->ch_layout, &back_layout) ==
          0);
  REQUIRE(side_layout.nb_channels == back_layout.nb_channels);
  CHECK_FALSE(surround == back_surround);
  CHECK(surround == ffmpeg::StreamInfo(surround));
}

TEST_CASE("StreamInfo忽略编解码元数据和初始化数据变化") {
  auto original = VideoStream();
  SetExtradata(original, {1, 2, 3});
  auto changed = original;
  auto* parameters = changed.codec_parameters.get();
  parameters->codec_tag = 123;
  parameters->profile = AV_PROFILE_H264_HIGH;
  parameters->level = 51;
  parameters->bit_rate = 8000000;
  parameters->color_space = AVCOL_SPC_BT709;
  parameters->color_range = AVCOL_RANGE_JPEG;
  parameters->color_primaries = AVCOL_PRI_BT2020;
  parameters->color_trc = AVCOL_TRC_SMPTE2084;
  parameters->sample_aspect_ratio = {4, 3};
  auto* side_data = av_packet_side_data_new(
      &parameters->coded_side_data, &parameters->nb_coded_side_data,
      AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t), 0);
  REQUIRE(side_data != nullptr);
  std::memset(side_data->data, 0, side_data->size);
  SetExtradata(changed, {3, 2, 1});
  CHECK(original == changed);
  SetExtradata(changed, {9});
  CHECK(original == changed);
}

TEST_CASE("StreamInfo只比较对应媒体类型的专用属性") {
  SECTION("视频忽略音频属性") {
    const auto original = VideoStream();
    auto changed = original;
    changed.codec_parameters.get()->sample_rate = 44100;
    av_channel_layout_default(&changed.codec_parameters.get()->ch_layout, 6);
    CHECK(original == changed);
  }
  SECTION("音频忽略视频属性") {
    const auto original = AudioStream();
    auto changed = original;
    auto* parameters = changed.codec_parameters.get();
    parameters->width = 1280;
    parameters->height = 720;
    parameters->framerate = {60, 1};
    CHECK(original == changed);
  }
  SECTION("其他媒体类型只比较公共信息") {
    auto original = VideoStream();
    original.codec_parameters.get()->codec_type = AVMEDIA_TYPE_SUBTITLE;
    original.codec_parameters.get()->codec_id = AV_CODEC_ID_SUBRIP;
    auto changed = original;
    auto* parameters = changed.codec_parameters.get();
    parameters->format = AV_PIX_FMT_RGB24;
    parameters->width = 1;
    parameters->height = 1;
    parameters->framerate = {1, 1};
    parameters->sample_rate = 44100;
    av_channel_layout_default(&parameters->ch_layout, 1);
    CHECK(original == changed);
    parameters->codec_id = AV_CODEC_ID_ASS;
    CHECK_FALSE(original == changed);
  }
}

TEST_CASE("StreamInfo支持轨道快照容器比较") {
  const std::vector<ffmpeg::StreamInfo> original{VideoStream(), AudioStream()};
  auto changed = original;
  changed[0].time_base = {2, 180000};
  changed[0].codec_parameters.get()->framerate = {60000, 2002};
  CHECK(original == changed);
  SECTION("轨道参数变化") {
    changed[1].codec_parameters.get()->sample_rate = 44100;
  }
  SECTION("轨道数量变化") { changed.pop_back(); }
  SECTION("轨道顺序变化") { std::swap(changed[0], changed[1]); }
  CHECK(original != changed);
}
