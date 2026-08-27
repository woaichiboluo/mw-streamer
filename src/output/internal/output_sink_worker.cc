#include "mw/output/internal/output_sink_worker.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include "mw/common/thread.h"
#include "mw/log/logging.h"

namespace mw::streamer::output::internal {
namespace {

using StreamerLog = log::Module<log::LogModule::kStreamer>;

}  // namespace

OutputSinkWorker::OutputSinkWorker(std::size_t queue_capacity,
                                   std::unique_ptr<OutputSink> sink,
                                   Callbacks callbacks)
    : queue_capacity_(queue_capacity),
      sink_(std::move(sink)),
      callbacks_(std::move(callbacks)) {
  if (queue_capacity_ == 0) {
    throw std::invalid_argument("OutputSinkWorker队列容量必须大于零");
  }
  if (!sink_) {
    throw std::invalid_argument("OutputSinkWorker的Sink不能为空");
  }
}

OutputSinkWorker::~OutputSinkWorker() { Abort(); }

void OutputSinkWorker::Start() {
  State expected = State::kCreated;
  if (!state_.compare_exchange_strong(expected, State::kRunning,
                                      std::memory_order_acq_rel)) {
    throw std::logic_error("OutputSinkWorker只能启动一次");
  }
  try {
    thread_ =
        std::make_unique<common::Thread>("mw-output-sink", [this]() { Run(); });
  } catch (...) {
    state_.store(State::kCreated, std::memory_order_release);
    throw;
  }
}

bool OutputSinkWorker::WriteAudio(const ffmpeg::Frame& frame) {
  return Write(AVMEDIA_TYPE_AUDIO, frame);
}

bool OutputSinkWorker::WriteVideo(const ffmpeg::Frame& frame) {
  return Write(AVMEDIA_TYPE_VIDEO, frame);
}

void OutputSinkWorker::RequestFinish() noexcept {
  std::lock_guard<std::mutex> lock(operation_mutex_);
  State expected = State::kRunning;
  if (state_.compare_exchange_strong(expected, State::kFinishing,
                                     std::memory_order_acq_rel)) {
    queue_.Close();
  }
}

void OutputSinkWorker::RequestAbort() noexcept {
  std::lock_guard<std::mutex> lock(operation_mutex_);
  const State state = state_.load(std::memory_order_acquire);
  if (state == State::kStopped || state == State::kAborting) {
    return;
  }
  state_.store(State::kAborting, std::memory_order_release);
  dropped_frames_.fetch_add(queue_.Clear(), std::memory_order_relaxed);
  queue_.Close();
}

void OutputSinkWorker::Stop() noexcept {
  RequestFinish();
  Join();
}

void OutputSinkWorker::Abort() noexcept {
  RequestAbort();
  Join();
}

std::size_t OutputSinkWorker::queue_depth() const { return queue_.size(); }

std::uint64_t OutputSinkWorker::dropped_frames() const noexcept {
  return dropped_frames_.load(std::memory_order_relaxed);
}

bool OutputSinkWorker::Write(AVMediaType media_type,
                             const ffmpeg::Frame& frame) {
  std::lock_guard<std::mutex> lock(operation_mutex_);
  if (state_.load(std::memory_order_acquire) != State::kRunning) {
    return false;
  }

  WorkItem work{media_type, frame.Ref()};
  if (queue_.TryPush(std::move(work), queue_capacity_)) {
    return true;
  }
  if (queue_.closed()) {
    return false;
  }

  dropped_frames_.fetch_add(queue_.Clear(), std::memory_order_relaxed);
  return queue_.Push({media_type, frame.Ref()});
}

void OutputSinkWorker::Run() noexcept {
  bool failed = false;
  try {
    sink_->Start();
    if (state_.load(std::memory_order_acquire) == State::kAborting) {
      StopSink();
      state_.store(State::kStopped, std::memory_order_release);
      return;
    }
    NotifyReady();
    while (auto work = queue_.WaitPop()) {
      Deliver(std::move(*work));
    }
  } catch (const std::exception& error) {
    failed = true;
    StreamerLog::Error("OutputSink执行异常: {}", error.what());
    RequestAbort();
    ReportFailure(error.what());
  } catch (...) {
    failed = true;
    StreamerLog::Error("OutputSink执行发生未知异常");
    RequestAbort();
    ReportFailure("未知异常");
  }

  StopSink();
  bool completed = false;
  {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    completed =
        !failed && state_.load(std::memory_order_acquire) == State::kFinishing;
    state_.store(State::kStopped, std::memory_order_release);
  }
  if (completed) {
    NotifyCompleted();
  }
}

void OutputSinkWorker::Deliver(WorkItem work) {
  if (work.media_type == AVMEDIA_TYPE_VIDEO) {
    sink_->WriteVideo(std::move(work.frame));
    return;
  }
  if (work.media_type == AVMEDIA_TYPE_AUDIO) {
    sink_->WriteAudio(std::move(work.frame));
    return;
  }
  throw std::logic_error("OutputSinkWorker收到未知媒体帧");
}

void OutputSinkWorker::StopSink() noexcept {
  if (!sink_stopped_.exchange(true, std::memory_order_acq_rel)) {
    sink_->Stop();
  }
}

void OutputSinkWorker::NotifyReady() noexcept {
  if (!callbacks_.on_ready) {
    return;
  }
  try {
    callbacks_.on_ready();
  } catch (const std::exception& error) {
    StreamerLog::Error("OutputSinkWorker就绪回调异常，已隔离: {}",
                       error.what());
  } catch (...) {
    StreamerLog::Error("OutputSinkWorker就绪回调发生未知异常，已隔离");
  }
}

void OutputSinkWorker::NotifyCompleted() noexcept {
  if (!callbacks_.on_completed) {
    return;
  }
  try {
    callbacks_.on_completed();
  } catch (const std::exception& error) {
    StreamerLog::Error("OutputSinkWorker完成回调异常，已隔离: {}",
                       error.what());
  } catch (...) {
    StreamerLog::Error("OutputSinkWorker完成回调发生未知异常，已隔离");
  }
}

void OutputSinkWorker::ReportFailure(const char* error) noexcept {
  if (!callbacks_.on_failed) {
    return;
  }
  try {
    callbacks_.on_failed(error);
  } catch (const std::exception& callback_error) {
    StreamerLog::Error("OutputSinkWorker失败回调异常，已隔离: {}",
                       callback_error.what());
  } catch (...) {
    StreamerLog::Error("OutputSinkWorker失败回调发生未知异常，已隔离");
  }
}

void OutputSinkWorker::Join() noexcept {
  if (thread_ && thread_->IsCurrent()) {
    return;
  }
  std::lock_guard<std::mutex> lock(join_mutex_);
  if (thread_) {
    thread_->Join();
  }
}

}  // namespace mw::streamer::output::internal
