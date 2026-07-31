#ifndef MW_STREAMER_INCLUDE_MW_ENCODER_AUDIO_ENCODER_H_
#define MW_STREAMER_INCLUDE_MW_ENCODER_AUDIO_ENCODER_H_

#include <functional>
#include <memory>

#include "mw/encoder/config.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::encoder {

class AudioEncoder final {
 public:
  using OnPacket = std::function<void(const ffmpeg::Packet& packet)>;

  explicit AudioEncoder(AudioEncoderConfig config = {}, int stream_index = 0);
  ~AudioEncoder();

  AudioEncoder(const AudioEncoder&) = delete;
  AudioEncoder& operator=(const AudioEncoder&) = delete;

  // Open configures the encoder from a Processor audio frame. The prototype is
  // borrowed and must be 48 kHz interleaved float32.
  void Open(const ffmpeg::Frame& prototype);

  // Encoding and callbacks are synchronous on the calling thread. The packet
  // is borrowed for OnPacket; copy or call Ref to retain it.
  void SetOnPacket(OnPacket callback);
  void Encode(const ffmpeg::Frame& frame);

  // Drain encodes remaining FIFO samples, emits all delayed packets, and ends
  // this encoder. Encode cannot be called after Drain.
  void Drain();

  bool is_open() const noexcept;
  const ffmpeg::StreamInfo& stream_info() const;
  const AudioEncoderConfig& config() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::encoder

#endif  // MW_STREAMER_INCLUDE_MW_ENCODER_AUDIO_ENCODER_H_
