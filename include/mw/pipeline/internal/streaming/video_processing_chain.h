#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_VIDEO_PROCESSING_CHAIN_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_VIDEO_PROCESSING_CHAIN_H_

#include <functional>
#include <memory>

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"

namespace mw::streamer::decoder {
class VideoDecoder;
}

namespace mw::streamer::processor {
class StreamingProcessorHandler;
}

namespace mw::streamer::pipeline::internal::streaming {

class VideoProcessingChain final {
 public:
  using OnFrame = std::function<void(const ffmpeg::Frame& frame)>;

  VideoProcessingChain(std::unique_ptr<decoder::VideoDecoder> decoder,
                       processor::StreamingProcessorHandler& processor,
                       OnFrame on_frame = {});
  ~VideoProcessingChain();

  VideoProcessingChain(const VideoProcessingChain&) = delete;
  VideoProcessingChain& operator=(const VideoProcessingChain&) = delete;

  void Input(const ffmpeg::Packet& packet);
  void Drain();
  void Flush();

 private:
  void ProcessFrame(const ffmpeg::Frame& frame);

  std::unique_ptr<decoder::VideoDecoder> decoder_;
  processor::StreamingProcessorHandler& processor_;
  OnFrame on_frame_;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_VIDEO_PROCESSING_CHAIN_H_
