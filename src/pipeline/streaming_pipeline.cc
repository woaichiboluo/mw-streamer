#include "mw/pipeline/streaming_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include <fmt/format.h>

#include "Network/Socket.h"
#include "Poller/EventPoller.h"
#include "mw/cache/packet_queue.h"
#include "mw/common/barrier.h"
#include "mw/decoder/video_decoder.h"
#include "mw/input/player_proxy.h"
#include "mw/log/logging.h"
#include "mw/pipeline/internal/streaming/audio_worker.h"
#include "mw/pipeline/internal/streaming/output_worker.h"
#include "mw/pipeline/internal/streaming/video_worker.h"
#include "mw/processor/internal/source_info_adapter.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;
using internal::streaming::AudioWorker;
using internal::streaming::OutputWorker;
using internal::streaming::VideoWorker;

}  // namespace

class StreamingPipeline::Impl final {
 public:
  explicit Impl(StreamingPipelineConfig config) : config_(std::move(config)) {}

  ~Impl() = default;

  void SetProcessorCallbacks(
      const MwStreamerStreamingProcessorCallbacks& callbacks) {
    RequireIdle("设置Processor回调");
    processor_callbacks_ = callbacks;
  }

  void SetOnStatus(OnStatus callback) {
    RequireIdle("设置Pipeline状态回调");
    on_status_ = std::move(callback);
  }

  void Start() {
    RequireIdle("启动StreamingPipeline");
    ValidateConfig();

    poller_ = toolkit::EventPollerPool::Instance().getPoller();
    player_ =
        std::make_unique<input::PlayerProxy>(poller_, config_.reconnect_policy);
    packet_queue_ =
        std::make_unique<cache::PacketQueue>(config_.cache_duration, poller_);
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      status_.store(StreamingPipelineStatus::kStarting,
                    std::memory_order_release);
      NotifyStatus(StreamingPipelineStatus::kStarting);
    }
    BindInputCallbacks();
    player_->Start(config_.input_url, config_.zlm.player);
    Log::Info("StreamingPipeline开始启动: input={}, outputs={}",
              config_.input_url, config_.output_targets.size());
  }

  void UpdateProcessorConfig(std::string config) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    switch (status()) {
      case StreamingPipelineStatus::kIdle:
        config_.processor.config = std::move(config);
        return;
      case StreamingPipelineStatus::kStarting:
        if (!processor_) {
          config_.processor.config = std::move(config);
          return;
        }
        processor_->UpdateConfig(std::move(config));
        return;
      case StreamingPipelineStatus::kRunning:
        processor_->UpdateConfig(std::move(config));
        return;
      case StreamingPipelineStatus::kFailed:
      case StreamingPipelineStatus::kStopped:
        throw std::logic_error("Processor配置不能在Pipeline停止后更新");
    }
    throw std::logic_error("StreamingPipeline包含未知状态");
  }

  void Stop() noexcept {
    StopSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      stopping_.store(true, std::memory_order_release);
      snapshot = SnapshotLocked();
    }

    RequestQuiesce(snapshot);

    std::future<void> player_stopped;
    if (snapshot.player) {
      auto completed = std::make_shared<std::promise<void>>();
      player_stopped = completed->get_future();
      snapshot.player->Stop(
          [completed = std::move(completed)]() { completed->set_value(); });
    }
    if (snapshot.packet_queue) {
      snapshot.packet_queue->Stop();
    }

    if (snapshot.audio_worker) {
      snapshot.audio_worker->Stop();
    }
    if (snapshot.video_worker) {
      snapshot.video_worker->Stop();
    }
    if (snapshot.output_worker) {
      snapshot.output_worker->Stop();
    }
    if (player_stopped.valid()) {
      player_stopped.wait();
    }
    if (snapshot.processor) {
      snapshot.processor->Stop();
    }

    std::unique_ptr<input::PlayerProxy> player;
    std::unique_ptr<cache::PacketQueue> packet_queue;
    std::unique_ptr<processor::StreamingProcessorHandler> processor;
    std::unique_ptr<common::Barrier> boundary_barrier;
    std::unique_ptr<OutputWorker> output_worker;
    std::unique_ptr<AudioWorker> audio_worker;
    std::unique_ptr<VideoWorker> video_worker;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      player = std::move(player_);
      packet_queue = std::move(packet_queue_);
      processor = std::move(processor_);
      boundary_barrier = std::move(boundary_barrier_);
      output_worker = std::move(output_worker_);
      audio_worker = std::move(audio_worker_);
      video_worker = std::move(video_worker_);
      if (status() != StreamingPipelineStatus::kFailed) {
        const auto previous = status_.exchange(
            StreamingPipelineStatus::kStopped, std::memory_order_acq_rel);
        if (previous != StreamingPipelineStatus::kStopped) {
          NotifyStatus(StreamingPipelineStatus::kStopped);
        }
      }
    }
    if (snapshot.poller && !snapshot.poller->isCurrentThread()) {
      snapshot.poller->sync([]() {});
    }

    video_worker.reset();
    audio_worker.reset();
    output_worker.reset();
    boundary_barrier.reset();
    processor.reset();
    player.reset();
    packet_queue.reset();

    Log::Info("StreamingPipeline停止完成: input={}, failed={}",
              config_.input_url, status() == StreamingPipelineStatus::kFailed);
  }

  StreamingPipelineStatus status() const noexcept {
    return status_.load(std::memory_order_acquire);
  }

 private:
  void RequireIdle(const char* operation) const {
    if (status() != StreamingPipelineStatus::kIdle) {
      throw std::logic_error(fmt::format("{}只能在Idle状态执行", operation));
    }
  }

  void ValidateConfig() const {
    if (config_.input_url.empty()) {
      throw std::invalid_argument("StreamingPipeline输入地址不能为空");
    }
    if (config_.output_targets.empty()) {
      throw std::invalid_argument("StreamingPipeline至少需要一个输出目标");
    }
    if (config_.audio_queue_capacity == 0 ||
        config_.video_queue_capacity == 0) {
      throw std::invalid_argument("StreamingPipeline工作队列容量必须大于0");
    }
  }

  void BindInputCallbacks() {
    packet_queue_->SetOnPacket(
        [this](std::uint64_t generation, const ffmpeg::Packet& packet) {
          OnDuePacket(generation, packet);
        });
    packet_queue_->SetOnTimelineReset(
        [this](std::uint64_t generation) { OnTimelineReset(generation); });
    packet_queue_->SetOnGenerationEnd(
        [this](std::uint64_t generation) { OnGenerationEnd(generation); });

    player_->SetOnStreamsReady(
        [this](std::uint64_t generation,
               const std::vector<ffmpeg::StreamInfo>& streams) {
          OnStreamsReady(generation, streams);
        });
    player_->SetOnPacket(
        [this](std::uint64_t generation, const ffmpeg::Packet& packet) {
          return packet_queue_->Input(generation, packet);
        });
    player_->SetOnState(
        [this](std::uint64_t generation, input::PlayerState state,
               const toolkit::SockException& reason, bool will_retry) {
          OnPlayerState(generation, state, reason, will_retry);
        });
  }

  void OnStreamsReady(std::uint64_t generation,
                      const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (stopping_.load(std::memory_order_acquire)) {
          return;
        }
        if (!processor_) {
          auto chains = BuildChains(streams);
          processor_ = std::move(chains.processor);
          boundary_barrier_ = std::move(chains.boundary_barrier);
          output_worker_ = std::move(chains.output_worker);
          audio_worker_ = std::move(chains.audio_worker);
          video_worker_ = std::move(chains.video_worker);
          audio_stream_index_ = chains.audio_stream_index;
          video_stream_index_ = chains.video_stream_index;

          output_worker_->Start();
          if (audio_worker_) {
            audio_worker_->Start();
          }
          if (video_worker_) {
            video_worker_->Start();
          }
        }
      }
      packet_queue_->SetStreams(generation, streams);
    } catch (const std::exception& error) {
      ReportFatal("初始化媒体处理链路", error.what());
    } catch (...) {
      ReportFatal("初始化媒体处理链路", "未知异常");
    }
  }

  struct Chains {
    std::unique_ptr<processor::StreamingProcessorHandler> processor;
    std::unique_ptr<common::Barrier> boundary_barrier;
    std::unique_ptr<OutputWorker> output_worker;
    std::unique_ptr<AudioWorker> audio_worker;
    std::unique_ptr<VideoWorker> video_worker;
    int audio_stream_index = -1;
    int video_stream_index = -1;

    ~Chains() {
      if (processor) {
        processor->Stop();
      }
    }

    Chains() = default;
    Chains(Chains&&) noexcept = default;
    Chains& operator=(Chains&&) noexcept = default;
    Chains(const Chains&) = delete;
    Chains& operator=(const Chains&) = delete;
  };

  Chains BuildChains(const std::vector<ffmpeg::StreamInfo>& streams) {
    std::optional<ffmpeg::StreamInfo> audio_stream;
    std::optional<ffmpeg::StreamInfo> video_stream;
    for (const auto& stream : streams) {
      stream.Validate();
      const auto media_type = stream.codec_parameters.get()->codec_type;
      if (media_type == AVMEDIA_TYPE_AUDIO) {
        if (audio_stream) {
          throw std::invalid_argument("StreamingPipeline首版只支持一路音频");
        }
        audio_stream = stream;
      } else if (media_type == AVMEDIA_TYPE_VIDEO) {
        if (video_stream) {
          throw std::invalid_argument("StreamingPipeline首版只支持一路视频");
        }
        video_stream = stream;
      }
    }
    if (!audio_stream && !video_stream) {
      throw std::invalid_argument("StreamingPipeline输入不包含音频或视频");
    }

    std::unique_ptr<decoder::VideoDecoder> video_decoder;
    if (video_stream) {
      video_decoder = std::make_unique<decoder::VideoDecoder>(
          *video_stream, config_.video_decoder);
    }
    Chains chains;
    chains.processor = std::make_unique<processor::StreamingProcessorHandler>(
        processor::internal::MakeProcessorSourceInfo(audio_stream,
                                                     video_stream),
        video_decoder ? video_decoder->hardware_context() : nullptr);
    const MwStreamerStreamingProcessorConfig processor_config{
        config_.processor.output_width,
        config_.processor.output_height,
        config_.processor.config.c_str(),
    };
    if (chains.processor->Start(processor_config, processor_callbacks_) !=
        kMwStreamerProcessorStartSuccess) {
      throw std::runtime_error("Processor拒绝启动");
    }

    const auto startup_packet_capacity =
        std::max(config_.audio_queue_capacity, config_.video_queue_capacity);
    chains.boundary_barrier = std::make_unique<common::Barrier>(
        static_cast<std::size_t>(audio_stream.has_value()) +
        static_cast<std::size_t>(video_stream.has_value()));
    chains.output_worker = std::make_unique<OutputWorker>(
        audio_stream.has_value(), video_stream.has_value(),
        config_.output_targets, config_.zlm.output, startup_packet_capacity,
        poller_,
        OutputWorker::Callbacks{
            [this]() { SetRunning(); },
            [this]() { SetNaturallyStopped(); },
            [this](const char* error) { ReportFatal("output", error); },
        });
    std::function<void(const char*, const char*)> on_worker_failed =
        [this](const char* worker, const char* error) {
          ReportFatal(worker, error);
        };

    if (audio_stream) {
      chains.audio_stream_index = audio_stream->stream_index;
      chains.audio_worker = std::make_unique<AudioWorker>(
          *audio_stream, config_.audio_decoder, config_.audio_encoder,
          config_.audio_queue_capacity, *chains.processor,
          *chains.boundary_barrier, *chains.output_worker, on_worker_failed);
    }
    if (video_stream) {
      chains.video_stream_index = video_stream->stream_index;
      chains.video_worker = std::make_unique<VideoWorker>(
          std::move(video_decoder), config_.video_encoder,
          config_.video_queue_capacity, *chains.processor,
          *chains.boundary_barrier, *chains.output_worker,
          std::move(on_worker_failed));
    }
    return chains;
  }

  void SetNaturallyStopped() noexcept {
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      if (stopping_.load(std::memory_order_acquire) ||
          status() == StreamingPipelineStatus::kFailed) {
        return;
      }
      const auto previous = status_.exchange(StreamingPipelineStatus::kStopped,
                                             std::memory_order_acq_rel);
      if (previous != StreamingPipelineStatus::kStopped) {
        NotifyStatus(StreamingPipelineStatus::kStopped);
      }
    }
  }

  void OnDuePacket(std::uint64_t generation,
                   const ffmpeg::Packet& packet) noexcept {
    static_cast<void>(generation);
    if (stopping_.load(std::memory_order_acquire) || !packet.get()) {
      return;
    }
    try {
      bool accepted = true;
      bool* overflow = nullptr;
      const char* media_name = nullptr;
      if (audio_worker_ && packet->stream_index == audio_stream_index_) {
        accepted = audio_worker_->Input(packet);
        overflow = &audio_overflow_;
        media_name = "音频";
      } else if (video_worker_ && packet->stream_index == video_stream_index_) {
        accepted = video_worker_->Input(packet);
        overflow = &video_overflow_;
        media_name = "视频";
      }
      if (overflow) {
        ReportQueueResult(media_name, accepted, overflow);
      }
    } catch (const std::exception& error) {
      ReportFatal("投递媒体包", error.what());
    } catch (...) {
      ReportFatal("投递媒体包", "未知异常");
    }
  }

  void ReportQueueResult(const char* media_name, bool accepted,
                         bool* overflow) {
    if (!accepted) {
      if (!std::exchange(*overflow, true)) {
        Log::Warning("{}工作队列已满，开始丢弃输入AVPacket", media_name);
      }
      return;
    }
    if (std::exchange(*overflow, false)) {
      Log::Info("{}工作队列已恢复，停止丢弃输入AVPacket", media_name);
    }
  }

  void OnTimelineReset(std::uint64_t generation) noexcept {
    static_cast<void>(generation);
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    try {
      if (audio_worker_) {
        audio_worker_->Reset();
      }
      if (video_worker_) {
        video_worker_->Reset();
      }
    } catch (const std::exception& error) {
      ReportFatal("重置媒体时间线", error.what());
    } catch (...) {
      ReportFatal("重置媒体时间线", "未知异常");
    }
  }

  void OnGenerationEnd(std::uint64_t generation) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    try {
      const bool final_end = generation == final_generation_;
      if (audio_worker_) {
        audio_worker_->End(final_end);
      }
      if (video_worker_) {
        video_worker_->End(final_end);
      }
    } catch (const std::exception& error) {
      ReportFatal("结束媒体generation", error.what());
    } catch (...) {
      ReportFatal("结束媒体generation", "未知异常");
    }
  }

  void OnPlayerState(std::uint64_t generation, input::PlayerState state,
                     const toolkit::SockException& reason,
                     bool will_retry) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    if (state == input::PlayerState::kWaitingRetry) {
      packet_queue_->EndInput(generation);
      Log::Warning(
          "StreamingPipeline输入中断，等待重连: generation={}, error={}",
          generation, reason.what());
    } else if (state == input::PlayerState::kEnded) {
      final_generation_ = generation;
      packet_queue_->EndInput(generation);
    } else if (state == input::PlayerState::kFailed && !will_retry) {
      ReportFatal("输入停止且不会重试", reason.what());
    }
  }

  void SetRunning() noexcept {
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      if (stopping_.load(std::memory_order_acquire)) {
        return;
      }
      StreamingPipelineStatus expected = StreamingPipelineStatus::kStarting;
      if (status_.compare_exchange_strong(expected,
                                          StreamingPipelineStatus::kRunning,
                                          std::memory_order_acq_rel)) {
        NotifyStatus(StreamingPipelineStatus::kRunning);
      }
    }
  }

  void ReportFatal(const char* operation, const char* detail) noexcept {
    StopSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      if (stopping_.load(std::memory_order_acquire) ||
          status() == StreamingPipelineStatus::kFailed ||
          status() == StreamingPipelineStatus::kStopped) {
        return;
      }
      stopping_.store(true, std::memory_order_release);
      status_.store(StreamingPipelineStatus::kFailed,
                    std::memory_order_release);
      snapshot = SnapshotLocked();
      NotifyStatus(StreamingPipelineStatus::kFailed);
    }

    Log::Error("StreamingPipeline失败: operation={}, detail={}", operation,
               detail);
    RequestQuiesce(snapshot);
    if (snapshot.packet_queue) {
      snapshot.packet_queue->Stop();
    }
    if (snapshot.player) {
      snapshot.player->Stop();
    }
  }

  struct StopSnapshot {
    toolkit::EventPoller* poller = nullptr;
    input::PlayerProxy* player = nullptr;
    cache::PacketQueue* packet_queue = nullptr;
    processor::StreamingProcessorHandler* processor = nullptr;
    common::Barrier* boundary_barrier = nullptr;
    OutputWorker* output_worker = nullptr;
    AudioWorker* audio_worker = nullptr;
    VideoWorker* video_worker = nullptr;
  };

  StopSnapshot SnapshotLocked() const noexcept {
    return {
        poller_.get(),       player_.get(),           packet_queue_.get(),
        processor_.get(),    boundary_barrier_.get(), output_worker_.get(),
        audio_worker_.get(), video_worker_.get(),
    };
  }

  static void RequestQuiesce(const StopSnapshot& snapshot) noexcept {
    if (snapshot.boundary_barrier) {
      snapshot.boundary_barrier->Cancel();
    }
    if (snapshot.audio_worker) {
      snapshot.audio_worker->RequestStop();
    }
    if (snapshot.video_worker) {
      snapshot.video_worker->RequestStop();
    }
    if (snapshot.output_worker) {
      snapshot.output_worker->RequestStop();
    }
  }

  void NotifyStatus(StreamingPipelineStatus status) noexcept {
    if (!on_status_) {
      return;
    }
    try {
      on_status_(status);
    } catch (const std::exception& error) {
      Log::Error("StreamingPipeline状态回调异常，已隔离: {}", error.what());
    } catch (...) {
      Log::Error("StreamingPipeline状态回调异常，已隔离: 未知异常");
    }
  }

  StreamingPipelineConfig config_;
  MwStreamerStreamingProcessorCallbacks processor_callbacks_{};
  OnStatus on_status_;

  std::atomic<StreamingPipelineStatus> status_{StreamingPipelineStatus::kIdle};
  std::atomic_bool stopping_{false};
  std::mutex control_mutex_;

  std::shared_ptr<toolkit::EventPoller> poller_;
  std::unique_ptr<input::PlayerProxy> player_;
  std::unique_ptr<cache::PacketQueue> packet_queue_;
  std::unique_ptr<processor::StreamingProcessorHandler> processor_;
  std::unique_ptr<common::Barrier> boundary_barrier_;
  std::unique_ptr<OutputWorker> output_worker_;
  std::unique_ptr<AudioWorker> audio_worker_;
  std::unique_ptr<VideoWorker> video_worker_;

  int audio_stream_index_ = -1;
  int video_stream_index_ = -1;
  bool audio_overflow_ = false;
  bool video_overflow_ = false;
  std::uint64_t final_generation_ = 0;
};

StreamingPipeline::StreamingPipeline(StreamingPipelineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

StreamingPipeline::~StreamingPipeline() {
  if (impl_) {
    impl_->Stop();
  }
}

void StreamingPipeline::SetProcessorCallbacks(
    const MwStreamerStreamingProcessorCallbacks& callbacks) {
  impl_->SetProcessorCallbacks(callbacks);
}

void StreamingPipeline::SetOnStatus(OnStatus callback) {
  impl_->SetOnStatus(std::move(callback));
}

void StreamingPipeline::Start() { impl_->Start(); }

void StreamingPipeline::UpdateProcessorConfig(std::string config) {
  impl_->UpdateProcessorConfig(std::move(config));
}

void StreamingPipeline::Stop() noexcept { impl_->Stop(); }

StreamingPipelineStatus StreamingPipeline::status() const noexcept {
  return impl_->status();
}

}  // namespace mw::streamer::pipeline
