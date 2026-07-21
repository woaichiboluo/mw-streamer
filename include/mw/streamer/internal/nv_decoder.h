#ifndef MW_STREAMER_INTERNAL_NV_DECODER_H_
#define MW_STREAMER_INTERNAL_NV_DECODER_H_

#include <optional>

extern "C" {
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/stream_info.h"
#include "mw/streamer/processor.h"

namespace mw::streamer::internal {

class NvDecoder {
 public:
  void Open(const StreamInfo& stream, const CudaDeviceContext& device_context);
  [[nodiscard]] bool Send(const ffmpeg::Packet& packet);
  [[nodiscard]] bool SendEndOfStream();
  [[nodiscard]] std::optional<ffmpeg::Frame> Receive();

  [[nodiscard]] MwGpuNv12FrameView View(
      const ffmpeg::Frame& frame) const noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] int stream_index() const noexcept;

 private:
  std::optional<ffmpeg::CodecContext> decoder_;
  AVRational packet_time_base_{0, 1};
  int stream_index_ = -1;
  int device_id_ = -1;
  int input_width_ = 0;
  int input_height_ = 0;
  bool drain_sent_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_NV_DECODER_H_
