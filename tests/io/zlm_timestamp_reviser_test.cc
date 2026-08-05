#include "mw/input/internal/zlm_timestamp_reviser.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>

#include "Common/Stamp.h"
#include "Extension/Frame.h"

namespace {

using mw::streamer::input::internal::ReviseZlmFrameTimestamp;
using mw::streamer::input::internal::ShouldReviseZlmTimestamps;

mediakit::Frame::Ptr MakeFrame(std::uint64_t dts) {
  auto frame = mediakit::FrameImp::create();
  frame->_codec_id = mediakit::CodecH264;
  frame->_dts = dts;
  frame->_pts = dts;
  return frame;
}

}  // namespace

TEST_CASE("only RTSP inputs use ZLM relative timestamp revision") {
  CHECK(ShouldReviseZlmTimestamps("rtsp://camera/live"));
  CHECK(ShouldReviseZlmTimestamps("RTSPS://camera/live"));

  CHECK_FALSE(ShouldReviseZlmTimestamps("rtmp://server/live"));
  CHECK_FALSE(ShouldReviseZlmTimestamps("srt://server:9000"));
  CHECK_FALSE(ShouldReviseZlmTimestamps("/data/video.mp4"));
}

TEST_CASE("non-RTSP inputs preserve their original cross-track offset") {
  auto video = MakeFrame(1000);
  auto audio = MakeFrame(1125);

  auto revised_video = ReviseZlmFrameTimestamp(video, nullptr);
  auto revised_audio = ReviseZlmFrameTimestamp(audio, nullptr);

  CHECK(revised_video == video);
  CHECK(revised_audio == audio);
  CHECK(revised_audio->dts() - revised_video->dts() == 125);
}

TEST_CASE("RTSP inputs use ZLM Stamp to smooth timestamp jumps") {
  mediakit::Stamp stamp;
  CHECK(ReviseZlmFrameTimestamp(MakeFrame(1000), &stamp)->dts() == 0);
  CHECK(ReviseZlmFrameTimestamp(MakeFrame(1040), &stamp)->dts() == 40);
  CHECK(ReviseZlmFrameTimestamp(MakeFrame(1080), &stamp)->dts() == 80);

  SECTION("rollback") {
    CHECK(ReviseZlmFrameTimestamp(MakeFrame(500), &stamp)->dts() == 120);
    CHECK(ReviseZlmFrameTimestamp(MakeFrame(540), &stamp)->dts() == 160);
  }

  SECTION("forward jump") {
    CHECK(ReviseZlmFrameTimestamp(MakeFrame(5000), &stamp)->dts() == 120);
    CHECK(ReviseZlmFrameTimestamp(MakeFrame(5040), &stamp)->dts() == 160);
  }
}
