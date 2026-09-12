#include "mw/encoder/encoder_sink.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mw/common/blocking_queue.h"
#include "mw/common/thread.h"
#include "mw/encoder/audio_encoder.h"
#include "mw/encoder/video_encoder.h"
#include "mw/log/logging.h"
#include "mw/performance/operation_recorder.h"

namespace mw::streamer {

class EncoderSink::Impl final {
 public:
  Impl(EncoderSink& owner, EncoderSinkConfig config)
      : owner_(owner),
        config_(std::move(config)),
        outputs_(owner.downstream()) {
    if (config_.frame_queue_capacity == 0 ||
        config_.startup_packet_capacity == 0) {
      throw std::invalid_argument("EncoderSink队列容量必须大于0");
    }
  }

  ~Impl() { Stop(); }

  void OnStreamsReady(const FrameStreamsReady& streams) {
    Submit([&]() {
      Work work;
      work.kind = WorkKind::kReady;
      work.generation = streams.generation;
      work.streams = streams.source_streams;
      return work;
    });
  }

  void OnFrame(const FrameReady& frame, bool video) {
    Submit([&]() {
      Work work;
      work.kind = video ? WorkKind::kVideo : WorkKind::kAudio;
      work.generation = frame.generation;
      work.frame = frame.frame;
      return work;
    });
  }

  void OnTimelineReset(const TimelineReset& reset) {
    Submit([&]() {
      queue_.EraseIf([&](const Work& work) {
        return IsFrame(work) && work.generation < reset.generation;
      });
      Work work;
      work.kind = WorkKind::kReset;
      work.generation = reset.generation;
      work.reset = reset;
      return work;
    });
  }

  void OnInputEnded(const StreamEnded& end) {
    Submit([&]() {
      Work work;
      work.kind = WorkKind::kEnd;
      work.generation = end.generation;
      work.end = end;
      return work;
    });
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    {
      // Creating the worker and closing admission share a lock. Never hold it
      // while joining: callbacks may still be submitting an asynchronous fatal.
      std::lock_guard<std::mutex> input_lock(input_mutex_);
      if (stopped_) {
        return;
      }
      stopping_.store(true);
      queue_.Close();
      queue_.Clear();
    }
    if (worker_) {
      worker_->Join();
    }
    owner_.StopDownstream();
    SetState(EncoderSinkState::kStopped);
    stopped_ = true;
  }

  EncoderSinkState state() const noexcept { return state_.load(); }

  std::string error() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return error_;
  }

  std::size_t queue_depth() const { return queue_.size(); }

  NodeSnapshot GetOwnPerformance() const {
    NodeSnapshot snapshot;
    snapshot.name = "EncoderSink";
    snapshot.operations = {audio_performance_.GetSnapshot(),
                           video_performance_.GetSnapshot()};
    return snapshot;
  }

  void HandleFatalError(const std::string& error) noexcept {
    Fail(error.c_str(), true);
  }

 private:
  enum class WorkKind { kReady, kAudio, kVideo, kReset, kEnd };

  struct Work {
    WorkKind kind = WorkKind::kReady;
    std::uint64_t generation = 0;
    std::vector<StreamInfo> streams;
    std::optional<Frame> frame;
    std::optional<TimelineReset> reset;
    std::optional<StreamEnded> end;
  };

  static bool IsFrame(const Work& work) {
    return work.kind == WorkKind::kAudio || work.kind == WorkKind::kVideo;
  }

  bool CanProcess() const { return !stopping_.load() && !failed_.load(); }

  template <typename MakeWork>
  void Submit(MakeWork make_work) noexcept {
    try {
      std::lock_guard<std::mutex> lock(input_mutex_);
      if (!CanProcess() || queue_.closed()) {
        return;
      }
      if (outputs_.empty()) {
        throw std::logic_error("EncoderSink至少需要一个下游Sink");
      }
      if (!worker_) {
        worker_ =
            std::make_unique<Thread>("mw-encoder", [this]() { Run(); });
      }
      if (!queue_.TryPush(make_work(), config_.frame_queue_capacity, IsFrame) &&
          !queue_.closed()) {
        throw std::runtime_error("EncoderSink编码帧队列已满");
      }
    } catch (const FatalError& error) {
      Fail(error.what(), true);
    } catch (const std::exception& error) {
      Fail(error.what());
    } catch (...) {
      Fail("EncoderSink提交发生未知异常");
    }
  }

  void Run() noexcept {
    try {
      while (auto work = queue_.WaitPop()) {
        if (!CanProcess()) {
          break;
        }
        Process(*work);
      }
    } catch (const FatalError& error) {
      Fail(error.what(), true);
    } catch (const std::exception& error) {
      Fail(error.what());
    } catch (...) {
      Fail("EncoderSink编码发生未知异常");
    }
    if (failed_.load()) {
      NotifyEnd(StreamEndReason::kFailed);
    }
    DiscardEncoders();
  }

  void Process(const Work& work) {
    switch (work.kind) {
      case WorkKind::kReady:
        Configure(work);
        return;
      case WorkKind::kAudio:
      case WorkKind::kVideo:
        EncodeFrame(work);
        return;
      case WorkKind::kReset:
        Reset(*work.reset);
        return;
      case WorkKind::kEnd:
        End(*work.end);
        return;
    }
  }

  void Configure(const Work& work) {
    if (work.generation == 0) {
      throw std::invalid_argument("EncoderSink输入代次必须大于0");
    }
    if (work.generation < generation_) {
      return;
    }
    if (source_ready_ ||
        (pending_reset_ && work.generation != *pending_reset_)) {
      throw std::logic_error("EncoderSink新输入代次必须先投递时间线重置");
    }
    if (generation_ != 0 && !pending_reset_) {
      throw std::logic_error("EncoderSink新输入代次缺少时间线重置");
    }
    if (work.streams.empty()) {
      throw std::invalid_argument("EncoderSink输入没有媒体轨道");
    }
    generation_ = work.generation;
    for (const auto& stream : work.streams) {
      ConfigureTrack(stream);
    }
    if (audio_encoder_ && video_encoder_ && audio_index_ == video_index_) {
      throw std::invalid_argument("EncoderSink输入轨道索引重复");
    }
    source_ready_ = true;
    input_ended_ = false;
    pending_reset_.reset();
    SetState(EncoderSinkState::kRunning);
    owner_.StartMessages();
  }

  void ConfigureTrack(const StreamInfo& stream) {
    stream.Validate();
    const auto type = stream.codec_parameters.get()->codec_type;
    if (type == AVMEDIA_TYPE_AUDIO) {
      if (audio_encoder_) {
        throw std::invalid_argument("EncoderSink只支持一路音频轨道");
      }
      audio_index_ = stream.stream_index;
      audio_encoder_ = std::make_unique<AudioEncoder>(
          config_.audio_encoder, audio_index_);
      audio_encoder_->SetOnPacket([this](const Packet& packet) {
        audio_performance_.AddOutput(1, packet->size);
        HandlePacket(packet);
      });
      return;
    }
    if (video_encoder_) {
      throw std::invalid_argument("EncoderSink只支持一路视频轨道");
    }
    video_index_ = stream.stream_index;
    video_encoder_ = std::make_unique<VideoEncoder>(
        config_.video_encoder, video_index_);
    video_encoder_->SetOnPacket([this](const Packet& packet) {
      video_performance_.AddOutput(1, packet->size);
      HandlePacket(packet);
    });
  }

  bool IsCurrentGeneration(std::uint64_t generation) const {
    if (generation < generation_) {
      return false;
    }
    if (!source_ready_ || generation != generation_ || input_ended_) {
      throw std::logic_error("EncoderSink收到未就绪或已结束代次的数据");
    }
    return true;
  }

  void EncodeFrame(const Work& work) {
    if (!IsCurrentGeneration(work.generation)) {
      return;
    }
    if (work.kind == WorkKind::kAudio) {
      EncodeTrack(audio_encoder_, *work.frame, audio_performance_,
                  std::max(0, work.frame->get()->nb_samples));
      return;
    }
    EncodeTrack(video_encoder_, *work.frame, video_performance_, 1);
  }

  template <typename Encoder>
  void EncodeTrack(const std::unique_ptr<Encoder>& encoder,
                   const Frame& frame,
                   OperationRecorder& recorder,
                   std::uint64_t input_count) {
    OperationRecorder::Call call(recorder, active_call_);
    recorder.AddInput(input_count);
    if (!encoder) {
      throw std::invalid_argument("EncoderSink收到未声明轨道的帧");
    }
    if (!encoder->is_open()) {
      encoder->Open(frame);
      PublishStreams();
    }
    if (CanProcess()) {
      encoder->Encode(frame);
    }
  }

  bool AllEncodersOpen() const {
    return (!audio_encoder_ || audio_encoder_->is_open()) &&
           (!video_encoder_ || video_encoder_->is_open());
  }

  void PublishStreams() {
    if (output_ready_ || !AllEncodersOpen() || !CanProcess()) {
      return;
    }
    StreamsReady ready{generation_, {}};
    if (audio_encoder_) {
      ready.streams.push_back(audio_encoder_->stream_info());
    }
    if (video_encoder_) {
      ready.streams.push_back(video_encoder_->stream_info());
    }
    std::sort(ready.streams.begin(), ready.streams.end(),
              [](const auto& left, const auto& right) {
                return left.stream_index < right.stream_index;
              });
    output_ready_ = true;
    output_ended_ = false;
    for (auto& output : outputs_) {
      if (!CanProcess()) {
        return;
      }
      ++ready_outputs_;
      published_outputs_ = ready_outputs_;
      OperationRecorder::Suspension pause(active_call_);
      output->OnStreamsReady(ready);
    }
    for (const auto& packet : pending_packets_) {
      ForwardPacket(packet);
    }
    pending_packets_.clear();
  }

  void HandlePacket(const Packet& packet) {
    if (!CanProcess()) {
      return;
    }
    if (output_ready_) {
      ForwardPacket(packet);
      return;
    }
    if (pending_packets_.size() >= config_.startup_packet_capacity) {
      throw std::runtime_error("EncoderSink编码启动包缓存已满");
    }
    pending_packets_.push_back(packet.Ref());
  }

  void ForwardPacket(const Packet& packet) {
    const PacketReady ready{generation_, packet.Ref()};
    for (auto& output : outputs_) {
      if (!CanProcess()) {
        return;
      }
      OperationRecorder::Suspension pause(active_call_);
      output->OnPacket(ready);
    }
  }

  void Reset(const TimelineReset& reset) {
    if (reset.generation == 0 || reset.generation <= generation_) {
      return;
    }
    DiscardEncoders();
    // Finish the old generation's last packet before delivering this boundary.
    // Codec delay is discarded; only new-generation encoders can emit after it.
    for (std::size_t index = 0; index < published_outputs_; ++index) {
      if (!CanProcess()) {
        return;
      }
      outputs_[index]->OnTimelineReset(reset);
    }
    ready_outputs_ = 0;
    generation_ = reset.generation;
    pending_reset_ = reset.generation;
    source_ready_ = false;
    input_ended_ = false;
    output_ready_ = false;
    output_ended_ = false;
    SetState(EncoderSinkState::kRunning);
    owner_.StartMessages();
  }

  void End(const StreamEnded& end) {
    if (!IsCurrentGeneration(end.generation)) {
      return;
    }
    if (end.reason == StreamEndReason::kEof) {
      CompleteEncoding();
    }
    if (!CanProcess()) {
      return;
    }
    NotifyEnd(end.reason);
    input_ended_ = true;
    DiscardEncoders();
    if (end.reason == StreamEndReason::kInterrupted) {
      return;
    }
    queue_.Close();
    queue_.Clear();
    if (end.reason == StreamEndReason::kFailed) {
      Fail("EncoderSink上游输入失败");
    } else {
      SetState(end.reason == StreamEndReason::kEof
                   ? EncoderSinkState::kEnded
                   : EncoderSinkState::kStopped);
    }
  }

  void CompleteEncoding() {
    if (!AllEncodersOpen()) {
      throw std::runtime_error("EncoderSink声明的轨道没有产生可编码帧");
    }
    SetState(EncoderSinkState::kDraining);
    if (audio_encoder_ && CanProcess()) {
      OperationRecorder::Call call(audio_performance_,
                                                active_call_);
      audio_encoder_->Drain();
    }
    if (video_encoder_ && CanProcess()) {
      OperationRecorder::Call call(video_performance_,
                                                active_call_);
      video_encoder_->Drain();
    }
  }

  void NotifyEnd(StreamEndReason reason) noexcept {
    if (output_ended_) {
      return;
    }
    output_ended_ = true;
    const StreamEnded end{generation_, reason};
    for (std::size_t index = 0; index < ready_outputs_; ++index) {
      outputs_[index]->OnInputEnded(end);
    }
  }

  void DiscardEncoders() noexcept {
    audio_encoder_.reset();
    video_encoder_.reset();
    pending_packets_.clear();
  }

  void SetState(EncoderSinkState state) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (!failed_.load()) {
      state_.store(state);
    }
  }

  void Fail(const char* error, bool fatal = false) noexcept {
    bool first_failure;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      first_failure = !failed_.exchange(true);
      if (first_failure) {
        error_ = error;
        state_.store(EncoderSinkState::kFailed);
      }
    }
    queue_.Close();
    queue_.Clear();
    if (fatal) {
      owner_.ReportFatalError(error);
    }
    if (first_failure) {
      Module<LogModule::kStreamer>::Error("EncoderSink失败: {}",
                                                    error);
    }
  }

  EncoderSink& owner_;
  const EncoderSinkConfig config_;
  OperationRecorder audio_performance_{
      PerformanceType::kAudioEncoder,
      PerformanceUnit::kSample,
      PerformanceUnit::kPacket};
  OperationRecorder video_performance_{
      PerformanceType::kVideoEncoder,
      PerformanceUnit::kFrame,
      PerformanceUnit::kPacket};
  const std::vector<std::unique_ptr<Sink>>& outputs_;
  BlockingQueue<Work> queue_;
  std::unique_ptr<Thread> worker_;
  std::mutex input_mutex_;
  std::mutex stop_mutex_;
  mutable std::mutex status_mutex_;
  std::atomic<EncoderSinkState> state_{EncoderSinkState::kIdle};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> failed_{false};
  std::string error_;
  bool stopped_ = false;

  // Execution state below belongs exclusively to the encoding worker.
  OperationRecorder::Call* active_call_ = nullptr;
  std::uint64_t generation_ = 0;
  std::optional<std::uint64_t> pending_reset_;
  bool source_ready_ = false;
  bool input_ended_ = false;
  bool output_ready_ = false;
  bool output_ended_ = false;
  std::size_t ready_outputs_ = 0;
  // Reset may supersede a generation before any track produces its first frame.
  // Previously exposed consumers still need every replacement reset.
  std::size_t published_outputs_ = 0;
  int audio_index_ = -1;
  int video_index_ = -1;
  std::unique_ptr<AudioEncoder> audio_encoder_;
  std::unique_ptr<VideoEncoder> video_encoder_;
  std::vector<Packet> pending_packets_;
};

EncoderSink::EncoderSink(std::string id, EncoderSinkConfig config)
    : Sink(std::move(id), SinkMediaType::kFrame,
                 SinkMediaType::kPacket),
      impl_(std::make_unique<Impl>(*this, std::move(config))) {}

EncoderSink::~EncoderSink() { Stop(); }

void EncoderSink::OnStreamsReady(const FrameStreamsReady& streams) {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void EncoderSink::OnAudioFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnFrame(frame, false);
}

void EncoderSink::OnVideoFrame(const FrameReady& frame) {
  CloseRegistration();
  impl_->OnFrame(frame, true);
}

void EncoderSink::OnTimelineReset(const TimelineReset& reset) {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void EncoderSink::OnInputEnded(const StreamEnded& end) {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void EncoderSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

EncoderSinkState EncoderSink::state() const noexcept { return impl_->state(); }

std::string EncoderSink::error() const { return impl_->error(); }

std::size_t EncoderSink::queue_depth() const { return impl_->queue_depth(); }

NodeSnapshot EncoderSink::GetOwnPerformance() const {
  return impl_->GetOwnPerformance();
}

void EncoderSink::HandleFatalError(const std::string& error) noexcept {
  impl_->HandleFatalError(error);
}

}  // namespace mw::streamer
