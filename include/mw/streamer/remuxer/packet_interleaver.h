#ifndef MW_STREAMER_REMUXER_PACKET_INTERLEAVER_H_
#define MW_STREAMER_REMUXER_PACKET_INTERLEAVER_H_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mw/streamer/remuxer/media_timestamp_converter.h"

namespace mw::streamer::internal {

// One already aligned encoder session, with at most one track of each type.
// Serialize all calls. Startup selection uses Encoder's ordering clock, then
// media timestamps are normalized once and interleaved by their signed DTS.
// Per-track DTS must be nondecreasing. The queue retains packets without a
// quota or live-network dropping policy.
class PacketInterleaver final {
 public:
  struct Item {
    ffmpeg::Packet packet;
    std::int64_t pts_ns;
    std::int64_t dts_ns;
    std::chrono::steady_clock::time_point queued_at{};
  };

  explicit PacketInterleaver(const std::vector<ffmpeg::StreamInfo>& streams)
      : converter_(streams) {
    if (streams.empty()) throw std::invalid_argument("交织轨道不能为空");
    for (const auto& stream : streams) {
      const auto type = stream.codec_parameters.get()->codec_type;
      if (std::any_of(tracks_.begin(), tracks_.end(),
                      [&](const auto& track) { return track.type == type; })) {
        throw std::invalid_argument("交织器只支持一条视频和一条音频轨道");
      }
      tracks_.push_back({stream.stream_index, type, stream.time_base,
                         stream.codec_parameters.get()->framerate,
                         std::nullopt});
    }
  }

  void Push(const ffmpeg::Packet& packet, std::int64_t ordering_dts_ns,
            std::chrono::steady_clock::time_point queued_at = {}) {
    if (flushed_) throw std::logic_error("交织器已经排空");
    const auto* input = packet.get();
    if (!input || input->pts == AV_NOPTS_VALUE ||
        input->dts == AV_NOPTS_VALUE || ordering_dts_ns == AV_NOPTS_VALUE) {
      throw std::invalid_argument("交织器需要有效包和时间戳");
    }
    const auto track = std::find_if(
        tracks_.begin(), tracks_.end(),
        [&](const auto& value) { return value.index == input->stream_index; });
    if (track == tracks_.end() || input->time_base.num <= 0 ||
        input->time_base.den <= 0 ||
        av_cmp_q(input->time_base, track->time_base) != 0) {
      throw std::invalid_argument("交织包轨道或时间基不匹配");
    }
    const auto track_index = static_cast<std::size_t>(track - tracks_.begin());
    if (!initialized_) {
      // Wait for the first video keyframe and discard earlier startup audio.
      if (track->type == AVMEDIA_TYPE_VIDEO && !First(track_index) &&
          !(input->flags & AV_PKT_FLAG_KEY)) {
        const auto first =
            std::find_if(queue_.begin(), queue_.end(), [&](const auto& value) {
              return value.ordering_dts_ns >= ordering_dts_ns;
            });
        ErasePrefix(static_cast<std::size_t>(first - queue_.begin()));
        ++discarded_packets_;
        discarded_bytes_ += PayloadBytes(packet);
        return;
      }
      Insert({{packet.Ref(), 0, 0, queued_at}, track_index, ordering_dts_ns},
             false);
      Initialize(false);
    } else {
      const auto timestamp = converter_.Convert(packet);
      Insert({{packet.Ref(), timestamp.pts_ns, timestamp.dts_ns, queued_at},
              track_index,
              ordering_dts_ns},
             true);
      Higher(track_index, timestamp.dts_ns);
    }
  }

  // Normal delivery waits for strictly later DTS on every other track. EOF
  // emits all retained data, including a partial set of tracks, and closes
  // admission. A video track with no keyframe contributes no packets.
  std::vector<Item> PopReady(bool flush = false) {
    if (flush) {
      if (!initialized_) Initialize(true);
      flushed_ = true;
    }
    std::vector<Item> result;
    if (!initialized_) return result;
    std::size_t count = 0;
    while (count < queue_.size() && (flush || Ready(queue_[count]))) ++count;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      queued_bytes_ -= PayloadBytes(queue_[index].item.packet);
      result.push_back(std::move(queue_[index].item));
    }
    queue_.erase(queue_.begin(),
                 queue_.begin() + static_cast<std::ptrdiff_t>(count));
    return result;
  }

  bool initialized() const noexcept { return initialized_; }
  std::size_t queued_packets() const noexcept { return queue_.size(); }
  std::size_t queued_bytes() const noexcept { return queued_bytes_; }
  std::uint64_t discarded_packets() const noexcept {
    return discarded_packets_;
  }
  std::uint64_t discarded_bytes() const noexcept { return discarded_bytes_; }

 private:
  struct Track {
    int index;
    AVMediaType type;
    AVRational time_base;
    AVRational frame_rate;
    std::optional<std::int64_t> highest_dts_ns;
  };
  struct Queued {
    Item item;
    std::size_t track;
    std::int64_t ordering_dts_ns;
  };

  static std::size_t PayloadBytes(const ffmpeg::Packet& packet) noexcept {
    return packet->size > 0 ? static_cast<std::size_t>(packet->size) : 0;
  }

  void Discard(const Queued& value) noexcept {
    const auto bytes = PayloadBytes(value.item.packet);
    queued_bytes_ -= bytes;
    ++discarded_packets_;
    discarded_bytes_ += bytes;
  }

  std::optional<std::size_t> First(std::size_t track) const {
    for (std::size_t index = 0; index < queue_.size(); ++index) {
      if (queue_[index].track == track) return index;
    }
    return std::nullopt;
  }

  static std::uint64_t Distance(std::int64_t a, std::int64_t b) noexcept {
    return a >= b
               ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
               : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
  }

  bool Before(const Queued& a, const Queued& b, bool media) const {
    const auto left = media ? a.item.dts_ns : a.ordering_dts_ns;
    const auto right = media ? b.item.dts_ns : b.ordering_dts_ns;
    return left != right ? left < right
                         : tracks_[a.track].type == AVMEDIA_TYPE_VIDEO &&
                               tracks_[b.track].type != AVMEDIA_TYPE_VIDEO;
  }

  void Insert(Queued value, bool media) {
    const auto bytes = PayloadBytes(value.item.packet);
    const auto position = std::upper_bound(
        queue_.begin(), queue_.end(), value,
        [&](const auto& a, const auto& b) { return Before(a, b, media); });
    queue_.insert(position, std::move(value));
    queued_bytes_ += bytes;
  }

  std::int64_t VideoDuration(const Queued& video) const {
    const auto& track = tracks_[video.track];
    const auto duration = video.item.packet->duration;
    const auto value =
        duration > 0
            ? av_rescale_q(duration, track.time_base, kNanoseconds)
            : av_rescale_q(1,
                           track.frame_rate.num > 0 && track.frame_rate.den > 0
                               ? av_inv_q(track.frame_rate)
                               : track.time_base,
                           kNanoseconds);
    if (value <= 0) throw std::invalid_argument("视频时长无法换算");
    return value;
  }

  // Select the closest audio packet in the ordering clock, capped
  // at the first video. Keep the audio prefix if the selected audio PTS <= 0.
  std::size_t StartIndex(std::size_t video, std::size_t audio_track) const {
    auto closest = std::numeric_limits<std::uint64_t>::max();
    std::size_t start = video;
    for (std::size_t index = 0; index < queue_.size(); ++index) {
      if (queue_[index].track != audio_track) continue;
      const auto distance = Distance(queue_[index].ordering_dts_ns,
                                     queue_[video].ordering_dts_ns);
      if (distance < closest) {
        closest = distance;
        start = index;
      }
    }
    start = std::min(start, video);
    for (std::size_t index = start; index < queue_.size(); ++index) {
      if (queue_[index].track != audio_track) continue;
      if (queue_[index].item.packet->pts <= 0) {
        start = std::min(start, *First(audio_track));
      }
      break;
    }
    return start;
  }

  void ErasePrefix(std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) Discard(queue_[index]);
    queue_.erase(queue_.begin(),
                 queue_.begin() + static_cast<std::ptrdiff_t>(count));
  }

  void Initialize(bool flush) {
    if (queue_.empty() || initialized_) return;
    std::optional<std::size_t> video_track;
    std::optional<std::size_t> audio_track;
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
      if (tracks_[index].type == AVMEDIA_TYPE_VIDEO) video_track = index;
      if (tracks_[index].type == AVMEDIA_TYPE_AUDIO) audio_track = index;
    }
    // Pruning may have removed a previously accepted first keyframe.
    if (video_track) {
      const auto key =
          std::find_if(queue_.begin(), queue_.end(), [&](const auto& value) {
            return value.track == *video_track &&
                   (value.item.packet->flags & AV_PKT_FLAG_KEY);
          });
      const auto key_index = static_cast<std::size_t>(key - queue_.begin());
      std::size_t index = 0;
      queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                  [&](const auto& value) {
                                    const bool discard =
                                        index++ < key_index &&
                                        value.track == *video_track;
                                    if (discard) Discard(value);
                                    return discard;
                                  }),
                   queue_.end());
    }
    if (queue_.empty()) return;
    const auto video = video_track ? First(*video_track) : std::nullopt;
    const auto audio = audio_track ? First(*audio_track) : std::nullopt;
    if (!flush && ((video_track && !video) || (audio_track && !audio))) return;
    if (video && audio) {
      const auto video_clock = queue_[*video].ordering_dts_ns;
      const auto audio_clock = queue_[*audio].ordering_dts_ns;
      if (!flush && audio_clock > video_clock &&
          Distance(audio_clock, video_clock) >
              static_cast<std::uint64_t>(VideoDuration(queue_[*video]))) {
        ErasePrefix(std::max(*video, *audio) + 1);
        return;
      }
      if (!flush &&
          std::none_of(queue_.begin(), queue_.end(), [&](const auto& value) {
            return value.track == *audio_track &&
                   value.ordering_dts_ns >= video_clock;
          })) {
        ErasePrefix(StartIndex(*video, *audio_track));
        return;
      }
      ErasePrefix(StartIndex(*video, *audio_track));
    }
    for (auto& value : queue_) {
      const auto timestamp = converter_.Convert(value.item.packet);
      value.item.pts_ns = timestamp.pts_ns;
      value.item.dts_ns = timestamp.dts_ns;
      Higher(value.track, timestamp.dts_ns);
    }
    initialized_ = true;
    std::stable_sort(
        queue_.begin(), queue_.end(),
        [&](const auto& a, const auto& b) { return Before(a, b, true); });
  }

  void Higher(std::size_t track, std::int64_t timestamp) {
    auto& highest = tracks_[track].highest_dts_ns;
    if (!highest || *highest < timestamp) highest = timestamp;
  }

  bool Ready(const Queued& packet) const {
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
      if (index == packet.track) continue;
      const auto highest = tracks_[index].highest_dts_ns;
      if (!highest || *highest <= packet.item.dts_ns) return false;
    }
    return true;
  }

  static constexpr AVRational kNanoseconds{1, 1000000000};
  MediaTimestampConverter converter_;
  std::vector<Track> tracks_;
  std::vector<Queued> queue_;
  std::size_t queued_bytes_ = 0;
  std::uint64_t discarded_packets_ = 0;
  std::uint64_t discarded_bytes_ = 0;
  bool initialized_ = false;
  bool flushed_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_REMUXER_PACKET_INTERLEAVER_H_
