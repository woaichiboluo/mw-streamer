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
  std::int64_t pts_ns;
  std::int64_t dts_ns;
};

// Media timestamp normalization for already aligned encoder tracks.
// Start with each track's first retained packet after startup selection. This
// class does not select packets or interleave them; Encoder's synchronized
// dts_ns remains a separate ordering clock, not the media DTS.
// Serialize calls. Share one converter per muxer session across all targets.
class MediaTimestampConverter final {
 public:
  explicit MediaTimestampConverter(
      const std::vector<ffmpeg::StreamInfo>& streams) {
    for (const auto& stream : streams) {
      stream.Validate();
      if (std::any_of(tracks_.begin(), tracks_.end(), [&](const auto& track) {
            return track.index == stream.stream_index;
          })) {
        throw std::invalid_argument("媒体时间转换的轨道索引重复");
      }
      tracks_.push_back({stream.stream_index,
                         stream.codec_parameters.get()->codec_type,
                         stream.time_base, std::nullopt});
    }
  }

  // Preserves negative codec delay, PTS reordering and the input packet.
  // Protocol-specific nonnegative shifting and millisecond conversion follow
  // this step, using a common shift for audio and video.
  MediaTimestamps Convert(const ffmpeg::Packet& packet) {
    const auto* input = packet.get();
    if (!input || input->pts == AV_NOPTS_VALUE ||
        input->dts == AV_NOPTS_VALUE) {
      throw std::invalid_argument("媒体时间转换需要有效PTS和DTS");
    }
    const auto track = std::find_if(
        tracks_.begin(), tracks_.end(),
        [&](const auto& value) { return value.index == input->stream_index; });
    if (track == tracks_.end() || input->time_base.num <= 0 ||
        input->time_base.den <= 0 ||
        av_cmp_q(input->time_base, track->time_base) != 0) {
      throw std::invalid_argument("媒体时间转换的轨道或时间基不匹配");
    }
    const auto offset =
        track->offset.value_or(track->type == AVMEDIA_TYPE_VIDEO
                                   ? input->pts
                                   : std::max<std::int64_t>(input->dts, 0));
    const MediaTimestamps result{av_rescale_q(Subtract(input->pts, offset),
                                              track->time_base, kNanoseconds),
                                 av_rescale_q(Subtract(input->dts, offset),
                                              track->time_base, kNanoseconds)};
    if (result.pts_ns == AV_NOPTS_VALUE || result.dts_ns == AV_NOPTS_VALUE) {
      throw std::overflow_error("媒体时间转换超出纳秒表示范围");
    }
    track->offset = offset;
    return result;
  }

  // Only a new encoder/muxer session resets the origin. Adding an output or
  // reconnecting an existing target does not reset it.
  void Reset() noexcept {
    for (auto& track : tracks_) track.offset.reset();
  }

 private:
  struct Track {
    int index;
    AVMediaType type;
    AVRational time_base;
    std::optional<std::int64_t> offset;
  };

  static std::int64_t Subtract(std::int64_t timestamp, std::int64_t offset) {
    if ((offset > 0 &&
         timestamp < std::numeric_limits<std::int64_t>::min() + offset) ||
        (offset < 0 &&
         timestamp > std::numeric_limits<std::int64_t>::max() + offset)) {
      throw std::overflow_error("媒体时间转换的起点平移溢出");
    }
    return timestamp - offset;
  }

  static constexpr AVRational kNanoseconds{1, 1000000000};
  std::vector<Track> tracks_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_REMUXER_MEDIA_TIMESTAMP_CONVERTER_H_
