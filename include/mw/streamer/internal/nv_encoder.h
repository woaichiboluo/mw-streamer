#ifndef MW_STREAMER_INTERNAL_NV_ENCODER_H_
#define MW_STREAMER_INTERNAL_NV_ENCODER_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"

namespace mw::streamer::internal {

enum class NvEncoderRateControl {
  kCbr,
  kVbr,
};

struct NvEncoderConfig {
  AVCodecID codec_id = AV_CODEC_ID_NONE;
  int width = 0;
  int height = 0;
  AVRational frame_rate{0, 1};
  int gop_size = 0;
  std::int64_t bit_rate = 0;
  std::int64_t max_bit_rate = 0;
  std::int64_t vbv_buffer_size = 0;
  NvEncoderRateControl rate_control = NvEncoderRateControl::kCbr;
  std::string preset = "p4";
  int max_b_frames = 0;
  AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
  AVColorSpace color_space = AVCOL_SPC_UNSPECIFIED;
  std::map<std::string, std::string> options;
};

class NvEncoder final {
 public:
  void Open(const NvEncoderConfig& config,
            const CudaDeviceContext& device_context,
            const GpuNv12FramePool& frame_pool);
  [[nodiscard]] bool Send(const ffmpeg::Frame& frame);
  [[nodiscard]] bool SendEndOfStream();
  [[nodiscard]] std::optional<ffmpeg::Packet> Receive();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] AVRational time_base() const noexcept;
  [[nodiscard]] const ffmpeg::CodecParameters& codec_parameters()
      const noexcept;

 private:
  std::optional<ffmpeg::CodecContext> encoder_;
  ffmpeg::CodecParameters codec_parameters_;
  const AVBufferRef* frame_context_ = nullptr;
  AVRational time_base_{0, 1};
  int width_ = 0;
  int height_ = 0;
  bool drain_sent_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_NV_ENCODER_H_
