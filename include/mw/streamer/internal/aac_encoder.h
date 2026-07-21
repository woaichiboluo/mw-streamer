#ifndef MW_STREAMER_INTERNAL_AAC_ENCODER_H_
#define MW_STREAMER_INTERNAL_AAC_ENCODER_H_

#include <cstdint>
#include <optional>

extern "C" {
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/audio_converter.h"

namespace mw::streamer::internal {

class AacEncoder final {
 public:
  void Open(std::int64_t bit_rate);
  [[nodiscard]] bool Send(const NormalizedAudioFrame& frame);
  [[nodiscard]] bool SendEndOfStream();
  [[nodiscard]] std::optional<ffmpeg::Packet> Receive();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] AVRational time_base() const noexcept;
  [[nodiscard]] const ffmpeg::CodecParameters& codec_parameters()
      const noexcept;

 private:
  std::optional<ffmpeg::CodecContext> encoder_;
  ffmpeg::CodecParameters codec_parameters_;
  std::int64_t next_pts_ = 0;
  bool drain_sent_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_AAC_ENCODER_H_
