#ifndef MW_STREAMER_INCLUDE_MW_INPUT_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_INPUT_CONFIG_H_

#include <chrono>

namespace mw::streamer::input {

struct ReconnectPolicy {
  // Number of retries after the initial attempt. A negative value retries
  // indefinitely.
  int max_retries = -1;
  std::chrono::milliseconds min_delay{2000};
  std::chrono::milliseconds max_delay{60000};
  std::chrono::milliseconds delay_step{3000};
};

}  // namespace mw::streamer::input

#endif  // MW_STREAMER_INCLUDE_MW_INPUT_CONFIG_H_
