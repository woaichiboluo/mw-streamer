#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_FRAME_SYNCHRONIZER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_FRAME_SYNCHRONIZER_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include "mw/ffmpeg/frame.h"
#include "mw/pipeline/internal/streaming/standby_video_frame.h"

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::pipeline::internal::streaming {

// Synchronizes processed audio and video frames on one output thread. Source
// PTS determines media alignment; arrival time is used only to bound how long
// one track may wait for the other.
class FrameSynchronizer final {
 public:
  using Clock = std::chrono::steady_clock;

  struct OutputFrame {
    AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
    ffmpeg::Frame frame;
    bool force_key_frame = false;
  };

  FrameSynchronizer(bool has_audio, bool has_video, AVRational video_frame_rate,
                    std::chrono::milliseconds max_track_wait,
                    bool standby_enabled, std::string standby_image_path,
                    const ffmpeg::HardwareContext* hardware_context);

  FrameSynchronizer(const FrameSynchronizer&) = delete;
  FrameSynchronizer& operator=(const FrameSynchronizer&) = delete;

  void PushAudio(ffmpeg::Frame frame);
  void PushVideo(ffmpeg::Frame frame);

  // Interrupt marks one track at a discarded or completed input generation.
  // Once every configured track reaches the boundary, real-frame scheduling
  // pauses and standby output starts when enabled.
  void Interrupt(AVMediaType media_type);

  // Finish marks the final end of a track. Remaining synchronized frames are
  // emitted before finished() becomes true.
  void Finish(AVMediaType media_type);

  std::optional<OutputFrame> TakeReady(Clock::time_point now);
  std::optional<Clock::time_point> deadline() const noexcept;
  bool finished() const noexcept;

 private:
  struct TimedFrame {
    ffmpeg::Frame frame;
    std::int64_t source_pts_us = AV_NOPTS_VALUE;
    Clock::time_point received_at;
  };

  struct TrackState {
    AVRational time_base{0, 1};
    std::int64_t next_pts = 0;
    std::int64_t last_source_pts_us = AV_NOPTS_VALUE;
    bool initialized = false;
    bool interrupted = false;
    bool finished = false;
  };

  static void ValidateFrame(const ffmpeg::Frame& frame, AVMediaType media_type);
  static std::int64_t SourcePtsUs(const ffmpeg::Frame& frame);
  static std::int64_t TimeUs(const TrackState& track);

  void Push(std::deque<TimedFrame>& queue, TrackState& track,
            ffmpeg::Frame frame, AVMediaType media_type);
  void MarkTrack(AVMediaType media_type, bool final_end);
  bool AllTracksInterrupted() const noexcept;
  bool AllTracksFinished() const noexcept;
  bool ReadyToStart();
  bool ReadyToResume();
  bool AlignFirstFrames();
  void StartRealOutput(bool resumed, Clock::time_point now);
  void StartStandby(Clock::time_point now);
  void PrepareSilenceFrame(const ffmpeg::Frame& prototype);

  std::optional<OutputFrame> TakeStartingFrame();
  std::optional<OutputFrame> TakeRealFrame(Clock::time_point now);
  std::optional<OutputFrame> TakeStandbyFrame(Clock::time_point now);
  OutputFrame TakeAudio(bool silence);
  OutputFrame TakeRealVideo(bool force_key_frame);
  OutputFrame TakeRepeatedVideo(bool force_key_frame);
  OutputFrame TakeStandbyVideo(bool force_key_frame);
  bool DropLateFrames(std::deque<TimedFrame>& queue, TrackState& track,
                      std::int64_t duration_us);
  std::int64_t DesiredTimeUs(const TimedFrame& frame) const;
  bool AudioDueBeforeVideo() const noexcept;
  std::optional<Clock::time_point> MissingTrackDeadline() const noexcept;

  const bool has_audio_;
  const bool has_video_;
  const AVRational video_frame_rate_;
  const std::chrono::milliseconds max_track_wait_;
  const bool standby_enabled_;
  const ffmpeg::HardwareContext* hardware_context_;

  std::deque<TimedFrame> audio_frames_;
  std::deque<TimedFrame> video_frames_;
  TrackState audio_;
  TrackState video_;
  std::optional<ffmpeg::Frame> silence_frame_;
  std::optional<ffmpeg::Frame> last_video_frame_;
  std::optional<StandbyVideoFrame> standby_video_frame_;

  std::int64_t source_origin_us_ = 0;
  std::int64_t output_anchor_us_ = 0;
  std::int64_t video_duration_ = 0;
  std::int64_t audio_duration_ = 0;
  Clock::time_point standby_started_at_;
  Clock::time_point real_started_at_;
  std::int64_t standby_anchor_us_ = 0;
  std::int64_t real_anchor_us_ = 0;
  std::optional<Clock::time_point> deadline_;
  bool started_ = false;
  bool standby_ = false;
  bool transition_ = false;
  bool force_next_real_video_ = false;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_FRAME_SYNCHRONIZER_H_
