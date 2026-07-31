#include "mw/pipeline/internal/streaming/video_processing_chain.h"

#include <utility>

#include "mw/decoder/video_decoder.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline::internal::streaming {

VideoProcessingChain::VideoProcessingChain(
    std::unique_ptr<decoder::VideoDecoder> decoder,
    processor::StreamingProcessorHandler& processor, OnFrame on_frame)
    : decoder_(std::move(decoder)),
      processor_(processor),
      on_frame_(std::move(on_frame)) {
  decoder_->SetOnFrame(
      [this](const ffmpeg::Frame& frame) { ProcessFrame(frame); });
}

VideoProcessingChain::~VideoProcessingChain() = default;

void VideoProcessingChain::Input(const ffmpeg::Packet& packet) {
  decoder_->Decode(packet);
}

void VideoProcessingChain::Drain() { decoder_->Drain(); }

void VideoProcessingChain::Flush() { decoder_->Flush(); }

void VideoProcessingChain::ProcessFrame(const ffmpeg::Frame& frame) {
  auto output = processor_.ProcessVideo(frame);
  if (on_frame_) {
    on_frame_(output);
  }
}

}  // namespace mw::streamer::pipeline::internal::streaming
