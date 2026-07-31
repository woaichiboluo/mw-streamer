#include "mw/pipeline/internal/streaming/audio_processing_chain.h"

#include <utility>

#include "mw/decoder/audio_decoder.h"
#include "mw/processor/streaming_processor_handler.h"
#include "mw/resampler/audio_resampler.h"

namespace mw::streamer::pipeline::internal::streaming {

AudioProcessingChain::AudioProcessingChain(
    ffmpeg::StreamInfo stream_info, decoder::AudioDecoderConfig decoder_config,
    processor::StreamingProcessorHandler& processor, OnFrame on_frame)
    : decoder_(std::make_unique<decoder::AudioDecoder>(
          std::move(stream_info), std::move(decoder_config))),
      resampler_(
          std::make_unique<resampler::AudioResampler>(decoder_->stream_info())),
      processor_(processor),
      on_frame_(std::move(on_frame)) {
  decoder_->SetOnFrame(
      [this](const ffmpeg::Frame& frame) { resampler_->Resample(frame); });
  resampler_->SetOnFrame(
      [this](const ffmpeg::Frame& frame) { ProcessFrame(frame); });
}

AudioProcessingChain::~AudioProcessingChain() = default;

void AudioProcessingChain::Input(const ffmpeg::Packet& packet) {
  decoder_->Decode(packet);
}

void AudioProcessingChain::Drain() {
  decoder_->Drain();
  resampler_->Drain();
}

void AudioProcessingChain::Flush() {
  decoder_->Flush();
  resampler_->Flush();
}

void AudioProcessingChain::ProcessFrame(const ffmpeg::Frame& frame) {
  auto output = processor_.ProcessAudio(frame);
  if (on_frame_) {
    on_frame_(output);
  }
}

}  // namespace mw::streamer::pipeline::internal::streaming
