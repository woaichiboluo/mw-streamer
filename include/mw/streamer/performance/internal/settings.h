#ifndef MW_STREAMER_PERFORMANCE_INTERNAL_SETTINGS_H_
#define MW_STREAMER_PERFORMANCE_INTERNAL_SETTINGS_H_

#include "mw/streamer/performance/config.h"

namespace mw::streamer::internal {

struct PerformanceStageNames {
  const char* module;
  const char* stage;
};

// Only valid, built-in stages may be used by instrumentation.
const PerformanceStageNames& GetPerformanceStageNames(PerformanceStage stage);

// Init owns one immutable configuration. Stop and destroy instrumented objects
// before destruction. Tests may own settings without initializing media
// runtimes.
class PerformanceSettings final {
 public:
  // Invalid enum values throw invalid_argument; an active instance throws
  // logic_error. Validation finishes before the configuration is published.
  explicit PerformanceSettings(const PerformanceConfig& config);
  ~PerformanceSettings();
  PerformanceSettings(const PerformanceSettings&) = delete;
  PerformanceSettings& operator=(const PerformanceSettings&) = delete;

  PerformanceMode mode(PerformanceStage stage) const noexcept;

 private:
  const PerformanceConfig config_;
};

// Returns kOff when no runtime is active. Safe for concurrent stage startup
// while the settings owner remains alive; does not support dynamic updates.
PerformanceMode GetPerformanceMode(PerformanceStage stage) noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PERFORMANCE_INTERNAL_SETTINGS_H_
