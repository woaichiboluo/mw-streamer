#ifndef MW_STREAMER_SRC_STREAMER_INPUT_TIMESTAMP_DISCONTINUITY_H_
#define MW_STREAMER_SRC_STREAMER_INPUT_TIMESTAMP_DISCONTINUITY_H_

#include <cstdint>
#include <vector>

#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/input/av_input.h"

namespace mw::streamer::internal {

// Live input's counterpart of FFmpeg CLI ts_discontinuity_process/detect and
// ist_dts_update (fftools/ffmpeg_demux.c, n8.0). libavformat does not perform
// this CLI correction. Reconstruct for each connection/loop generation.
// Uses FFmpeg's default 10 s jump / 100 ms backward tolerance; formats without
// AVFMT_TS_DISCONT only discard timestamps beyond its 30 h error threshold.
// This has no copyts/start-time/loop-offset policy and never synthesizes packet
// timestamps. Packets are private headers; payload and PTS-DTS spacing survive
// correction. Arithmetic that cannot be represented throws overflow_error.
class TimestampDiscontinuity final {
 public:
  explicit TimestampDiscontinuity(const AvInputInfo& info);
  void Process(ffmpeg::Packet& packet);

 private:
  struct Track {
    int index;
    AVMediaType type;
    AVRational time_base;
    AVRational frame_rate;
    int sample_rate;
    int frame_size;
    int video_delay;
    bool saw_first_timestamp = false;
    std::int64_t dts_us = AV_NOPTS_VALUE;
    std::int64_t next_dts_us = AV_NOPTS_VALUE;
  };

  void Correct(ffmpeg::Packet& packet, const Track& track,
               std::int64_t delta_us);
  void Detect(ffmpeg::Packet& packet, const Track& track);
  void Predict(const ffmpeg::Packet& packet, Track& track);

  bool discontinuous_;
  std::int64_t offset_us_ = 0;
  std::int64_t last_dts_us_ = AV_NOPTS_VALUE;
  std::vector<Track> tracks_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_SRC_STREAMER_INPUT_TIMESTAMP_DISCONTINUITY_H_
