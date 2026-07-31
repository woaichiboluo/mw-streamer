#include "mw/zlm/internal/config_validator.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mw::streamer::zlm::internal {
namespace {

void ValidateTimeout(std::chrono::milliseconds timeout,
                     const char* error_message) {
  constexpr auto kMaximumTimeout =
      std::chrono::milliseconds{std::numeric_limits<int>::max()};
  if (timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumTimeout) {
    throw std::invalid_argument(error_message);
  }
}

}  // namespace

void ValidatePlayerConfig(const PlayerConfig& config) {
  ValidateTimeout(config.connect_timeout, "ZLM拉流连接超时时间超出有效范围");
  ValidateTimeout(config.media_timeout, "ZLM媒体超时时间超出有效范围");
}

void ValidateRecordingConfig(const RecordingConfig& config) {
  constexpr auto kMaximumDuration =
      std::chrono::milliseconds{std::numeric_limits<std::uint32_t>::max()};
  if (config.file_buffer_size == 0 ||
      config.file_buffer_size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("ZLM录像文件缓存大小超出有效范围");
  }
  if (config.hls_segment_duration <= std::chrono::milliseconds::zero() ||
      config.hls_segment_duration > kMaximumDuration) {
    throw std::invalid_argument("ZLM HLS分片时长超出有效范围");
  }
}

void ValidateOutputConfig(const OutputConfig& config) {
  constexpr auto kMaximumDuration =
      std::chrono::milliseconds{std::numeric_limits<std::uint32_t>::max()};
  ValidateTimeout(config.pusher.connect_timeout,
                  "ZLM推流连接超时时间超出有效范围");
  if (config.muxer.paced_sender_interval < std::chrono::milliseconds::zero() ||
      config.muxer.paced_sender_interval > kMaximumDuration) {
    throw std::invalid_argument("ZLM平滑发送间隔超出有效范围");
  }
  ValidateRecordingConfig(config.recording);
}

}  // namespace mw::streamer::zlm::internal
