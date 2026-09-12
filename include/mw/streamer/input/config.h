#ifndef MW_STREAMER_INPUT_CONFIG_H_
#define MW_STREAMER_INPUT_CONFIG_H_

#include <chrono>
#include <string>

#include "mw/streamer/zlm/config.h"

namespace mw::streamer {

struct ReconnectPolicy {
  // Number of retries after the initial attempt. A negative value retries
  // indefinitely.
  int max_retries = -1;
  std::chrono::milliseconds min_delay{2000};
  std::chrono::milliseconds max_delay{60000};
  std::chrono::milliseconds delay_step{3000};
};

struct FileInputConfig {
  std::string path;
};

struct ZlmInputConfig {
  std::string url;
  ReconnectPolicy reconnect_policy;
  PlayerConfig player;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_CONFIG_H_
