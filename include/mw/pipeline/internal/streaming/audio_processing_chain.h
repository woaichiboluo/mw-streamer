#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_AUDIO_PROCESSING_CHAIN_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_AUDIO_PROCESSING_CHAIN_H_

#include <functional>
#include <memory>

#include "mw/decoder/config.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::decoder {
class AudioDecoder;
}

namespace mw::streamer::processor {
class StreamingProcessorHandler;
}

namespace mw::streamer::resampler {
class AudioResampler;
}

namespace mw::streamer::pipeline::internal::streaming {

class AudioProcessingChain final {
 public:
  using OnFrame = std::function<void(const ffmpeg::Frame& frame)>;

  AudioProcessingChain(ffmpeg::StreamInfo stream_info,
                       decoder::AudioDecoderConfig decoder_config,
                       processor::StreamingProcessorHandler& processor,
                       OnFrame on_frame = {});
  ~AudioProcessingChain();

  AudioProcessingChain(const AudioProcessingChain&) = delete;
  AudioProcessingChain& operator=(const AudioProcessingChain&) = delete;

  void Input(const ffmpeg::Packet& packet);
  void Drain();
  void Flush();

 private:
  void ProcessFrame(const ffmpeg::Frame& frame);

  std::unique_ptr<decoder::AudioDecoder> decoder_;
  std::unique_ptr<resampler::AudioResampler> resampler_;
  processor::StreamingProcessorHandler& processor_;
  OnFrame on_frame_;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_AUDIO_PROCESSING_CHAIN_H_
