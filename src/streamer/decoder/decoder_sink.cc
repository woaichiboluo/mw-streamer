#include "mw/streamer/decoder/decoder_sink.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mw/log.h"
#include "mw/streamer/cache/packet_queue.h"
#include "mw/streamer/common/barrier.h"
#include "mw/streamer/common/blocking_queue.h"
#include "mw/streamer/common/thread.h"
#include "mw/streamer/decoder/audio_decoder.h"
#include "mw/streamer/decoder/video_decoder.h"
#include "mw/streamer/performance/operation_recorder.h"
#include "mw/streamer/resampler/audio_resampler.h"
#include "mw/streamer/sink/fatal_error.h"

namespace mw::streamer {

class DecoderSink::Impl final : public Sink {
 public:
  Impl(DecoderSink& owner, DecoderSinkConfig config)
      : Sink(owner.id() + "/queue", SinkMediaType::kPacket),
        owner_(owner),
        config_(std::move(config)),
        outputs_(owner.downstream()) {
    if (config_.audio_decode_queue_capacity == 0 ||
        config_.video_decode_queue_capacity == 0) {
      throw std::invalid_argument("解码队列容量必须大于0");
    }
    queue_ = std::make_unique<PacketQueue>(config_.cache_duration, *this);
  }

  ~Impl() override { Stop(); }

  void SubmitStreams(const StreamsReady& streams) noexcept {
    Guard([&]() {
      const bool offline =
          streams.delivery_mode == StreamDeliveryMode::kOffline;
      if (mode_initialized_ && offline != offline_) {
        throw std::invalid_argument("DecoderSink不能跨代次改变输入投递模式");
      }
      if (offline && config_.cache_duration.count() != 0) {
        throw std::invalid_argument("离线文件解码不能配置播放缓存时长");
      }
      offline_ = offline;
      mode_initialized_ = true;
      if (offline_) {
        OnStreamsReady(streams);
      } else {
        queue_->OnStreamsReady(streams);
      }
    });
  }

  void SubmitPacket(const PacketReady& packet) noexcept {
    Guard([&]() {
      if (offline_) {
        OnPacket(packet);
      } else {
        queue_->OnPacket(packet);
      }
    });
  }

  void SubmitReset(const TimelineReset& reset) noexcept {
    Guard([&]() {
      if (offline_) {
        OnTimelineReset(reset);
      } else {
        queue_->OnTimelineReset(reset);
      }
    });
  }

  void SubmitEnd(const StreamEnded& end) noexcept {
    Guard([&]() {
      if (offline_) {
        OnInputEnded(end);
      } else {
        queue_->OnInputEnded(end);
      }
    });
  }

  void RequestStop() noexcept override {
    stopping_.store(true);
    queue_->Abort();
    CancelWork();
  }

  void Stop() noexcept override {
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    if (stopped_) {
      return;
    }
    RequestStop();
    queue_->Stop();
    for (auto& track : tracks_) {
      if (track->worker) {
        track->worker->Join();
      }
    }
    owner_.StopDownstream();
    SetState(PacketSinkState::kStopped);
    stopped_ = true;
  }

  PacketSinkState state() const noexcept {
    const auto current = state_.load();
    const auto queued = queue_->state();
    if (current == PacketSinkState::kFailed ||
        queued == PacketSinkState::kFailed) {
      return PacketSinkState::kFailed;
    }
    if (current == PacketSinkState::kRunning &&
        queued == PacketSinkState::kDraining &&
        queue_->generation() == generation_.load()) {
      return PacketSinkState::kDraining;
    }
    return current;
  }

  std::string error() const {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (!error_.empty()) {
        return error_;
      }
    }
    return queue_->error();
  }

  NodeSnapshot GetOwnPerformance() const override {
    NodeSnapshot snapshot;
    snapshot.name = "DecoderSink";
    snapshot.operations = {audio_performance_.GetSnapshot(),
                           video_performance_.GetSnapshot()};
    return snapshot;
  }

  void HandleFatalError(const std::string& error) noexcept override {
    ReportDownstreamFatal(error);
  }

 private:
  // This Sink consumes queue output. The public DecoderSink submits
  // input through Submit* instead, so due packets never re-enter the cache.
  void OnStreamsReady(const StreamsReady& streams) noexcept override {
    Guard([&]() { Configure(streams); });
  }

  void OnPacket(const PacketReady& packet) noexcept override {
    Guard([&]() { ForwardPacket(packet.generation, packet.packet); });
  }

  void OnTimelineReset(const TimelineReset& reset) noexcept override {
    Guard([&]() {
      pending_reset_ = reset;
      for (auto& track : tracks_) {
        track->queue.EraseIf(IsPacket);
        track->recovering = false;
      }
    });
  }

  void OnInputEnded(const StreamEnded& end) noexcept override {
    Guard([&]() {
      SetState(PacketSinkState::kDraining);
      if (end.reason == StreamEndReason::kStopped ||
          end.reason == StreamEndReason::kFailed) {
        for (auto& track : tracks_) {
          track->queue.EraseIf(IsPacket);
        }
      }
      QueueEnd(end);
    });
  }

  enum class WorkKind { kPacket, kDecoderReset, kTimelineReset, kEnd };

  struct Work {
    WorkKind kind = WorkKind::kPacket;
    std::uint64_t generation = 0;
    std::optional<Packet> packet;
    std::optional<TimelineReset> reset;
    std::optional<StreamsReady> streams;
    std::optional<StreamEnded> end;
  };

  static bool IsPacket(const Work& work) {
    return work.kind == WorkKind::kPacket;
  }

  struct Track {
    int stream_index = -1;
    std::uint64_t generation = 0;
    // Only the PacketQueue scheduling thread reads/writes recovery state.
    bool recovering = false;
    BlockingQueue<Work> queue;
    std::unique_ptr<AudioDecoder> audio;
    std::unique_ptr<AudioResampler> resampler;
    std::unique_ptr<VideoDecoder> video;
    std::unique_ptr<Thread> worker;
    // Accessed only by this track's decode worker and synchronous callbacks.
    OperationRecorder::Call* active_call = nullptr;
  };

  template <typename Function>
  void Guard(Function&& function) noexcept {
    try {
      if (!CanProcess()) {
        return;
      }
      std::forward<Function>(function)();
    } catch (const FatalError& exception) {
      Fail(exception.what(), true);
    } catch (const std::exception& exception) {
      Fail(exception.what());
    } catch (...) {
      Fail("解码链路发生未知异常");
    }
  }

  // Queue failure can occur after its End was delivered, while decoders are
  // still draining. Consume that state at work boundaries instead of requiring
  // a second End notification. Never call this while holding the barrier lock.
  bool CanProcess() {
    if (stopping_.load() || failed_.load()) {
      return false;
    }
    if (queue_->state() == PacketSinkState::kFailed) {
      const auto error = queue_->error();
      Fail(error.c_str());
      return false;
    }
    return true;
  }

  void Configure(const StreamsReady& streams) {
    if (outputs_.empty()) {
      throw std::logic_error("DecoderSink至少需要一个下游Sink");
    }
    if (streams.generation == 0 || streams.generation <= generation_.load()) {
      return;
    }
    if (!tracks_.empty() &&
        (!pending_reset_ || pending_reset_->generation != streams.generation)) {
      throw std::logic_error("新输入代次必须先投递时间线重置");
    }
    generation_.store(streams.generation);
    if (tracks_.empty()) {
      OpenTracks(streams);
    } else {
      Work work;
      work.kind = WorkKind::kTimelineReset;
      work.generation = streams.generation;
      work.reset = *pending_reset_;
      work.streams = streams;
      for (auto& track : tracks_) {
        track->queue.EraseIf(IsPacket);
        track->recovering = false;
        track->queue.Push(work);
      }
    }
    pending_reset_.reset();
    SetState(PacketSinkState::kRunning);
  }

  void OpenTracks(const StreamsReady& streams) {
    std::lock_guard<std::mutex> lock(work_mutex_);
    bool has_audio = false;
    bool has_video = false;
    for (const auto& stream : streams.streams) {
      const auto type = stream.codec_parameters.get()->codec_type;
      if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) {
        continue;
      }
      if ((type == AVMEDIA_TYPE_AUDIO && std::exchange(has_audio, true)) ||
          (type == AVMEDIA_TYPE_VIDEO && std::exchange(has_video, true))) {
        throw std::invalid_argument("DecoderSink每种媒体类型仅支持一路轨道");
      }
      auto track = std::make_unique<Track>();
      track->stream_index = stream.stream_index;
      track->generation = streams.generation;
      if (type == AVMEDIA_TYPE_AUDIO) {
        track->audio =
            std::make_unique<AudioDecoder>(stream, config_.audio_decoder);
        track->resampler = std::make_unique<AudioResampler>(stream);
        track->audio->SetOnFrame([raw = track.get()](const Frame& frame) {
          raw->resampler->Resample(frame);
        });
        track->resampler->SetOnFrame(
            [this, raw = track.get()](const Frame& frame) {
              audio_performance_.AddOutput(frame->nb_samples);
              if (CanProcess()) {
                const FrameReady ready{raw->generation, frame.Ref()};
                OperationRecorder::Suspension pause(raw->active_call);
                for (const auto& output : outputs_) {
                  output->OnAudioFrame(ready);
                }
              }
            });
      } else {
        track->video =
            std::make_unique<VideoDecoder>(stream, config_.video_decoder);
        track->video->SetOnFrame([this, raw = track.get()](const Frame& frame) {
          video_performance_.AddOutput(1);
          if (CanProcess()) {
            const FrameReady ready{raw->generation, frame.Ref()};
            OperationRecorder::Suspension pause(raw->active_call);
            for (const auto& output : outputs_) {
              output->OnVideoFrame(ready);
            }
          }
        });
      }
      tracks_.push_back(std::move(track));
    }
    if (tracks_.empty()) {
      throw std::invalid_argument("DecoderSink需要音频或视频轨道");
    }
    barrier_ = std::make_unique<Barrier>(tracks_.size());
    const FrameStreamsReady ready{streams.generation, streams.streams,
                                  HardwareContext()};
    owner_.StartMessages();
    for (const auto& output : outputs_) {
      output->OnStreamsReady(ready);
    }
    for (auto& track : tracks_) {
      track->worker = std::make_unique<Thread>(
          track->audio ? "mw-dec-audio" : "mw-dec-video",
          [this, raw = track.get()]() { Run(*raw); });
    }
  }

  const HardwareContext* HardwareContext() const {
    for (const auto& track : tracks_) {
      if (track->video) {
        return track->video->hardware_context();
      }
    }
    return nullptr;
  }

  void ForwardPacket(std::uint64_t generation, const Packet& packet) {
    if (generation != generation_.load()) {
      return;
    }
    for (auto& track : tracks_) {
      if (track->stream_index != packet->stream_index) {
        continue;
      }
      Work work;
      work.generation = generation;
      work.packet = packet.Ref();
      if (offline_) {
        const auto capacity = track->audio
                                  ? config_.audio_decode_queue_capacity
                                  : config_.video_decode_queue_capacity;
        track->queue.WaitPush(std::move(work), capacity, IsPacket);
      } else if (track->audio) {
        track->queue.TryPush(std::move(work),
                             config_.audio_decode_queue_capacity, IsPacket);
      } else if (track->recovering) {
        if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
          Work reset;
          reset.kind = WorkKind::kDecoderReset;
          reset.generation = generation;
          track->queue.Push(std::move(reset));
          track->queue.Push(std::move(work));
          track->recovering = false;
        }
      } else if (!track->queue.TryPush(std::move(work),
                                       config_.video_decode_queue_capacity,
                                       IsPacket)) {
        track->queue.EraseIf(IsPacket);
        track->recovering = true;
      }
      return;
    }
  }

  void QueueEnd(const StreamEnded& end) {
    Work work;
    work.kind = WorkKind::kEnd;
    work.generation = end.generation;
    work.end = end;
    for (auto& track : tracks_) {
      track->queue.Push(work);
    }
  }

  static void Flush(Track& track) {
    if (track.audio) {
      track.audio->Flush();
      track.resampler->Flush();
    } else {
      track.video->Flush();
    }
  }

  void Run(Track& track) noexcept {
    Guard([&]() {
      while (CanProcess()) {
        auto work = track.queue.WaitPop();
        if (!work || !CanProcess() || !ProcessWork(track, *work)) {
          return;
        }
      }
    });
  }

  bool ProcessWork(Track& track, const Work& work) {
    switch (work.kind) {
      case WorkKind::kPacket: {
        auto& recorder = track.audio ? audio_performance_ : video_performance_;
        OperationRecorder::Call call(recorder, track.active_call);
        recorder.AddInput(1, work.packet->get()->size);
        track.generation = work.generation;
        if (track.audio) {
          track.audio->Decode(*work.packet);
        } else {
          track.video->Decode(*work.packet);
        }
        break;
      }
      case WorkKind::kDecoderReset:
        Flush(track);
        break;
      case WorkKind::kTimelineReset:
        Flush(track);
        track.generation = work.generation;
        return barrier_->ArriveAndWait(
            [&]() { CompleteReset(*work.reset, *work.streams); });
      case WorkKind::kEnd:
        return EndTrack(track, *work.end);
    }
    return true;
  }

  void CompleteReset(const TimelineReset& reset, const StreamsReady& streams) {
    // Barrier completion holds its lock; CanProcess may cancel that barrier.
    if (stopping_.load() || failed_.load() ||
        queue_->state() == PacketSinkState::kFailed) {
      return;
    }
    for (const auto& output : outputs_) {
      output->OnTimelineReset(reset);
    }
    const FrameStreamsReady ready{streams.generation, streams.streams,
                                  HardwareContext()};
    owner_.StartMessages();
    for (const auto& output : outputs_) {
      output->OnStreamsReady(ready);
    }
  }

  bool EndTrack(Track& track, const StreamEnded& end) {
    if (end.reason != StreamEndReason::kEof &&
        end.reason != StreamEndReason::kInterrupted) {
      Flush(track);
    } else {
      auto& recorder = track.audio ? audio_performance_ : video_performance_;
      OperationRecorder::Call call(recorder, track.active_call);
      if (track.audio) {
        track.audio->Drain();
        track.resampler->Drain();
      } else {
        track.video->Drain();
      }
    }
    return barrier_->ArriveAndWait([&]() { CompleteEnd(end); });
  }

  void CompleteEnd(const StreamEnded& end) {
    if (stopping_.load() || failed_.load() ||
        queue_->state() == PacketSinkState::kFailed) {
      return;
    }
    for (const auto& output : outputs_) {
      output->OnInputEnded(end);
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (end.generation != generation_.load() || stopping_.load() ||
        failed_.load() || queue_->state() == PacketSinkState::kFailed) {
      return;
    }
    if (end.reason == StreamEndReason::kEof) {
      state_.store(PacketSinkState::kEnded);
    } else if (end.reason == StreamEndReason::kInterrupted) {
      state_.store(PacketSinkState::kRunning);
    } else if (end.reason == StreamEndReason::kStopped) {
      state_.store(PacketSinkState::kStopped);
    } else if (end.reason == StreamEndReason::kFailed) {
      error_ = "输入失败";
      failed_.store(true);
      state_.store(PacketSinkState::kFailed);
    }
  }

  void SetState(PacketSinkState state) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (state_.load() != PacketSinkState::kFailed) {
      state_.store(state);
    }
  }

  bool RecordFailure(const char* error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (failed_.exchange(true)) {
      return false;
    }
    error_ = error;
    state_.store(PacketSinkState::kFailed);
    return true;
  }

  void ReportDownstreamFatal(const std::string& error) noexcept {
    RecordFailure(error.c_str());
    owner_.ReportFatalError(error);
    // Do not take work_mutex_ or cancel a barrier here: startup may hold those
    // locks while waiting for this downstream worker to exit. The coordinating
    // owner performs Stop; failed_ already prevents further decoder work.
  }

  void Fail(const char* error, bool fatal = false) noexcept {
    const bool first_failure = RecordFailure(error);
    if (fatal) {
      owner_.ReportFatalError(error);
    }
    queue_->Abort();
    CancelWork();
    if (first_failure) {
      MW_LOG_ERROR("streamer", "DecoderSink失败: {}", error);
    }
  }

  void CancelWork() {
    std::lock_guard<std::mutex> lock(work_mutex_);
    if (barrier_) {
      barrier_->Cancel();
    }
    // Stopping/failure is already visible to workers. Closing rejects new
    // writes; a concurrent pop is discarded by CanProcess before execution.
    for (auto& track : tracks_) {
      track->queue.Close();
      track->queue.Clear();
    }
  }

  DecoderSink& owner_;
  DecoderSinkConfig config_;
  // Setup is serialized on the input thread; queue forwarding reads the mode.
  bool mode_initialized_ = false;
  std::atomic<bool> offline_{false};
  OperationRecorder audio_performance_{PerformanceType::kAudioDecoder,
                                       PerformanceUnit::kPacket,
                                       PerformanceUnit::kSample};
  OperationRecorder video_performance_{PerformanceType::kVideoDecoder,
                                       PerformanceUnit::kPacket,
                                       PerformanceUnit::kFrame};
  const std::vector<std::unique_ptr<Sink>>& outputs_;
  std::unique_ptr<PacketQueue> queue_;
  std::vector<std::unique_ptr<Track>> tracks_;
  std::unique_ptr<Barrier> barrier_;
  std::mutex work_mutex_;
  std::mutex stop_mutex_;
  mutable std::mutex status_mutex_;
  std::string error_;
  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<bool> failed_{false};
  std::atomic<bool> stopping_{false};
  bool stopped_ = false;
  // The queue thread retains the reset until replacement streams arrive, so
  // both decode workers can receive one complete generation boundary.
  std::optional<TimelineReset> pending_reset_;
};

DecoderSink::DecoderSink(std::string id, DecoderSinkConfig config)
    : Sink(std::move(id), SinkMediaType::kPacket, SinkMediaType::kFrame),
      impl_(std::make_unique<Impl>(*this, std::move(config))) {}

DecoderSink::~DecoderSink() { Stop(); }

void DecoderSink::OnStreamsReady(const StreamsReady& streams) noexcept {
  CloseRegistration();
  impl_->SubmitStreams(streams);
}

void DecoderSink::OnPacket(const PacketReady& packet) noexcept {
  CloseRegistration();
  impl_->SubmitPacket(packet);
}

void DecoderSink::OnTimelineReset(const TimelineReset& reset) noexcept {
  CloseRegistration();
  impl_->SubmitReset(reset);
}

void DecoderSink::OnInputEnded(const StreamEnded& end) noexcept {
  CloseRegistration();
  impl_->SubmitEnd(end);
}

void DecoderSink::RequestStop() noexcept {
  Sink::RequestStop();
  impl_->RequestStop();
}

void DecoderSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

PacketSinkState DecoderSink::state() const noexcept { return impl_->state(); }

std::string DecoderSink::error() const { return impl_->error(); }

NodeSnapshot DecoderSink::GetOwnPerformance() const {
  return impl_->GetOwnPerformance();
}

void DecoderSink::HandleFatalError(const std::string& error) noexcept {
  impl_->HandleFatalError(error);
}

}  // namespace mw::streamer
