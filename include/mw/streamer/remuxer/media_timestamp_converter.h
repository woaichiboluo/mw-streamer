#ifndef MW_STREAMER_REMUXER_MEDIA_TIMESTAMP_CONVERTER_H_
#define MW_STREAMER_REMUXER_MEDIA_TIMESTAMP_CONVERTER_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

struct MediaTimestamps {
  std::uint64_t pts_ms;
  std::uint64_t dts_ms;
};

// Protocol adaptation only: preserve native timestamps until the ZLM boundary,
// then apply one nonnegative millisecond shift shared by every track.
class MediaTimestampConverter final {
 public:
  explicit MediaTimestampConverter(
      const std::vector<ffmpeg::StreamInfo>& streams)
      : streams_(streams) {
    if (streams.empty()) throw std::invalid_argument("媒体轨道不能为空");
    for (std::size_t i = 0; i < streams.size(); ++i) {
      streams[i].Validate();
      for (std::size_t j = 0; j < i; ++j)
        if (streams[i].stream_index == streams[j].stream_index)
          throw std::invalid_argument("媒体轨道索引重复");
    }
  }

  void Validate(const ffmpeg::Packet& packet) const {
    const auto* input = packet.get();
    if (!input || input->pts == AV_NOPTS_VALUE || input->dts == AV_NOPTS_VALUE)
      throw std::invalid_argument("媒体包需要有效 PTS 和 DTS");
    const auto track =
        std::find_if(streams_.begin(), streams_.end(), [&](const auto& stream) {
          return stream.stream_index == input->stream_index;
        });
    if (track == streams_.end() || input->time_base.num <= 0 ||
        input->time_base.den <= 0 ||
        av_cmp_q(input->time_base, track->time_base) != 0)
      throw std::invalid_argument("媒体包轨道或时间基不匹配");
  }

  void Initialize(const std::vector<ffmpeg::Packet>& first_packets) {
    if (shift_ms_) return;
    std::int64_t minimum = 0;
    for (const auto& packet : first_packets) {
      Validate(packet);
      minimum = std::min({minimum, Milliseconds(packet->pts, packet->time_base),
                          Milliseconds(packet->dts, packet->time_base)});
    }
    shift_ms_ = -minimum;
  }

  bool initialized() const noexcept { return shift_ms_.has_value(); }

  MediaTimestamps Convert(const ffmpeg::Packet& packet) const {
    Validate(packet);
    if (!shift_ms_) throw std::logic_error("媒体共同偏移尚未确定");
    return {Shift(Milliseconds(packet->pts, packet->time_base)),
            Shift(Milliseconds(packet->dts, packet->time_base))};
  }

 private:
  static std::int64_t Milliseconds(std::int64_t value, AVRational time_base) {
    const auto result = av_rescale_q(value, time_base, AVRational{1, 1000});
    if (result == std::numeric_limits<std::int64_t>::min() ||
        result == std::numeric_limits<std::int64_t>::max())
      throw std::overflow_error("媒体毫秒时间戳超出表示范围");
    return result;
  }
  std::uint64_t Shift(std::int64_t value) const {
    if (value < -*shift_ms_)
      throw std::invalid_argument("媒体时间戳早于共同输出起点");
    if (value > std::numeric_limits<std::int64_t>::max() - *shift_ms_)
      throw std::overflow_error("媒体共同偏移溢出");
    return static_cast<std::uint64_t>(value + *shift_ms_);
  }
  std::vector<ffmpeg::StreamInfo> streams_;
  std::optional<std::int64_t> shift_ms_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_REMUXER_MEDIA_TIMESTAMP_CONVERTER_H_
