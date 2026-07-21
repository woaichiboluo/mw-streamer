#ifndef MW_STREAMER_INTERNAL_AUDIO_AUTO_DECODER_H_
#define MW_STREAMER_INTERNAL_AUDIO_AUTO_DECODER_H_

#include <optional>

extern "C" {
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"

namespace mw::streamer::internal {

struct StreamInfo;

class AudioAutoDecoder {
 public:
  void Open(const StreamInfo& stream);
  [[nodiscard]] bool Send(const ffmpeg::Packet& packet);
  [[nodiscard]] bool SendEndOfStream();
  [[nodiscard]] std::optional<ffmpeg::Frame> Receive();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] int stream_index() const noexcept;

 private:
  std::optional<ffmpeg::CodecContext> decoder_;
  AVRational packet_time_base_{0, 1};
  int stream_index_ = -1;
  bool drain_sent_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_AUDIO_AUTO_DECODER_H_
