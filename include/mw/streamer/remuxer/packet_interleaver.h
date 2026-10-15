#ifndef MW_STREAMER_REMUXER_PACKET_INTERLEAVER_H_
#define MW_STREAMER_REMUXER_PACKET_INTERLEAVER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mw/streamer/remuxer/media_timestamp_converter.h"

namespace mw::streamer::internal {

// Native DTS ordering for one encoder session. No keyframe selection, per-track
// origin normalization or packet dropping. Calls are externally serialized.
class PacketInterleaver final {
 public:
  struct Item {
    ffmpeg::Packet packet;
  };
  explicit PacketInterleaver(const std::vector<ffmpeg::StreamInfo>& streams)
      : validation_(streams) {
    for (const auto& stream : streams) {
      const auto type = stream.codec_parameters.get()->codec_type;
      if (std::any_of(tracks_.begin(), tracks_.end(),
                      [&](const auto& track) { return track.type == type; }))
        throw std::invalid_argument("交织器只支持一条视频和一条音频轨道");
      tracks_.push_back(
          {stream.stream_index, type, stream.time_base, std::nullopt});
    }
  }
  void Validate(const ffmpeg::Packet& packet) const {
    validation_.Validate(packet);
  }
  void Push(const ffmpeg::Packet& packet) {
    if (flushed_) throw std::logic_error("交织器已经排空");
    Validate(packet);
    auto track = std::find_if(
        tracks_.begin(), tracks_.end(),
        [&](const auto& value) { return value.index == packet->stream_index; });
    if (track->highest && packet->dts < *track->highest)
      throw std::invalid_argument("编码包 DTS 回退");
    Item item{packet.Ref()};
    const auto position = std::upper_bound(
        queue_.begin(), queue_.end(), item,
        [&](const auto& left, const auto& right) {
          const auto comparison =
              av_compare_ts(left.packet->dts, left.packet->time_base,
                            right.packet->dts, right.packet->time_base);
          return comparison != 0 ? comparison < 0
                                 : TrackOrder(left.packet->stream_index) <
                                       TrackOrder(right.packet->stream_index);
        });
    queue_.insert(position, std::move(item));
    track->highest = packet->dts;
  }
  // After startup, FFmpeg's ten-second max_interleave_delta permits progress
  // when a track stalls. ZLM cannot retain an indefinitely absent startup
  // track; before every first packet arrives, exceeding this window fails.
  std::vector<Item> PopReady(bool flush = false) {
    if (flush) flushed_ = true;
    const bool all_tracks_seen = std::all_of(
        tracks_.begin(), tracks_.end(),
        [](const auto& track) { return track.highest.has_value(); });
    if (!flush && !initialized_ && !all_tracks_seen && ExceedsDelta())
      throw std::runtime_error("ZLM 启动超过 10 秒仍缺少轨道首包");
    initialized_ = initialized_ || flush || all_tracks_seen;
    std::vector<Item> result;
    if (!initialized_) return result;
    while (!queue_.empty() &&
           (flush || AllTracksBuffered() || ExceedsDelta())) {
      result.push_back(std::move(queue_.front()));
      queue_.erase(queue_.begin());
    }
    return result;
  }
  // Called before the first PopReady: capture each available track's first
  // encoded packet to determine the one protocol shift, including its PTS.
  std::vector<ffmpeg::Packet> FirstPackets() const {
    std::vector<ffmpeg::Packet> result;
    for (const auto& track : tracks_) {
      const auto first =
          std::find_if(queue_.begin(), queue_.end(), [&](const auto& item) {
            return item.packet->stream_index == track.index;
          });
      if (first != queue_.end()) result.push_back(first->packet.Ref());
    }
    return result;
  }
  std::size_t queued_packets() const noexcept { return queue_.size(); }

 private:
  struct Track {
    int index;
    AVMediaType type;
    AVRational time_base;
    std::optional<std::int64_t> highest;
  };
  bool ExceedsDelta() const {
    if (queue_.size() < 2) return false;
    const auto& first = queue_.front().packet;
    const auto& last = queue_.back().packet;
    // Compare a rational difference, without converting media to nanoseconds.
    const auto first_us =
        av_rescale_q(first->dts, first->time_base, AVRational{1, 1000000});
    const auto last_us =
        av_rescale_q(last->dts, last->time_base, AVRational{1, 1000000});
    return last_us > first_us && static_cast<std::uint64_t>(last_us) -
                                         static_cast<std::uint64_t>(first_us) >
                                     10000000;
  }
  std::size_t TrackOrder(int index) const {
    const auto track =
        std::find_if(tracks_.begin(), tracks_.end(),
                     [&](const auto& value) { return value.index == index; });
    return static_cast<std::size_t>(track - tracks_.begin());
  }
  // FFmpeg ff_interleave_packet_per_dts checks current buffered tracks,
  // including equal DTS. A previous track watermark is not sufficient.
  bool AllTracksBuffered() const {
    return std::all_of(tracks_.begin(), tracks_.end(), [&](const auto& track) {
      return std::any_of(queue_.begin(), queue_.end(), [&](const auto& item) {
        return item.packet->stream_index == track.index;
      });
    });
  }
  MediaTimestampConverter validation_;
  std::vector<Track> tracks_;
  std::vector<Item> queue_;
  bool initialized_ = false;
  bool flushed_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_REMUXER_PACKET_INTERLEAVER_H_
