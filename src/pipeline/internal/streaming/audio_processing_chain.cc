#include "mw/pipeline/internal/streaming/audio_processing_chain.h"

#include <utility>

#include "mw/decoder/audio_decoder.h"
#include "mw/performance/internal/stage_recorder.h"
#include "mw/performance/internal/stopwatch.h"
#include "mw/processor/streaming_processor_handler.h"
#include "mw/resampler/audio_resampler.h"

namespace mw::streamer::pipeline::internal::streaming {

AudioProcessingChain::AudioProcessingChain(
    ffmpeg::StreamInfo stream_info, decoder::AudioDecoderConfig decoder_config,
    processor::StreamingProcessorHandler& processor,
    performance::internal::TrackRecorder& performance, OnFrame on_frame)
    : decoder_(std::make_unique<decoder::AudioDecoder>(
          std::move(stream_info), std::move(decoder_config))),
      resampler_(
          std::make_unique<resampler::AudioResampler>(decoder_->stream_info())),
      processor_(processor),
      performance_(performance),
      on_frame_(std::move(on_frame)) {
  decoder_->SetOnFrame(
      [this](const ffmpeg::Frame& frame) { resampler_->Resample(frame); });
  resampler_->SetOnFrame(
      [this](const ffmpeg::Frame& frame) { ProcessFrame(frame); });
}

AudioProcessingChain::~AudioProcessingChain() = default;

void AudioProcessingChain::Input(const ffmpeg::Packet& packet) {
  const auto result = decoder_->Decode(packet);
  performance_.decode().Record(result.samples, result.service_time);
}

void AudioProcessingChain::Drain() {
  const auto result = decoder_->Drain();
  performance_.decode().Record(result.samples, result.service_time);
  resampler_->Drain();
}

void AudioProcessingChain::Flush() {
  decoder_->Flush();
  resampler_->Flush();
}

void AudioProcessingChain::ProcessFrame(const ffmpeg::Frame& frame) {
  performance::internal::Stopwatch stopwatch;
  auto output = stopwatch.Measure(
      [this, &frame]() { return processor_.ProcessAudio(frame); });
  performance_.process().Record(static_cast<std::uint64_t>(output->nb_samples),
                                stopwatch.elapsed());
  if (on_frame_) {
    on_frame_(output);
  }
}

}  // namespace mw::streamer::pipeline::internal::streaming
