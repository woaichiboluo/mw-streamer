#include "mw/streamer/internal/input_options.h"

#include <chrono>

#include "gtest/gtest.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {
namespace {

TEST(InputOptionsTest, PreservesOptionsAndAppliesStrongRtspTransport) {
  InputConfig config;
  config.protocol = InputProtocol::kRtsp;
  config.rtsp_transport = RtspTransport::kUdp;
  config.open_timeout = std::chrono::seconds(3);
  config.read_timeout = std::chrono::seconds(4);
  config.options["rtsp_transport"] = "tcp";
  config.options["allowed_media_types"] = "video+audio";
  FfmpegInterrupt interrupt;

  const DemuxerOpenOptions result = MakeDemuxerOpenOptions(config, &interrupt);

  EXPECT_EQ(result.interrupt, &interrupt);
  EXPECT_EQ(result.open_timeout, std::chrono::seconds(3));
  EXPECT_EQ(result.read_timeout, std::chrono::seconds(4));
  EXPECT_EQ(result.options.at("rtsp_transport"), "udp");
  EXPECT_EQ(result.options.at("allowed_media_types"), "video+audio");
}

TEST(InputOptionsTest, DoesNotInjectRtspOptionsForOtherProtocols) {
  InputConfig config;
  config.protocol = InputProtocol::kSrt;
  FfmpegInterrupt interrupt;

  const DemuxerOpenOptions result = MakeDemuxerOpenOptions(config, &interrupt);

  EXPECT_EQ(result.options.count("rtsp_transport"), 0U);
}

TEST(InputOptionsTest, RejectsMissingInterruptController) {
  EXPECT_THROW(MakeDemuxerOpenOptions(InputConfig{}, nullptr), Error);
}

}  // namespace
}  // namespace mw::streamer::internal
