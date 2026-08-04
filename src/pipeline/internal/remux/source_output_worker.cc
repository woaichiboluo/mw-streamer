#include "mw/pipeline/internal/remux/source_output_worker.h"

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
}

#include <fmt/format.h>

#include "Poller/EventPoller.h"
#include "mw/log/logging.h"
#include "mw/output/output_session.h"

namespace mw::streamer::pipeline::internal::remux {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

std::shared_ptr<toolkit::EventPoller> SelectPoller(
    const std::shared_ptr<toolkit::EventPoller>& player_poller) {
  std::shared_ptr<toolkit::EventPoller> selected;
  auto& pool = toolkit::EventPollerPool::Instance();
  pool.for_each(
      [&selected, &player_poller](const toolkit::TaskExecutor::Ptr& executor) {
        const auto candidate =
            std::dynamic_pointer_cast<toolkit::EventPoller>(executor);
        if (!selected && candidate && candidate != player_poller) {
          selected = candidate;
        }
      });
  return selected ? std::move(selected) : pool.getPoller(false);
}

}  // namespace

SourceOutputWorker::SourceOutputWorker(
    std::vector<std::string> targets, zlm::OutputConfig zlm_config,
    std::size_t queue_capacity,
    const std::shared_ptr<toolkit::EventPoller>& player_poller,
    OnFailed on_failed)
    : targets_(std::move(targets)),
      zlm_config_(std::move(zlm_config)),
      queue_capacity_(queue_capacity),
      poller_(SelectPoller(player_poller)),
      on_failed_(std::move(on_failed)) {
  if (targets_.empty()) {
    throw std::invalid_argument("SourceOutputWorker至少需要一个输出目标");
  }
  if (queue_capacity_ == 0) {
    throw std::invalid_argument("SourceOutputWorker队列容量必须大于0");
  }
}

SourceOutputWorker::~SourceOutputWorker() { Stop(); }

void SourceOutputWorker::Open(
    const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
  SourceOutputWorkerState expected = SourceOutputWorkerState::kWaitingStreams;
  if (!state_.compare_exchange_strong(expected,
                                      SourceOutputWorkerState::kOpening,
                                      std::memory_order_acq_rel)) {
    return;
  }

  try {
    std::exception_ptr open_error;
    poller_->sync([this, streams, &open_error]() {
      try {
        if (state() != SourceOutputWorkerState::kOpening) {
          return;
        }
        output::OutputConfig config;
        config.streams = streams;
        config.targets = targets_;
        config.zlm = zlm_config_;
        auto output =
            std::make_shared<output::OutputSession>(std::move(config), poller_);
        output->SetOnAllTargetsUnavailable(
            [this]() { Fail("所有输入旁路输出目标均已永久失效"); });
        output->Open();
        output_ = output.get();
        std::atomic_store_explicit(&published_output_, std::move(output),
                                   std::memory_order_release);
      } catch (...) {
        open_error = std::current_exception();
      }
    });
    if (open_error) {
      std::rethrow_exception(open_error);
    }
    expected = SourceOutputWorkerState::kOpening;
    if (!state_.compare_exchange_strong(expected,
                                        SourceOutputWorkerState::kRunning,
                                        std::memory_order_acq_rel)) {
      poller_->async([this]() { CloseOutputOnPoller(); }, false);
    }
  } catch (const std::exception& error) {
    Fail(fmt::format("打开输入旁路OutputSession失败：{}", error.what()));
  } catch (...) {
    Fail("打开输入旁路OutputSession失败：未知异常");
  }
}

bool SourceOutputWorker::Write(std::uint64_t generation,
                               const ffmpeg::Packet& packet) noexcept {
  if (state() != SourceOutputWorkerState::kRunning) {
    return false;
  }
  const auto* raw_packet = packet.get();
  if (!raw_packet) {
    Fail(fmt::format("输入旁路收到空AVPacket：generation={}", generation));
    return false;
  }
  if (raw_packet->dts == AV_NOPTS_VALUE || raw_packet->pts == AV_NOPTS_VALUE) {
    Fail(fmt::format(
        "输入旁路AVPacket缺少时间戳：generation={}, stream={}, pts={}, dts={}",
        generation, raw_packet->stream_index, raw_packet->pts,
        raw_packet->dts));
    return false;
  }

  bool schedule_drain = false;
  std::string failure;
  try {
    auto retained_packet = packet.Ref();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state() != SourceOutputWorkerState::kRunning) {
        return false;
      }
      const auto previous = last_dts_by_stream_.find(raw_packet->stream_index);
      if (previous != last_dts_by_stream_.end() &&
          raw_packet->dts < previous->second) {
        failure = fmt::format(
            "输入旁路DTS回退：generation={}, stream={}, previous_dts={}, "
            "current_dts={}, pts={}",
            generation, raw_packet->stream_index, previous->second,
            raw_packet->dts, raw_packet->pts);
      } else if (queue_.size() >= queue_capacity_) {
        failure = fmt::format("输入旁路队列已满：generation={}, capacity={}",
                              generation, queue_capacity_);
      } else {
        last_dts_by_stream_[raw_packet->stream_index] = raw_packet->dts;
        queue_.push_back({std::move(retained_packet)});
        schedule_drain = !std::exchange(drain_scheduled_, true);
      }
    }
  } catch (const std::exception& error) {
    failure = fmt::format("引用输入旁路AVPacket失败：{}", error.what());
  } catch (...) {
    failure = "引用输入旁路AVPacket失败：未知异常";
  }

  if (!failure.empty()) {
    Fail(std::move(failure));
    return false;
  }
  if (schedule_drain) {
    ScheduleDrain();
  }
  return true;
}

void SourceOutputWorker::ScheduleDrain() noexcept {
  try {
    poller_->async([this]() { DrainOnPoller(); }, false);
  } catch (const std::exception& error) {
    Fail(fmt::format("调度输入旁路写入失败：{}", error.what()));
  } catch (...) {
    Fail("调度输入旁路写入失败：未知异常");
  }
}

void SourceOutputWorker::DrainOnPoller() noexcept {
  for (;;) {
    std::optional<WorkItem> work;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto current_state = state();
      if (current_state == SourceOutputWorkerState::kFailed ||
          current_state == SourceOutputWorkerState::kStopped) {
        queue_.clear();
        drain_scheduled_ = false;
        return;
      }
      if (queue_.empty()) {
        drain_scheduled_ = false;
        return;
      }
      work.emplace(std::move(queue_.front()));
      queue_.pop_front();
    }
    if (output_) {
      output_->Write(work->packet);
    }
  }
}

void SourceOutputWorker::RequestStop() noexcept {
  auto current = state();
  while (current != SourceOutputWorkerState::kFailed &&
         current != SourceOutputWorkerState::kStopped &&
         current != SourceOutputWorkerState::kStopping &&
         !state_.compare_exchange_weak(current,
                                       SourceOutputWorkerState::kStopping,
                                       std::memory_order_acq_rel)) {
  }
}

void SourceOutputWorker::Stop() noexcept {
  RequestStop();
  try {
    const auto finalize = [this]() {
      DrainOnPoller();
      CloseOutputOnPoller();
      if (state() != SourceOutputWorkerState::kFailed) {
        state_.store(SourceOutputWorkerState::kStopped,
                     std::memory_order_release);
      }
    };
    if (poller_->isCurrentThread()) {
      finalize();
    } else {
      poller_->sync(finalize);
    }
  } catch (const std::exception& error) {
    Log::Error("停止输入旁路Worker失败，已隔离：{}", error.what());
  } catch (...) {
    Log::Error("停止输入旁路Worker失败，已隔离：未知异常");
  }
}

SourceOutputWorkerState SourceOutputWorker::state() const noexcept {
  return state_.load(std::memory_order_acquire);
}

std::size_t SourceOutputWorker::queue_depth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::shared_ptr<output::OutputSession> SourceOutputWorker::output_session()
    const {
  return std::atomic_load_explicit(&published_output_,
                                   std::memory_order_acquire);
}

void SourceOutputWorker::CloseOutputOnPoller() noexcept {
  output_ = nullptr;
  auto output = std::atomic_exchange_explicit(
      &published_output_, std::shared_ptr<output::OutputSession>{},
      std::memory_order_acq_rel);
  if (output) {
    output->Close();
  }
}

void SourceOutputWorker::Fail(std::string reason) noexcept {
  auto current = state();
  while (current != SourceOutputWorkerState::kFailed &&
         current != SourceOutputWorkerState::kStopping &&
         current != SourceOutputWorkerState::kStopped) {
    if (state_.compare_exchange_weak(current, SourceOutputWorkerState::kFailed,
                                     std::memory_order_acq_rel)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        drain_scheduled_ = false;
      }
      Log::Error("源压缩包输出Worker已停止：{}", reason);
      if (on_failed_) {
        try {
          on_failed_(reason.c_str());
        } catch (const std::exception& error) {
          Log::Error("源压缩包输出Worker失败回调异常，已隔离：{}",
                     error.what());
        } catch (...) {
          Log::Error("源压缩包输出Worker失败回调异常，已隔离：未知异常");
        }
      }
      try {
        poller_->async([this]() { CloseOutputOnPoller(); }, false);
      } catch (const std::exception& error) {
        Log::Error("调度关闭输入旁路OutputSession失败，已隔离：{}",
                   error.what());
      } catch (...) {
        Log::Error("调度关闭输入旁路OutputSession失败，已隔离：未知异常");
      }
      return;
    }
  }
}

}  // namespace mw::streamer::pipeline::internal::remux
