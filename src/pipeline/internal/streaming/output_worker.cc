#include "mw/pipeline/internal/streaming/output_worker.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "mw/common/thread.h"
#include "mw/log/logging.h"
#include "mw/output/internal/output_sink_worker.h"
#include "mw/output/output_sink.h"
#include "mw/pipeline/internal/streaming/encoded_output_sink.h"

namespace mw::streamer::pipeline::internal::streaming {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

}  // namespace

OutputWorker::OutputWorker(
    int audio_stream_index, int video_stream_index,
    encoder::AudioEncoderConfig audio_encoder_config,
    encoder::VideoEncoderConfig video_encoder_config,
    std::vector<std::unique_ptr<output::OutputSink>> output_sinks,
    std::vector<std::string> output_targets, zlm::OutputConfig zlm_config,
    std::size_t startup_packet_capacity,
    std::chrono::milliseconds max_track_wait, bool standby_enabled,
    std::string standby_image_path,
    const ffmpeg::HardwareContext* hardware_context,
    performance::internal::TrackRecorder* audio_performance,
    performance::internal::TrackRecorder* video_performance,
    std::shared_ptr<toolkit::EventPoller> poller, Callbacks callbacks)
    : has_audio_(audio_stream_index >= 0),
      has_video_(video_stream_index >= 0),
      callbacks_(std::move(callbacks)),
      synchronizer_(has_audio_, has_video_,
                    has_video_ ? AVRational{video_encoder_config.frame_rate.num,
                                            video_encoder_config.frame_rate.den}
                               : AVRational{0, 1},
                    max_track_wait, standby_enabled,
                    std::move(standby_image_path), hardware_context) {
  if (startup_packet_capacity == 0) {
    throw std::invalid_argument("OutputWorker Sink队列容量必须大于0");
  }
  raw_active_.assign(output_sinks.size(), true);
  raw_ready_.assign(output_sinks.size(), false);
  raw_completed_.assign(output_sinks.size(), false);
  encoded_active_ = !output_targets.empty();
  encoded_completed_ = !encoded_active_;

  raw_sinks_.reserve(output_sinks.size());
  for (std::size_t index = 0; index < output_sinks.size(); ++index) {
    raw_sinks_.push_back(std::make_unique<output::internal::OutputSinkWorker>(
        startup_packet_capacity, std::move(output_sinks[index]),
        output::internal::OutputSinkWorker::Callbacks{
            [this, index]() { MarkRawReady(index); },
            [this, index]() { HandleRawCompleted(index); },
            [this, index](const char* error) { HandleRawFailed(index, error); },
        }));
  }
  if (encoded_active_) {
    encoded_sink_ = std::make_unique<EncodedOutputSink>(
        audio_stream_index, video_stream_index, std::move(audio_encoder_config),
        std::move(video_encoder_config), std::move(output_targets),
        std::move(zlm_config), startup_packet_capacity, startup_packet_capacity,
        audio_performance, video_performance, std::move(poller),
        EncodedOutputSink::Callbacks{
            [this]() { HandleEncodedReady(); },
            [this]() { HandleEncodedCompleted(); },
            [this](const char* error) { HandleEncodedFailed(error); },
        });
  }
}

OutputWorker::~OutputWorker() { Stop(); }

void OutputWorker::Start() {
  if (thread_) {
    throw std::logic_error("OutputWorker只能启动一次");
  }
  for (auto& sink : raw_sinks_) {
    sink->Start();
  }
  if (encoded_sink_) {
    encoded_sink_->Start();
  }
  thread_ = std::make_unique<common::Thread>("mw-output", [this]() { Run(); });
}

void OutputWorker::WriteAudio(const ffmpeg::Frame& frame) {
  queue_.Push({WorkType::kFrame, AVMEDIA_TYPE_AUDIO, frame.Ref()});
}

void OutputWorker::WriteVideo(const ffmpeg::Frame& frame) {
  queue_.Push({WorkType::kFrame, AVMEDIA_TYPE_VIDEO, frame.Ref()});
}

void OutputWorker::InterruptTrack(AVMediaType media_type) {
  queue_.Push({WorkType::kInterrupt, media_type, std::nullopt});
}

void OutputWorker::FinishTrack(AVMediaType media_type) {
  queue_.Push({WorkType::kFinish, media_type, std::nullopt});
}

void OutputWorker::RequestStop() noexcept {
  queue_.Clear();
  queue_.Close();
  AbortSinks();
}

void OutputWorker::Stop() noexcept {
  RequestStop();
  if (thread_ && !thread_->IsCurrent()) {
    thread_->Join();
  }
  for (auto& sink : raw_sinks_) {
    sink->Abort();
  }
  if (encoded_sink_) {
    encoded_sink_->Stop();
  }
}

std::size_t OutputWorker::queue_depth() const {
  std::size_t depth = queue_.size();
  for (const auto& sink : raw_sinks_) {
    depth += sink->queue_depth();
  }
  if (encoded_sink_) {
    depth += encoded_sink_->queue_depth();
  }
  return depth;
}

std::shared_ptr<output::OutputSession> OutputWorker::output_session() const {
  return encoded_sink_ ? encoded_sink_->output_session() : nullptr;
}

void OutputWorker::Run() noexcept {
  try {
    RunLoop();
  } catch (const std::exception& error) {
    callbacks_.on_failed(error.what());
  } catch (...) {
    callbacks_.on_failed("未知异常");
  }
}

void OutputWorker::RunLoop() {
  MaybeNotifyReady();
  for (;;) {
    DrainReadyFrames();
    if (synchronizer_.finished()) {
      FinishSinks();
      queue_.Close();
      return;
    }

    std::optional<WorkItem> work;
    if (const auto deadline = synchronizer_.deadline()) {
      work = queue_.WaitPopUntil(*deadline);
    } else {
      work = queue_.WaitPop();
    }
    if (!work) {
      if (queue_.closed()) {
        return;
      }
      continue;
    }
    HandleWork(std::move(*work));
  }
}

void OutputWorker::HandleWork(WorkItem work) {
  switch (work.type) {
    case WorkType::kFrame:
      if (work.media_type == AVMEDIA_TYPE_AUDIO) {
        synchronizer_.PushAudio(std::move(*work.frame));
      } else if (work.media_type == AVMEDIA_TYPE_VIDEO) {
        synchronizer_.PushVideo(std::move(*work.frame));
      } else {
        throw std::invalid_argument("OutputWorker收到未知媒体帧");
      }
      break;
    case WorkType::kInterrupt:
      synchronizer_.Interrupt(work.media_type);
      break;
    case WorkType::kFinish:
      synchronizer_.Finish(work.media_type);
      break;
  }
}

void OutputWorker::DrainReadyFrames() {
  while (auto frame =
             synchronizer_.TakeReady(FrameSynchronizer::Clock::now())) {
    DispatchFrame(std::move(*frame));
  }
}

void OutputWorker::DispatchFrame(FrameSynchronizer::OutputFrame frame) {
  std::vector<std::size_t> raw_indices;
  bool write_encoded = false;
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    raw_indices.reserve(raw_active_.size());
    for (std::size_t index = 0; index < raw_active_.size(); ++index) {
      if (raw_active_[index]) {
        raw_indices.push_back(index);
      }
    }
    write_encoded = encoded_active_;
  }

  for (const auto index : raw_indices) {
    const bool accepted = frame.media_type == AVMEDIA_TYPE_AUDIO
                              ? raw_sinks_[index]->WriteAudio(frame.frame)
                              : raw_sinks_[index]->WriteVideo(frame.frame);
    if (!accepted) {
      HandleRawFailed(index, "Raw Output Sink已停止接收帧");
    }
  }
  if (write_encoded && !encoded_sink_->Write(frame.frame, frame.media_type,
                                             frame.force_key_frame)) {
    HandleEncodedFailed("Encoded Output Sink已停止接收帧");
  }
}

void OutputWorker::FinishSinks() {
  if (encoded_sink_) {
    encoded_sink_->Finish();
  }
  for (auto& sink : raw_sinks_) {
    sink->RequestFinish();
  }
  MaybeNotifyCompleted();
}

void OutputWorker::HandleEncodedReady() {
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    if (!encoded_active_) {
      return;
    }
    encoded_ready_ = true;
  }
  MaybeNotifyReady();
}

void OutputWorker::HandleEncodedCompleted() {
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    encoded_completed_ = true;
  }
  MaybeNotifyCompleted();
}

void OutputWorker::HandleEncodedFailed(const char* error) noexcept {
  bool no_sink = false;
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    if (!encoded_active_) {
      return;
    }
    encoded_active_ = false;
    encoded_completed_ = true;
    no_sink = std::none_of(raw_active_.begin(), raw_active_.end(),
                           [](bool active) { return active; });
  }
  Log::Error("Encoded Output Sink失败，已与其他Sink隔离: {}", error);
  if (no_sink) {
    callbacks_.on_failed(error);
    return;
  }
  MaybeNotifyReady();
  MaybeNotifyCompleted();
}

void OutputWorker::HandleRawCompleted(std::size_t index) {
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    raw_completed_[index] = true;
  }
  MaybeNotifyCompleted();
}

void OutputWorker::HandleRawFailed(std::size_t index,
                                   const char* error) noexcept {
  bool no_sink = false;
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    if (!raw_active_[index]) {
      return;
    }
    raw_active_[index] = false;
    raw_completed_[index] = true;
    no_sink =
        !encoded_active_ && std::none_of(raw_active_.begin(), raw_active_.end(),
                                         [](bool active) { return active; });
  }
  Log::Error("Raw Output Sink失败，已与其他Sink隔离: {}", error);
  if (no_sink) {
    callbacks_.on_failed(error);
    return;
  }
  MaybeNotifyReady();
  MaybeNotifyCompleted();
}

void OutputWorker::MarkRawReady(std::size_t index) {
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    if (!raw_active_[index] || raw_ready_[index]) {
      return;
    }
    raw_ready_[index] = true;
  }
  MaybeNotifyReady();
}

void OutputWorker::MaybeNotifyReady() {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    const bool raw_ready =
        std::equal(raw_active_.begin(), raw_active_.end(), raw_ready_.begin(),
                   [](bool active, bool ready) { return !active || ready; });
    if (!ready_notified_ && raw_ready && (!encoded_active_ || encoded_ready_)) {
      ready_notified_ = true;
      notify = true;
    }
  }
  if (notify) {
    callbacks_.on_ready();
  }
}

void OutputWorker::MaybeNotifyCompleted() {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(sink_state_mutex_);
    const bool raw_completed =
        std::all_of(raw_completed_.begin(), raw_completed_.end(),
                    [](bool completed) { return completed; });
    if (!completed_notified_ && raw_completed && encoded_completed_) {
      completed_notified_ = true;
      notify = true;
    }
  }
  if (notify) {
    callbacks_.on_completed();
  }
}

void OutputWorker::AbortSinks() noexcept {
  if (encoded_sink_) {
    encoded_sink_->Abort();
  }
  for (auto& sink : raw_sinks_) {
    sink->RequestAbort();
  }
}

}  // namespace mw::streamer::pipeline::internal::streaming
