#include "mw/pipeline/internal/streaming/output_worker.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "mw/common/thread.h"
#include "mw/encoder/audio_encoder.h"
#include "mw/encoder/video_encoder.h"
#include "mw/output/output_session.h"
#include "mw/performance/internal/stage_recorder.h"
#include "mw/performance/internal/stopwatch.h"

namespace mw::streamer::pipeline::internal::streaming {

OutputWorker::OutputWorker(
    int audio_stream_index, int video_stream_index,
    encoder::AudioEncoderConfig audio_encoder_config,
    encoder::VideoEncoderConfig video_encoder_config,
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
      output_targets_(std::move(output_targets)),
      zlm_config_(std::move(zlm_config)),
      startup_packet_capacity_(startup_packet_capacity),
      poller_(std::move(poller)),
      callbacks_(std::move(callbacks)),
      audio_encoder_(
          has_audio_ ? std::make_unique<encoder::AudioEncoder>(
                           std::move(audio_encoder_config), audio_stream_index)
                     : nullptr),
      video_encoder_(
          has_video_ ? std::make_unique<encoder::VideoEncoder>(
                           std::move(video_encoder_config), video_stream_index)
                     : nullptr),
      synchronizer_(has_audio_, has_video_,
                    video_encoder_
                        ? AVRational{video_encoder_->config().frame_rate.num,
                                     video_encoder_->config().frame_rate.den}
                        : AVRational{0, 1},
                    max_track_wait, standby_enabled,
                    std::move(standby_image_path), hardware_context),
      audio_performance_(audio_performance),
      video_performance_(video_performance) {
  if (has_audio_ != (audio_performance_ != nullptr) ||
      has_video_ != (video_performance_ != nullptr)) {
    throw std::invalid_argument("OutputWorker性能记录器与轨道不匹配");
  }
  if (audio_encoder_) {
    audio_encoder_->SetOnPacket(
        [this](const ffmpeg::Packet& packet) { HandlePacket(packet); });
  }
  if (video_encoder_) {
    video_encoder_->SetOnPacket(
        [this](const ffmpeg::Packet& packet) { HandlePacket(packet); });
  }
}

OutputWorker::~OutputWorker() { Stop(); }

void OutputWorker::Start() {
  if (thread_) {
    throw std::logic_error("OutputWorker只能启动一次");
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
}

void OutputWorker::Stop() noexcept {
  RequestStop();
  if (thread_ && !thread_->IsCurrent()) {
    thread_->Join();
  }
  CloseOutput();
}

std::size_t OutputWorker::queue_depth() const { return queue_.size(); }

std::shared_ptr<output::OutputSession> OutputWorker::output_session() const {
  return std::atomic_load_explicit(&published_output_,
                                   std::memory_order_acquire);
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
  for (;;) {
    DrainReadyFrames();
    if (synchronizer_.finished()) {
      CompleteOutput();
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
    EncodeFrame(std::move(*frame));
  }
}

void OutputWorker::EncodeFrame(FrameSynchronizer::OutputFrame frame) {
  performance::internal::Stopwatch stopwatch;
  if (frame.media_type == AVMEDIA_TYPE_AUDIO) {
    if (!audio_encoder_) {
      throw std::logic_error("OutputWorker没有音频编码器");
    }
    if (!audio_encoder_->is_open()) {
      audio_encoder_->Open(frame.frame);
      if (AllEncodersOpen()) {
        PrepareOutput();
      }
    }
    stopwatch.Measure(
        [this, &frame]() { audio_encoder_->Encode(frame.frame); });
    audio_performance_->encode().Record(
        static_cast<std::uint64_t>(frame.frame->nb_samples),
        stopwatch.elapsed());
    return;
  }
  if (frame.media_type != AVMEDIA_TYPE_VIDEO || !video_encoder_) {
    throw std::logic_error("OutputWorker没有视频编码器");
  }
  if (!video_encoder_->is_open()) {
    video_encoder_->Open(frame.frame);
    if (AllEncodersOpen()) {
      PrepareOutput();
    }
  }
  stopwatch.Measure([this, &frame]() {
    video_encoder_->Encode(frame.frame,
                           frame.force_key_frame
                               ? encoder::VideoEncodeMode::kForceKeyFrame
                               : encoder::VideoEncodeMode::kAutomatic);
  });
  video_performance_->encode().Record(1, stopwatch.elapsed());
}

void OutputWorker::HandlePacket(const ffmpeg::Packet& packet) {
  if (output_targets_.empty()) {
    return;
  }
  if (output_) {
    output_->Write(packet);
    return;
  }
  if (pending_packets_.size() >= startup_packet_capacity_) {
    throw std::runtime_error("Output启动缓存已满");
  }
  pending_packets_.push_back(packet.Ref());
}

void OutputWorker::PrepareOutput() {
  if (ready_) {
    return;
  }
  if (output_targets_.empty()) {
    ready_ = true;
    callbacks_.on_ready();
    return;
  }
  auto output = std::make_shared<output::OutputSession>(
      output::OutputConfig{EncodedStreams(), output_targets_, zlm_config_},
      poller_);
  output->Open();
  output_ = output.get();
  std::atomic_store_explicit(&published_output_, std::move(output),
                             std::memory_order_release);
  for (const auto& packet : pending_packets_) {
    output_->Write(packet);
  }
  pending_packets_.clear();
  ready_ = true;
  callbacks_.on_ready();
}

void OutputWorker::CompleteOutput() {
  if (audio_encoder_) {
    if (!audio_encoder_->is_open()) {
      throw std::runtime_error("音频流没有产生可编码帧");
    }
    audio_encoder_->Drain();
  }
  if (video_encoder_) {
    if (!video_encoder_->is_open()) {
      throw std::runtime_error("视频流没有产生可编码帧");
    }
    video_encoder_->Drain();
  }
  CloseOutput();
  callbacks_.on_completed();
  queue_.Close();
}

void OutputWorker::CloseOutput() noexcept {
  output_ = nullptr;
  auto output = std::atomic_exchange_explicit(
      &published_output_, std::shared_ptr<output::OutputSession>{},
      std::memory_order_acq_rel);
  if (output) {
    output->Close();
  }
}

bool OutputWorker::AllEncodersOpen() const noexcept {
  return (!audio_encoder_ || audio_encoder_->is_open()) &&
         (!video_encoder_ || video_encoder_->is_open());
}

std::vector<ffmpeg::StreamInfo> OutputWorker::EncodedStreams() const {
  std::vector<ffmpeg::StreamInfo> streams;
  if (audio_encoder_) {
    streams.push_back(audio_encoder_->stream_info());
  }
  if (video_encoder_) {
    streams.push_back(video_encoder_->stream_info());
  }
  std::sort(streams.begin(), streams.end(),
            [](const auto& left, const auto& right) {
              return left.stream_index < right.stream_index;
            });
  return streams;
}

}  // namespace mw::streamer::pipeline::internal::streaming
