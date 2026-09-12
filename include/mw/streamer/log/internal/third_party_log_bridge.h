#ifndef MW_STREAMER_LOG_INTERNAL_THIRD_PARTY_LOG_BRIDGE_H_
#define MW_STREAMER_LOG_INTERNAL_THIRD_PARTY_LOG_BRIDGE_H_

#include <memory>

namespace mw::streamer::internal {

class ThirdPartyLogBridge {
 public:
  ThirdPartyLogBridge();
  ~ThirdPartyLogBridge();

  ThirdPartyLogBridge(const ThirdPartyLogBridge&) = delete;
  ThirdPartyLogBridge& operator=(const ThirdPartyLogBridge&) = delete;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_LOG_INTERNAL_THIRD_PARTY_LOG_BRIDGE_H_
