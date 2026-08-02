#include "mw/pipeline/internal/streaming/video_processing_chain.h"

#include <utility>

#include "mw/decoder/video_decoder.h"
#include "mw/performance/internal/stage_recorder.h"
#include "mw/performance/internal/stopwatch.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline::internal::streaming {

VideoProcessingChain::VideoProcessingChain(
    std::unique_ptr<decoder::VideoDecoder> decoder,
    processor::StreamingProcessorHandler& processor,
    performance::internal::TrackRecorder& performance, OnFrame on_frame)
    : decoder_(std::move(decoder)),
      processor_(processor),
      performance_(performance),
      on_frame_(std::move(on_frame)) {
  decoder_->SetOnFrame(
      [this](const ffmpeg::Frame& frame) { ProcessFrame(frame); });
}

VideoProcessingChain::~VideoProcessingChain() = default;

void VideoProcessingChain::Input(const ffmpeg::Packet& packet) {
  const auto result = decoder_->Decode(packet);
  performance_.decode().Record(result.frames, result.service_time);
}

void VideoProcessingChain::Drain() {
  const auto result = decoder_->Drain();
  performance_.decode().Record(result.frames, result.service_time);
}

void VideoProcessingChain::Flush() { decoder_->Flush(); }

void VideoProcessingChain::ProcessFrame(const ffmpeg::Frame& frame) {
  performance::internal::Stopwatch stopwatch;
  auto output = stopwatch.Measure(
      [this, &frame]() { return processor_.ProcessVideo(frame); });
  performance_.process().Record(1, stopwatch.elapsed());
  if (on_frame_) {
    on_frame_(output);
  }
}

}  // namespace mw::streamer::pipeline::internal::streaming
