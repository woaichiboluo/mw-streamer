#ifndef MW_STREAMER_INCLUDE_MW_ENCODER_VIDEO_ENCODER_H_
#define MW_STREAMER_INCLUDE_MW_ENCODER_VIDEO_ENCODER_H_

#include <functional>
#include <memory>

#include "mw/encoder/config.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::encoder {

enum class VideoEncodeMode {
  kAutomatic,
  kForceKeyFrame,
};

class VideoEncoder final {
 public:
  using OnPacket = std::function<void(const ffmpeg::Packet& packet)>;

  explicit VideoEncoder(VideoEncoderConfig config, int stream_index = 0);
  ~VideoEncoder();

  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  // Open configures the encoder from the first Processor output frame. CUDA
  // frames reuse its hw_frames_ctx; host frames remain on the host path.
  void Open(const ffmpeg::Frame& prototype);

  // Encoding and callbacks are synchronous on the calling thread. The packet
  // is borrowed for OnPacket; copy or call Ref to retain it.
  void SetOnPacket(OnPacket callback);
  void Encode(const ffmpeg::Frame& frame,
              VideoEncodeMode mode = VideoEncodeMode::kAutomatic);

  // Drain emits all delayed packets and ends this encoder. Encode cannot be
  // called after Drain.
  void Drain();

  bool is_open() const noexcept;
  const ffmpeg::StreamInfo& stream_info() const;
  const VideoEncoderConfig& config() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::encoder

#endif  // MW_STREAMER_INCLUDE_MW_ENCODER_VIDEO_ENCODER_H_
