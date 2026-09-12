#ifndef MW_STREAMER_SINK_FATAL_ERROR_H_
#define MW_STREAMER_SINK_FATAL_ERROR_H_

#include <stdexcept>

namespace mw::streamer {

// Requests Pipeline-wide shutdown when propagated out of a Sink call.
class FatalError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SINK_FATAL_ERROR_H_
