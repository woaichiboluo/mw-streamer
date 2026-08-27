#include "mw/pipeline/internal/streaming/output_event_mailbox.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mw/common/blocking_queue.h"
#include "mw/common/thread.h"
#include "mw/log/logging.h"
#include "mw/processor/streaming_processor_handler.h"

namespace mw::streamer::pipeline::internal::streaming {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

struct OwnedOutputEvent {
  std::string sink_id;
  std::string type;
  std::vector<std::uint8_t> payload;
  std::uint8_t has_timestamp = 0;
  MwStreamerMediaTimestamp timestamp{};
};

OwnedOutputEvent CopyEvent(const MwStreamerOutputEvent& event) {
  OwnedOutputEvent copy{
      event.sink_id, event.type, {}, event.has_timestamp, event.timestamp,
  };
  if (event.payload_size != 0) {
    copy.payload.resize(event.payload_size);
    std::memcpy(copy.payload.data(), event.payload, event.payload_size);
  }
  return copy;
}

}  // namespace

class OutputEventMailbox::Impl final {
 public:
  Impl(std::size_t queue_capacity,
       processor::StreamingProcessorHandler& processor)
      : queue_capacity_(queue_capacity), processor_(processor) {
    if (queue_capacity_ == 0) {
      throw std::invalid_argument("Processor输出事件队列容量必须大于零");
    }
  }

  ~Impl() { Stop(); }

  void Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
      throw std::logic_error("Processor输出事件Mailbox只能启动一次");
    }
    thread_ = std::make_unique<common::Thread>("mw-output-event",
                                               [this]() { Run(); });
  }

  OutputEventSubmitResult Submit(const MwStreamerOutputEvent& event) {
    if (!event.sink_id || !event.type ||
        (event.payload_size != 0 && !event.payload)) {
      throw std::invalid_argument("Processor输出事件字段无效");
    }
    if (!started_.load(std::memory_order_acquire) ||
        stopping_.load(std::memory_order_acquire)) {
      return OutputEventSubmitResult::kStopped;
    }
    if (!queue_.TryPush(CopyEvent(event), queue_capacity_)) {
      return stopping_.load(std::memory_order_acquire) || queue_.closed()
                 ? OutputEventSubmitResult::kStopped
                 : OutputEventSubmitResult::kQueueFull;
    }
    return OutputEventSubmitResult::kAccepted;
  }

  void RequestStop() noexcept {
    stopping_.store(true, std::memory_order_release);
    queue_.Clear();
    queue_.Close();
  }

  void Stop() noexcept {
    RequestStop();
    if (thread_ && !thread_->IsCurrent()) {
      thread_->Join();
    }
  }

  std::size_t queue_depth() const { return queue_.size(); }

 private:
  void Run() noexcept {
    while (auto event = queue_.WaitPop()) {
      const MwStreamerOutputEvent view = {
          event->sink_id.c_str(),
          event->type.c_str(),
          event->payload.empty() ? nullptr : event->payload.data(),
          event->payload.size(),
          event->has_timestamp,
          event->timestamp,
      };
      try {
        processor_.NotifyOutputEvent(view);
      } catch (const std::exception& error) {
        Log::Error("Processor输出事件回调异常，已隔离：{}", error.what());
      } catch (...) {
        Log::Error("Processor输出事件回调异常，已隔离：未知异常");
      }
    }
  }

  const std::size_t queue_capacity_;
  processor::StreamingProcessorHandler& processor_;
  common::BlockingQueue<OwnedOutputEvent> queue_;
  std::unique_ptr<common::Thread> thread_;
  std::atomic<bool> started_ = false;
  std::atomic<bool> stopping_ = false;
};

OutputEventMailbox::OutputEventMailbox(
    std::size_t queue_capacity, processor::StreamingProcessorHandler& processor)
    : impl_(std::make_unique<Impl>(queue_capacity, processor)) {}

OutputEventMailbox::~OutputEventMailbox() = default;

void OutputEventMailbox::Start() { impl_->Start(); }

OutputEventSubmitResult OutputEventMailbox::Submit(
    const MwStreamerOutputEvent& event) {
  return impl_->Submit(event);
}

void OutputEventMailbox::RequestStop() noexcept { impl_->RequestStop(); }

void OutputEventMailbox::Stop() noexcept { impl_->Stop(); }

std::size_t OutputEventMailbox::queue_depth() const {
  return impl_->queue_depth();
}

}  // namespace mw::streamer::pipeline::internal::streaming
