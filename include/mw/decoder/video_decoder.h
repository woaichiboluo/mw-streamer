#ifndef MW_STREAMER_INCLUDE_MW_DECODER_VIDEO_DECODER_H_
#define MW_STREAMER_INCLUDE_MW_DECODER_VIDEO_DECODER_H_

#include <functional>
#include <memory>

#include "mw/decoder/config.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::decoder {

class VideoDecoder final {
 public:
  using OnFrame = std::function<void(const ffmpeg::Frame& frame)>;

  explicit VideoDecoder(ffmpeg::StreamInfo stream_info,
                        VideoDecoderConfig config = {});
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Decoding and callbacks are synchronous on the calling thread. One packet
  // may produce zero or more frames. The frame is borrowed for OnFrame; copy or
  // call Ref to retain it.
  void SetOnFrame(OnFrame callback);
  void Decode(const ffmpeg::Packet& packet);

  // Drain emits all delayed frames and ends the current decoder timeline.
  // Decode cannot be called again until Flush starts a new timeline.
  void Drain();
  void Flush();

  const ffmpeg::StreamInfo& stream_info() const noexcept;
  const VideoDecoderConfig& config() const noexcept;

  // Returns the CUDA context owned by this decoder. Software decoders return
  // nullptr. The pointer is borrowed for the VideoDecoder lifetime.
  const ffmpeg::HardwareContext* hardware_context() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::decoder

#endif  // MW_STREAMER_INCLUDE_MW_DECODER_VIDEO_DECODER_H_
