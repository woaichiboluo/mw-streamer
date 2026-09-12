#ifndef MW_STREAMER_ENCODER_VIDEO_ENCODER_H_
#define MW_STREAMER_ENCODER_VIDEO_ENCODER_H_

#include <functional>
#include <memory>

#include "mw/streamer/encoder/config.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

enum class VideoEncodeMode {
  kAutomatic,
  kForceKeyFrame,
};

class VideoEncoder final {
 public:
  using OnPacket = std::function<void(const Packet& packet)>;

  explicit VideoEncoder(VideoEncoderConfig config, int stream_index = 0);
  ~VideoEncoder();

  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  // Open configures the encoder from the first Processor output frame. CUDA
  // frames reuse its hw_frames_ctx; host frames remain on the host path.
  void Open(const Frame& prototype);

  // Encoding and callbacks are synchronous on the calling thread. The packet
  // is borrowed for OnPacket; copy or call Ref to retain it.
  void SetOnPacket(OnPacket callback);
  // CUDA frame pools may change while the FFmpeg device context, underlying
  // pixel format, frame dimensions and time_base remain unchanged.
  void Encode(const Frame& frame,
              VideoEncodeMode mode = VideoEncodeMode::kAutomatic);

  // Drain emits all delayed packets and ends this encoder. Encode cannot be
  // called after Drain.
  void Drain();

  bool is_open() const noexcept;
  const StreamInfo& stream_info() const;
  const VideoEncoderConfig& config() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_VIDEO_ENCODER_H_
