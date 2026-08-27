#include "mw/pipeline/internal/streaming/encoded_output_sink.h"

#include <algorithm>
#include <cstdint>
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

EncodedOutputSink::EncodedOutputSink(
    int audio_stream_index, int video_stream_index,
    encoder::AudioEncoderConfig audio_encoder_config,
    encoder::VideoEncoderConfig video_encoder_config,
    std::vector<std::string> output_targets, zlm::OutputConfig zlm_config,
    std::size_t queue_capacity, std::size_t startup_packet_capacity,
    performance::internal::TrackRecorder* audio_performance,
    performance::internal::TrackRecorder* video_performance,
    std::shared_ptr<toolkit::EventPoller> poller, Callbacks callbacks)
    : queue_capacity_(queue_capacity),
      output_targets_(std::move(output_targets)),
      zlm_config_(std::move(zlm_config)),
      startup_packet_capacity_(startup_packet_capacity),
      poller_(std::move(poller)),
      callbacks_(std::move(callbacks)),
      audio_encoder_(
          audio_stream_index >= 0
              ? std::make_unique<encoder::AudioEncoder>(
                    std::move(audio_encoder_config), audio_stream_index)
              : nullptr),
      video_encoder_(
          video_stream_index >= 0
              ? std::make_unique<encoder::VideoEncoder>(
                    std::move(video_encoder_config), video_stream_index)
              : nullptr),
      audio_performance_(audio_performance),
      video_performance_(video_performance) {
  if (queue_capacity_ == 0) {
    throw std::invalid_argument("EncodedOutputSink队列容量必须大于0");
  }
  if (!audio_encoder_ && !video_encoder_) {
    throw std::invalid_argument("EncodedOutputSink至少需要一条媒体轨道");
  }
  if ((audio_encoder_ != nullptr) != (audio_performance_ != nullptr) ||
      (video_encoder_ != nullptr) != (video_performance_ != nullptr)) {
    throw std::invalid_argument("EncodedOutputSink性能记录器与轨道不匹配");
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

EncodedOutputSink::~EncodedOutputSink() { Stop(); }

void EncodedOutputSink::Start() {
  if (thread_) {
    throw std::logic_error("EncodedOutputSink只能启动一次");
  }
  thread_ = std::make_unique<common::Thread>("mw-encoded", [this]() { Run(); });
}

bool EncodedOutputSink::Write(const ffmpeg::Frame& frame,
                              AVMediaType media_type, bool force_key_frame) {
  if (media_type != AVMEDIA_TYPE_AUDIO && media_type != AVMEDIA_TYPE_VIDEO) {
    throw std::invalid_argument("EncodedOutputSink收到未知媒体帧");
  }
  if ((media_type == AVMEDIA_TYPE_AUDIO && !audio_encoder_) ||
      (media_type == AVMEDIA_TYPE_VIDEO && !video_encoder_)) {
    throw std::invalid_argument("EncodedOutputSink收到未配置的媒体轨道");
  }
  const bool accepted = queue_.TryPush(
      WorkItem{frame.Ref(), media_type, force_key_frame}, queue_capacity_);
  if (!accepted && !queue_.closed()) {
    aborted_.store(true, std::memory_order_release);
    queue_.Clear();
    queue_.Close();
    ReportFailure("编码输出队列已满");
  }
  return accepted;
}

void EncodedOutputSink::Finish() { queue_.Close(); }

void EncodedOutputSink::Abort() noexcept {
  aborted_.store(true, std::memory_order_release);
  queue_.Clear();
  queue_.Close();
}

void EncodedOutputSink::Stop() noexcept {
  Abort();
  if (thread_ && !thread_->IsCurrent()) {
    thread_->Join();
  }
  CloseOutput();
}

std::size_t EncodedOutputSink::queue_depth() const { return queue_.size(); }

std::shared_ptr<output::OutputSession> EncodedOutputSink::output_session()
    const {
  return std::atomic_load_explicit(&published_output_,
                                   std::memory_order_acquire);
}

void EncodedOutputSink::Run() noexcept {
  try {
    RunLoop();
  } catch (const std::exception& error) {
    aborted_.store(true, std::memory_order_release);
    queue_.Clear();
    queue_.Close();
    CloseOutput();
    ReportFailure(error.what());
  } catch (...) {
    aborted_.store(true, std::memory_order_release);
    queue_.Clear();
    queue_.Close();
    CloseOutput();
    ReportFailure("未知异常");
  }
}

void EncodedOutputSink::RunLoop() {
  while (auto work = queue_.WaitPop()) {
    if (aborted_.load(std::memory_order_acquire)) {
      return;
    }
    EncodeFrame(std::move(*work));
  }
  if (!aborted_.load(std::memory_order_acquire)) {
    CompleteOutput();
  }
}

void EncodedOutputSink::EncodeFrame(WorkItem work) {
  performance::internal::Stopwatch stopwatch;
  if (work.media_type == AVMEDIA_TYPE_AUDIO) {
    if (!audio_encoder_->is_open()) {
      audio_encoder_->Open(work.frame);
      if (AllEncodersOpen()) {
        PrepareOutput();
      }
    }
    stopwatch.Measure([this, &work]() { audio_encoder_->Encode(work.frame); });
    audio_performance_->encode().Record(
        static_cast<std::uint64_t>(work.frame->nb_samples),
        stopwatch.elapsed());
    return;
  }
  if (!video_encoder_->is_open()) {
    video_encoder_->Open(work.frame);
    if (AllEncodersOpen()) {
      PrepareOutput();
    }
  }
  stopwatch.Measure([this, &work]() {
    video_encoder_->Encode(work.frame,
                           work.force_key_frame
                               ? encoder::VideoEncodeMode::kForceKeyFrame
                               : encoder::VideoEncodeMode::kAutomatic);
  });
  video_performance_->encode().Record(1, stopwatch.elapsed());
}

void EncodedOutputSink::HandlePacket(const ffmpeg::Packet& packet) {
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

void EncodedOutputSink::PrepareOutput() {
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

void EncodedOutputSink::CompleteOutput() {
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
}

void EncodedOutputSink::CloseOutput() noexcept {
  output_ = nullptr;
  auto output = std::atomic_exchange_explicit(
      &published_output_, std::shared_ptr<output::OutputSession>{},
      std::memory_order_acq_rel);
  if (output) {
    output->Close();
  }
}

void EncodedOutputSink::ReportFailure(const char* error) noexcept {
  if (!failure_reported_.exchange(true, std::memory_order_acq_rel)) {
    callbacks_.on_failed(error);
  }
}

bool EncodedOutputSink::AllEncodersOpen() const noexcept {
  return (!audio_encoder_ || audio_encoder_->is_open()) &&
         (!video_encoder_ || video_encoder_->is_open());
}

std::vector<ffmpeg::StreamInfo> EncodedOutputSink::EncodedStreams() const {
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
