#ifndef MW_STREAMER_REMUXER_OUTPUT_INFO_H_
#define MW_STREAMER_REMUXER_OUTPUT_INFO_H_

#include <string>
#include <vector>

namespace mw::streamer {

// Planned output addresses before Start, and actual addresses during/after the
// latest session. Stop retains this snapshot but clears configured outputs.
struct RemuxerOutputInfo {
  std::vector<std::string> output_urls;
  std::string rtsp_url;
  std::string hls_url;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_REMUXER_OUTPUT_INFO_H_
