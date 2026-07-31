#include "mw/pipeline/internal/streaming/audio_worker.h"

#include <exception>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
}

#include "mw/common/barrier.h"
#include "mw/common/thread.h"
#include "mw/encoder/audio_encoder.h"
#include "mw/pipeline/internal/streaming/output_worker.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline::internal::streaming {

AudioWorker::AudioWorker(
    ffmpeg::StreamInfo stream_info, decoder::AudioDecoderConfig decoder_config,
    encoder::AudioEncoderConfig encoder_config, std::size_t queue_capacity,
    processor::StreamingProcessorHandler& processor,
    common::Barrier& boundary_barrier, OutputWorker& output,
    std::function<void(const char* worker, const char* error)> on_failed)
    : queue_capacity_(queue_capacity),
      encoder_(std::make_unique<encoder::AudioEncoder>(
          std::move(encoder_config), stream_info.stream_index)),
      processing_chain_(
          std::move(stream_info), std::move(decoder_config), processor,
          [this](const ffmpeg::Frame& frame) { EncodeFrame(frame); }),
      processor_(processor),
      boundary_barrier_(boundary_barrier),
      output_(output),
      on_failed_(std::move(on_failed)) {
  encoder_->SetOnPacket(
      [this](const ffmpeg::Packet& packet) { output_.Write(packet); });
}

AudioWorker::~AudioWorker() { Stop(); }

void AudioWorker::Start() {
  if (thread_) {
    throw std::logic_error("AudioWorker只能启动一次");
  }
  thread_ = std::make_unique<common::Thread>("mw-audio", [this]() { Run(); });
}

bool AudioWorker::Input(const ffmpeg::Packet& packet) {
  return queue_.TryPush(WorkItem{WorkType::kPacket, packet.Ref(), false},
                        queue_capacity_);
}

void AudioWorker::Reset() {
  queue_.Clear();
  queue_.Push({WorkType::kTimelineReset, std::nullopt, false});
}

void AudioWorker::End(bool final_end) {
  queue_.Push({WorkType::kEnd, std::nullopt, final_end});
}

void AudioWorker::RequestStop() noexcept {
  queue_.Clear();
  queue_.Close();
}

void AudioWorker::Stop() noexcept {
  RequestStop();
  if (thread_ && !thread_->IsCurrent()) {
    thread_->Join();
  }
}

void AudioWorker::Run() noexcept {
  try {
    RunLoop();
  } catch (const std::exception& error) {
    on_failed_("audio", error.what());
  } catch (...) {
    on_failed_("audio", "未知异常");
  }
}

void AudioWorker::RunLoop() {
  while (auto work = queue_.WaitPop()) {
    switch (work->type) {
      case WorkType::kPacket:
        processing_chain_.Input(*work->packet);
        break;
      case WorkType::kTimelineReset:
        processing_chain_.Flush();
        if (!SynchronizeBoundary(kMwStreamerProcessorTimelineReset)) {
          return;
        }
        break;
      case WorkType::kEnd:
        processing_chain_.Drain();
        if (work->final_end) {
          if (!encoder_->is_open()) {
            throw std::runtime_error("音频流没有产生可编码帧");
          }
          if (!SynchronizeBoundary(kMwStreamerProcessorEndOfInput)) {
            return;
          }
          encoder_->Drain();
          output_.EndTrack(AVMEDIA_TYPE_AUDIO);
          return;
        }
        break;
    }
  }
}

bool AudioWorker::SynchronizeBoundary(
    MwStreamerProcessorBoundaryReason reason) {
  return boundary_barrier_.ArriveAndWait(
      [this, reason]() { processor_.NotifyBoundary(reason); });
}

void AudioWorker::EncodeFrame(const ffmpeg::Frame& frame) {
  if (!encoder_->is_open()) {
    encoder_->Open(frame);
    output_.RegisterOutputStream(AVMEDIA_TYPE_AUDIO, encoder_->stream_info());
  }
  encoder_->Encode(frame);
}

}  // namespace mw::streamer::pipeline::internal::streaming
