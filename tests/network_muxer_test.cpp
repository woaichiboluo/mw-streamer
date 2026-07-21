#include "mw/streamer/internal/network_muxer.h"

#include <string>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {
namespace {

ffmpeg::CodecParameters MakeVideoParameters(AVCodecID codec_id) {
  ffmpeg::CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters.get()->codec_id = codec_id;
  parameters.get()->format = AV_PIX_FMT_NV12;
  parameters.get()->width = 160;
  parameters.get()->height = 144;
  return parameters;
}

TEST(NetworkMuxerTest, RejectsFileProtocol) {
  OutputConfig config;
  config.protocol = OutputProtocol::kFile;
  config.url = "unused.mp4";
  const ffmpeg::CodecParameters parameters =
      MakeVideoParameters(AV_CODEC_ID_H264);

  NetworkMuxer muxer;
  try {
    muxer.Open(config, parameters, AVRational{1, 25});
    FAIL() << "expected file protocol rejection";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kInvalidArgument);
  }
}

TEST(NetworkMuxerTest, RejectsEmptyUrl) {
  OutputConfig config;
  config.protocol = OutputProtocol::kRtmp;
  const ffmpeg::CodecParameters parameters =
      MakeVideoParameters(AV_CODEC_ID_H264);

  NetworkMuxer muxer;
  EXPECT_THROW(muxer.Open(config, parameters, AVRational{1, 25}), Error);
}

TEST(NetworkMuxerTest, RejectsUnsupportedVideoCodec) {
  OutputConfig config;
  config.protocol = OutputProtocol::kSrt;
  config.url = "srt://127.0.0.1:1";
  const ffmpeg::CodecParameters parameters =
      MakeVideoParameters(AV_CODEC_ID_VP9);

  NetworkMuxer muxer;
  try {
    muxer.Open(config, parameters, AVRational{1, 25});
    FAIL() << "expected video codec rejection";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kUnsupported);
  }
}

TEST(NetworkMuxerTest, RejectsInvalidTimeBaseBeforeConnecting) {
  OutputConfig config;
  config.protocol = OutputProtocol::kRtsp;
  config.url = "rtsp://127.0.0.1:1/test";
  const ffmpeg::CodecParameters parameters =
      MakeVideoParameters(AV_CODEC_ID_H264);

  NetworkMuxer muxer;
  try {
    muxer.Open(config, parameters, AVRational{0, 1});
    FAIL() << "expected time base rejection";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kInvalidArgument);
  }
}

TEST(NetworkMuxerTest, OpenIsSingleUseAfterFailure) {
  OutputConfig config;
  config.protocol = OutputProtocol::kRtmp;
  const ffmpeg::CodecParameters parameters =
      MakeVideoParameters(AV_CODEC_ID_H264);

  NetworkMuxer muxer;
  EXPECT_THROW(muxer.Open(config, parameters, AVRational{1, 25}), Error);
  config.url = "rtmp://127.0.0.1:1/test";
  try {
    muxer.Open(config, parameters, AVRational{1, 25});
    FAIL() << "expected single-use rejection";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kInvalidArgument);
  }
}

}  // namespace
}  // namespace mw::streamer::internal
