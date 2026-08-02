#include "mw/pipeline/internal/streaming/video_worker.h"

#include <exception>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/packet.h>
}

#include "mw/common/barrier.h"
#include "mw/common/thread.h"
#include "mw/decoder/video_decoder.h"
#include "mw/performance/internal/stage_recorder.h"
#include "mw/pipeline/internal/streaming/output_worker.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline::internal::streaming {

VideoWorker::VideoWorker(
    std::unique_ptr<decoder::VideoDecoder> decoder, std::size_t queue_capacity,
    processor::StreamingProcessorHandler& processor,
    common::Barrier& boundary_barrier, OutputWorker& output,
    performance::internal::TrackRecorder& performance,
    std::function<void(const char* worker, const char* error)> on_failed)
    : queue_capacity_(queue_capacity),
      processing_chain_(
          std::move(decoder), processor, performance,
          [this](const ffmpeg::Frame& frame) { output_.WriteVideo(frame); }),
      processor_(processor),
      boundary_barrier_(boundary_barrier),
      output_(output),
      performance_(performance),
      on_failed_(std::move(on_failed)) {}

VideoWorker::~VideoWorker() { Stop(); }

void VideoWorker::Start() {
  if (thread_) {
    throw std::logic_error("VideoWorker只能启动一次");
  }
  thread_ = std::make_unique<common::Thread>("mw-video", [this]() { Run(); });
}

bool VideoWorker::Input(const ffmpeg::Packet& packet) {
  if (recovering_) {
    if ((packet->flags & AV_PKT_FLAG_KEY) == 0) {
      performance_.RecordDroppedPackets(1);
      return false;
    }
    queue_.Push({WorkType::kDecoderReset, std::nullopt, false});
    queue_.Push({WorkType::kPacket, packet.Ref(), false});
    recovering_ = false;
    return true;
  }

  const bool accepted = queue_.TryPush(
      WorkItem{WorkType::kPacket, packet.Ref(), false}, queue_capacity_);
  if (!accepted) {
    performance_.RecordDroppedPackets(queue_.Clear() + 1);
    recovering_ = true;
  }
  return accepted;
}

void VideoWorker::Reset() {
  recovering_ = false;
  queue_.Clear();
  queue_.Push({WorkType::kTimelineReset, std::nullopt, false});
}

void VideoWorker::End(bool final_end) {
  queue_.Push({WorkType::kEnd, std::nullopt, final_end});
}

void VideoWorker::RequestStop() noexcept {
  queue_.Clear();
  queue_.Close();
}

void VideoWorker::Stop() noexcept {
  RequestStop();
  if (thread_ && !thread_->IsCurrent()) {
    thread_->Join();
  }
}

std::size_t VideoWorker::queue_depth() const { return queue_.size(); }

void VideoWorker::Run() noexcept {
  try {
    RunLoop();
  } catch (const std::exception& error) {
    on_failed_("video", error.what());
  } catch (...) {
    on_failed_("video", "未知异常");
  }
}

void VideoWorker::RunLoop() {
  while (auto work = queue_.WaitPop()) {
    switch (work->type) {
      case WorkType::kPacket:
        processing_chain_.Input(*work->packet);
        break;
      case WorkType::kDecoderReset:
        processing_chain_.Flush();
        break;
      case WorkType::kTimelineReset:
        processing_chain_.Flush();
        if (!SynchronizeBoundary(kMwStreamerProcessorTimelineReset)) {
          return;
        }
        output_.InterruptTrack(AVMEDIA_TYPE_VIDEO);
        break;
      case WorkType::kEnd:
        processing_chain_.Drain();
        if (work->final_end) {
          if (!SynchronizeBoundary(kMwStreamerProcessorEndOfInput)) {
            return;
          }
          output_.FinishTrack(AVMEDIA_TYPE_VIDEO);
          return;
        }
        output_.InterruptTrack(AVMEDIA_TYPE_VIDEO);
        break;
    }
  }
}

bool VideoWorker::SynchronizeBoundary(
    MwStreamerProcessorBoundaryReason reason) {
  return boundary_barrier_.ArriveAndWait(
      [this, reason]() { processor_.NotifyBoundary(reason); });
}

}  // namespace mw::streamer::pipeline::internal::streaming
