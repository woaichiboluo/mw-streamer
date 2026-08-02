#include "mw/pipeline/internal/streaming/audio_worker.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "mw/common/barrier.h"
#include "mw/common/thread.h"
#include "mw/performance/internal/stage_recorder.h"
#include "mw/pipeline/internal/streaming/output_worker.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline::internal::streaming {

AudioWorker::AudioWorker(
    ffmpeg::StreamInfo stream_info, decoder::AudioDecoderConfig decoder_config,
    std::size_t queue_capacity, processor::StreamingProcessorHandler& processor,
    common::Barrier& boundary_barrier, OutputWorker& output,
    performance::internal::TrackRecorder& performance,
    std::function<void(const char* worker, const char* error)> on_failed)
    : queue_capacity_(queue_capacity),
      processing_chain_(
          std::move(stream_info), std::move(decoder_config), processor,
          performance,
          [this](const ffmpeg::Frame& frame) { output_.WriteAudio(frame); }),
      processor_(processor),
      boundary_barrier_(boundary_barrier),
      output_(output),
      performance_(performance),
      on_failed_(std::move(on_failed)) {}

AudioWorker::~AudioWorker() { Stop(); }

void AudioWorker::Start() {
  if (thread_) {
    throw std::logic_error("AudioWorker只能启动一次");
  }
  thread_ = std::make_unique<common::Thread>("mw-audio", [this]() { Run(); });
}

bool AudioWorker::Input(const ffmpeg::Packet& packet) {
  const bool accepted = queue_.TryPush(
      WorkItem{WorkType::kPacket, packet.Ref(), false}, queue_capacity_);
  if (!accepted) {
    performance_.RecordDroppedPackets(1);
  }
  return accepted;
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

std::size_t AudioWorker::queue_depth() const { return queue_.size(); }

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
        output_.InterruptTrack(AVMEDIA_TYPE_AUDIO);
        break;
      case WorkType::kEnd:
        processing_chain_.Drain();
        if (work->final_end) {
          if (!SynchronizeBoundary(kMwStreamerProcessorEndOfInput)) {
            return;
          }
          output_.FinishTrack(AVMEDIA_TYPE_AUDIO);
          return;
        }
        output_.InterruptTrack(AVMEDIA_TYPE_AUDIO);
        break;
    }
  }
}

bool AudioWorker::SynchronizeBoundary(
    MwStreamerProcessorBoundaryReason reason) {
  return boundary_barrier_.ArriveAndWait(
      [this, reason]() { processor_.NotifyBoundary(reason); });
}

}  // namespace mw::streamer::pipeline::internal::streaming
