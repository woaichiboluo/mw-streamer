#include <catch2/catch_test_macros.hpp>
#include <type_traits>

#include "mw/encoder/config.h"

namespace {

using mw::streamer::encoder::AudioEncoderConfig;
using mw::streamer::encoder::EncoderProperties;
using mw::streamer::encoder::VideoEncoderConfig;

static_assert(
    std::is_same_v<EncoderProperties, std::map<std::string, std::string>>);

}  // namespace

TEST_CASE("encoder configs are FFmpeg-independent value types") {
  const AudioEncoderConfig audio;
  const VideoEncoderConfig video;

  CHECK(audio.encoder_name.empty());
  CHECK(audio.properties.empty());
  CHECK(video.codec == kMwStreamerCodecH264);
  CHECK(video.encoder_name.empty());
  CHECK(video.frame_rate.num == 0);
  CHECK(video.frame_rate.den == 1);
  CHECK(video.properties.empty());
}
