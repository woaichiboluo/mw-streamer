#ifndef MW_STREAMER_PERFORMANCE_CONFIG_H_
#define MW_STREAMER_PERFORMANCE_CONFIG_H_

#include <map>

namespace mw::streamer {

enum class PerformanceMode {
  kOff,
  // INFO interval reports and a final session summary.
  kSummary,
  // Summary reports plus TRACE details for individual samples.
  kDetail,
};

enum class PerformanceStage {
  kInput,
  kDecoderVideo,
  kDecoderAudio,
  kMediaFilterVideo,
  kMediaFilterAudio,
  kForceFps,
  kAudioResampler,
  kSyncerVideo,
  kSyncerAudio,
  kEncoderVideo,
  kEncoderAudio,
  kEncoderAudioFilter,
  kRemuxerConvert,
  kRemuxerOutput,
};

struct PerformanceConfig {
  // Omitted stages are disabled. Init copies the configuration for its entire
  // lifetime; changes to this map do not affect the active runtime. Performance
  // output shares the configured log sinks but ignores LogConfig::modules.
  std::map<PerformanceStage, PerformanceMode> stages;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PERFORMANCE_CONFIG_H_
