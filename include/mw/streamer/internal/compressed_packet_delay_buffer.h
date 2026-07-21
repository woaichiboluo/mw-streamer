#ifndef MW_STREAMER_INTERNAL_COMPRESSED_PACKET_DELAY_BUFFER_H_
#define MW_STREAMER_INTERNAL_COMPRESSED_PACKET_DELAY_BUFFER_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "mw/streamer/ffmpeg/packet.h"

namespace mw::streamer::internal {

enum class CompressedMediaType {
  kVideo,
  kAudio,
};

struct ScheduledPacket {
  std::size_t input_index = 0;
  std::uint64_t generation = 0;
  CompressedMediaType media_type = CompressedMediaType::kVideo;
  ffmpeg::Packet packet;
  std::int64_t source_dts_us = 0;
  std::int64_t timeline_dts_us = 0;
  std::uint64_t sequence = 0;
};

class CompressedPacketDelayBuffer final {
 public:
  CompressedPacketDelayBuffer(std::size_t input_count,
                              std::chrono::milliseconds delay);

  CompressedPacketDelayBuffer(const CompressedPacketDelayBuffer&) = delete;
  CompressedPacketDelayBuffer& operator=(const CompressedPacketDelayBuffer&) =
      delete;
  CompressedPacketDelayBuffer(CompressedPacketDelayBuffer&&) = delete;
  CompressedPacketDelayBuffer& operator=(CompressedPacketDelayBuffer&&) =
      delete;

  [[nodiscard]] bool Push(ScheduledPacket packet);
  [[nodiscard]] std::optional<ScheduledPacket> WaitPop(std::size_t input_index);
  void Cancel() noexcept;

  [[nodiscard]] bool is_started() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> playback_cursor_us() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> last_timeline_dts_us(
      std::size_t input_index) const;

 private:
  struct InputState {
    std::map<std::pair<std::int64_t, std::uint64_t>, ScheduledPacket> packets;
    std::optional<std::int64_t> first_timeline_dts_us;
    std::optional<std::int64_t> maximum_timeline_dts_us;
    std::optional<std::int64_t> last_timeline_dts_us;
  };

  void TryStartLocked();
  [[nodiscard]] bool IsWarmedLocked(const InputState& input) const noexcept;

  const std::chrono::microseconds delay_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<InputState> inputs_;
  std::optional<std::chrono::steady_clock::time_point> steady_origin_;
  std::int64_t media_origin_us_ = 0;
  bool cancelled_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMPRESSED_PACKET_DELAY_BUFFER_H_
