#include "mw/pipeline/remux_pipeline.h"

#include <fmt/format.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "Network/Socket.h"
#include "Poller/EventPoller.h"
#include "mw/input/player_proxy.h"
#include "mw/log/logging.h"
#include "mw/output/output_session.h"
#include "mw/performance/internal/remux_collector.h"
#include "mw/pipeline/internal/remux/source_output_worker.h"

namespace mw::streamer::pipeline {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;
using internal::remux::SourceOutputWorker;
using internal::remux::SourceOutputWorkerState;

constexpr std::size_t kSourceQueueCapacity = 384;

bool IsNetworkInput(const std::string& url) {
  return url.find("://") != std::string::npos;
}

}  // namespace

class RemuxPipeline::Impl final {
 public:
  explicit Impl(RemuxPipelineConfig config) : config_(std::move(config)) {}

  void SetOnStatus(OnStatus callback) {
    RequireIdle("设置Pipeline状态回调");
    on_status_ = std::move(callback);
  }

  void Start() {
    RequireIdle("启动RemuxPipeline");
    ValidateConfig();

    performance_.Reset();
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      poller_ = toolkit::EventPollerPool::Instance().getPoller();
      player_ = std::make_shared<input::PlayerProxy>(poller_,
                                                     config_.reconnect_policy);
      output_worker_ = std::make_unique<SourceOutputWorker>(
          config_.output_targets, config_.zlm.output, kSourceQueueCapacity,
          poller_, [this](const char* error) { ReportFatal(error); });
      status_.store(RemuxPipelineStatus::kStarting, std::memory_order_release);
      NotifyStatus(RemuxPipelineStatus::kStarting);
    }
    BindInputCallbacks();
    player_->Start(config_.input_url, config_.zlm.player);
    Log::Info("RemuxPipeline开始启动: input={}, outputs={}", config_.input_url,
              config_.output_targets.size());
  }

  void Stop() noexcept {
    StopSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      stopping_.store(true, std::memory_order_release);
      snapshot = SnapshotLocked();
    }

    if (snapshot.output_worker) {
      snapshot.output_worker->RequestStop();
    }

    std::future<void> player_stopped;
    if (snapshot.player) {
      auto completed = std::make_shared<std::promise<void>>();
      player_stopped = completed->get_future();
      snapshot.player->Stop(
          [completed = std::move(completed)]() { completed->set_value(); });
    }
    if (snapshot.output_worker) {
      snapshot.output_worker->Stop();
    }
    if (player_stopped.valid()) {
      player_stopped.wait();
    }

    std::shared_ptr<input::PlayerProxy> player;
    std::unique_ptr<SourceOutputWorker> output_worker;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      player = std::move(player_);
      output_worker = std::move(output_worker_);
      if (status() != RemuxPipelineStatus::kFailed) {
        const auto previous = status_.exchange(RemuxPipelineStatus::kStopped,
                                               std::memory_order_acq_rel);
        if (previous != RemuxPipelineStatus::kStopped) {
          NotifyStatus(RemuxPipelineStatus::kStopped);
        }
      }
    }
    if (snapshot.poller && !snapshot.poller->isCurrentThread()) {
      snapshot.poller->sync([]() {});
    }
    output_worker.reset();
    player.reset();

    Log::Info("RemuxPipeline停止完成: input={}, failed={}", config_.input_url,
              status() == RemuxPipelineStatus::kFailed);
  }

  RemuxPipelineStatus status() const noexcept {
    return status_.load(std::memory_order_acquire);
  }

  performance::RemuxPipelineSnapshot CollectPerformance() {
    std::shared_ptr<input::PlayerProxy> player;
    std::shared_ptr<output::OutputSession> output;
    std::size_t output_queue_depth = 0;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      player = player_;
      if (output_worker_) {
        output_queue_depth = output_worker_->queue_depth();
        output = output_worker_->output_session();
      }
    }

    performance::NetworkInputSnapshot network_input;
    network_input.is_network = IsNetworkInput(config_.input_url);
    if (player) {
      network_input.connected = player->state() == input::PlayerState::kReady;
      network_input.generation = player->generation();
      network_input.reconnect_count = player->reconnect_count();
      if (network_input.is_network) {
        network_input.received_bytes = player->received_bytes();
      }
    }

    std::vector<performance::NetworkOutputSnapshot> outputs;
    if (output) {
      auto traffic = output->GetNetworkTraffic();
      outputs.reserve(traffic.size());
      for (auto& target : traffic) {
        outputs.push_back({
            std::move(target.target),
            target.connected,
            target.reconnect_count,
            target.sent_bytes,
        });
      }
    }
    return performance_.Collect(output_queue_depth, std::move(network_input),
                                std::move(outputs));
  }

 private:
  struct StopSnapshot {
    toolkit::EventPoller* poller = nullptr;
    input::PlayerProxy* player = nullptr;
    SourceOutputWorker* output_worker = nullptr;
  };

  void RequireIdle(const char* operation) const {
    if (status() != RemuxPipelineStatus::kIdle) {
      throw std::logic_error(fmt::format("{}只能在Idle状态执行", operation));
    }
  }

  void ValidateConfig() const {
    if (config_.input_url.empty()) {
      throw std::invalid_argument("RemuxPipeline输入地址不能为空");
    }
    if (config_.output_targets.empty()) {
      throw std::invalid_argument("RemuxPipeline至少需要一个输出目标");
    }
  }

  void BindInputCallbacks() {
    player_->SetOnStreamsReady(
        [this](std::uint64_t, const std::vector<ffmpeg::StreamInfo>& streams) {
          OnStreamsReady(streams);
        });
    player_->SetOnPacket(
        [this](std::uint64_t generation, const ffmpeg::Packet& packet) {
          if (stopping_.load(std::memory_order_acquire) || !output_worker_) {
            return false;
          }
          const bool accepted = output_worker_->Write(generation, packet);
          if (accepted) {
            const auto* raw_packet = packet.get();
            performance_.RecordPacket(
                raw_packet && raw_packet->size > 0
                    ? static_cast<std::size_t>(raw_packet->size)
                    : 0);
          }
          return accepted;
        });
    player_->SetOnState(
        [this](std::uint64_t generation, input::PlayerState state,
               const toolkit::SockException& reason, bool will_retry) {
          OnPlayerState(generation, state, reason, will_retry);
        });
  }

  void OnStreamsReady(const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
    if (stopping_.load(std::memory_order_acquire) || !output_worker_) {
      return;
    }
    output_worker_->Open(streams);
    if (output_worker_->state() != SourceOutputWorkerState::kRunning) {
      return;
    }

    std::lock_guard<std::mutex> lock(control_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    RemuxPipelineStatus expected = RemuxPipelineStatus::kStarting;
    if (status_.compare_exchange_strong(expected, RemuxPipelineStatus::kRunning,
                                        std::memory_order_acq_rel)) {
      NotifyStatus(RemuxPipelineStatus::kRunning);
    }
  }

  void OnPlayerState(std::uint64_t generation, input::PlayerState state,
                     const toolkit::SockException& reason,
                     bool will_retry) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    if (state == input::PlayerState::kWaitingRetry) {
      Log::Warning("RemuxPipeline输入中断，等待重连: generation={}, error={}",
                   generation, reason.what());
      return;
    }
    if (state == input::PlayerState::kEnded) {
      CompleteNaturally();
      return;
    }
    if (state == input::PlayerState::kFailed && !will_retry) {
      ReportFatal(reason.what());
    }
  }

  void CompleteNaturally() noexcept {
    if (output_worker_) {
      output_worker_->Stop();
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (stopping_.load(std::memory_order_acquire) ||
        status() == RemuxPipelineStatus::kFailed) {
      return;
    }
    const auto previous = status_.exchange(RemuxPipelineStatus::kStopped,
                                           std::memory_order_acq_rel);
    if (previous != RemuxPipelineStatus::kStopped) {
      NotifyStatus(RemuxPipelineStatus::kStopped);
    }
  }

  void ReportFatal(const char* error) noexcept {
    StopSnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      if (stopping_.load(std::memory_order_acquire) ||
          status() == RemuxPipelineStatus::kFailed ||
          status() == RemuxPipelineStatus::kStopped) {
        return;
      }
      stopping_.store(true, std::memory_order_release);
      status_.store(RemuxPipelineStatus::kFailed, std::memory_order_release);
      snapshot = SnapshotLocked();
      NotifyStatus(RemuxPipelineStatus::kFailed);
    }

    Log::Error("RemuxPipeline失败: {}", error);
    if (snapshot.output_worker) {
      snapshot.output_worker->RequestStop();
    }
    if (snapshot.player) {
      snapshot.player->Stop();
    }
  }

  StopSnapshot SnapshotLocked() const noexcept {
    return {poller_.get(), player_.get(), output_worker_.get()};
  }

  void NotifyStatus(RemuxPipelineStatus status) noexcept {
    if (!on_status_) {
      return;
    }
    try {
      on_status_(status);
    } catch (const std::exception& error) {
      Log::Error("RemuxPipeline状态回调异常，已隔离: {}", error.what());
    } catch (...) {
      Log::Error("RemuxPipeline状态回调异常，已隔离: 未知异常");
    }
  }

  RemuxPipelineConfig config_;
  OnStatus on_status_;
  std::atomic<RemuxPipelineStatus> status_{RemuxPipelineStatus::kIdle};
  std::atomic_bool stopping_{false};
  std::mutex control_mutex_;
  performance::internal::RemuxCollector performance_;
  std::shared_ptr<toolkit::EventPoller> poller_;
  std::shared_ptr<input::PlayerProxy> player_;
  std::unique_ptr<SourceOutputWorker> output_worker_;
};

RemuxPipeline::RemuxPipeline(RemuxPipelineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

RemuxPipeline::~RemuxPipeline() {
  if (impl_) {
    impl_->Stop();
  }
}

void RemuxPipeline::SetOnStatus(OnStatus callback) {
  impl_->SetOnStatus(std::move(callback));
}

void RemuxPipeline::Start() { impl_->Start(); }

void RemuxPipeline::Stop() noexcept { impl_->Stop(); }

RemuxPipelineStatus RemuxPipeline::status() const noexcept {
  return impl_->status();
}

performance::RemuxPipelineSnapshot RemuxPipeline::CollectPerformance() {
  return impl_->CollectPerformance();
}

}  // namespace mw::streamer::pipeline
