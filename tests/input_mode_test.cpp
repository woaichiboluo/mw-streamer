#include "mw/streamer/internal/input_mode.h"

#include "gtest/gtest.h"

namespace mw::streamer::internal {
namespace {

TEST(InputModeTest, ClassifiesNetworkAndFileProtocols) {
  InputConfig input;
  for (InputProtocol protocol :
       {InputProtocol::kRtmp, InputProtocol::kRtsp, InputProtocol::kSrt}) {
    input.protocol = protocol;
    EXPECT_TRUE(IsLiveInput(input));
    EXPECT_FALSE(IsFiniteInput(input));
  }

  input.protocol = InputProtocol::kFile;
  EXPECT_FALSE(IsLiveInput(input));
  EXPECT_TRUE(IsFiniteInput(input));
}

TEST(InputModeTest, ClassifiesHlsUsingExplicitMode) {
  InputConfig input;
  input.protocol = InputProtocol::kHls;
  EXPECT_EQ(input.hls_mode, HlsMode::kLive);
  EXPECT_TRUE(IsLiveInput(input));
  EXPECT_FALSE(IsFiniteInput(input));

  input.hls_mode = HlsMode::kVod;
  EXPECT_FALSE(IsLiveInput(input));
  EXPECT_TRUE(IsFiniteInput(input));
}

}  // namespace
}  // namespace mw::streamer::internal
