#include "streamer/performance/settings.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>

namespace mw::streamer::internal {
namespace {

constexpr std::array<const char*, 15> kStageNames{{
    "read.packet",
    "decode.video",
    "decode.audio",
    "filter.video",
    "filter.audio",
    "filter.force_fps",
    "filter.audio_resampler",
    "syncer.video",
    "syncer.audio",
    "encode.video",
    "encode.audio",
    "filter.encoder.audio",
    "remux.convert",
    "remux.output",
    "live.pipeline",
}};
static_assert(static_cast<std::size_t>(PerformanceStage::kLivePipeline) + 1 ==
              kStageNames.size());

std::atomic<const PerformanceSettings*> g_settings{nullptr};

}  // namespace

const char* GetPerformanceStageName(PerformanceStage stage) {
  return kStageNames.at(static_cast<std::size_t>(stage));
}

PerformanceSettings::PerformanceSettings(const PerformanceConfig& config)
    : config_(config) {
  for (const auto& [stage, mode] : config_.stages) {
    if (static_cast<std::size_t>(stage) >= kStageNames.size()) {
      throw std::invalid_argument("无效的性能统计阶段");
    }
    if (mode != PerformanceMode::kOff && mode != PerformanceMode::kSummary &&
        mode != PerformanceMode::kDetail) {
      throw std::invalid_argument("无效的性能统计模式");
    }
  }
  const PerformanceSettings* expected = nullptr;
  if (!g_settings.compare_exchange_strong(expected, this,
                                          std::memory_order_release,
                                          std::memory_order_acquire)) {
    throw std::logic_error("性能统计配置已初始化");
  }
}

PerformanceSettings::~PerformanceSettings() {
  g_settings.store(nullptr, std::memory_order_release);
}

PerformanceMode PerformanceSettings::mode(
    PerformanceStage stage) const noexcept {
  const auto found = config_.stages.find(stage);
  return found == config_.stages.end() ? PerformanceMode::kOff : found->second;
}

PerformanceMode GetPerformanceMode(PerformanceStage stage) noexcept {
  const auto* settings = g_settings.load(std::memory_order_acquire);
  return settings ? settings->mode(stage) : PerformanceMode::kOff;
}

}  // namespace mw::streamer::internal
