#include "streamer/performance/settings.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>

namespace mw::streamer::internal {
namespace {

constexpr std::array<PerformanceStageNames, 14> kStageNames{{
    {"perf.input", "read.packet"},
    {"perf.decoder.video", "decode.video"},
    {"perf.decoder.audio", "decode.audio"},
    {"perf.filter.video", "filter.video"},
    {"perf.filter.audio", "filter.audio"},
    {"perf.filter.force_fps", "filter.force_fps"},
    {"perf.filter.audio_resampler", "filter.audio_resampler"},
    {"perf.syncer.video", "syncer.video"},
    {"perf.syncer.audio", "syncer.audio"},
    {"perf.encoder.video", "encode.video"},
    {"perf.encoder.audio", "encode.audio"},
    {"perf.filter.encoder.audio", "filter.encoder.audio"},
    {"perf.remuxer.convert", "remux.convert"},
    {"perf.remuxer.output", "remux.output"},
}};
static_assert(static_cast<std::size_t>(PerformanceStage::kRemuxerOutput) + 1 ==
              kStageNames.size());

std::atomic<const PerformanceSettings*> g_settings{nullptr};

}  // namespace

const PerformanceStageNames& GetPerformanceStageNames(PerformanceStage stage) {
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
