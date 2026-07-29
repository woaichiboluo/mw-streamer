#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_AUDIO_DECODER_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_AUDIO_DECODER_H_

#include <functional>
#include <memory>

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::ffmpeg {

class AudioDecoder final {
 public:
  using OnFrame = std::function<void(const Frame& frame)>;

  explicit AudioDecoder(StreamInfo stream_info);
  ~AudioDecoder();

  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  // Decoding and callbacks are synchronous on the calling thread. One packet
  // may produce zero or more frames. The frame is borrowed for OnFrame; copy or
  // call Ref to retain it.
  void SetOnFrame(OnFrame callback);
  void Decode(const Packet& packet);

  // Drain emits all delayed frames and ends the current decoder timeline.
  // Decode cannot be called again until Reset starts a new timeline.
  void Drain();
  void Reset();

  const StreamInfo& stream_info() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_AUDIO_DECODER_H_
