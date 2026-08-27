#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_INTERNAL_OUTPUT_SINK_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_INTERNAL_OUTPUT_SINK_WORKER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

extern "C" {
#include <libavutil/avutil.h>
}

#include "mw/common/blocking_queue.h"
#include "mw/ffmpeg/frame.h"
#include "mw/output/output_sink.h"

namespace mw::streamer::common {
class Thread;
}

namespace mw::streamer::output::internal {

// Serializes one OutputSink on an isolated thread. A full queue discards only
// this worker's pending frames so the producer and other sinks keep moving.
class OutputSinkWorker final {
 public:
  struct Callbacks {
    std::function<void()> on_ready;
    std::function<void()> on_completed;
    std::function<void(const char* error)> on_failed;
  };

  OutputSinkWorker(std::size_t queue_capacity, std::unique_ptr<OutputSink> sink,
                   Callbacks callbacks = {});
  ~OutputSinkWorker();

  OutputSinkWorker(const OutputSinkWorker&) = delete;
  OutputSinkWorker& operator=(const OutputSinkWorker&) = delete;

  void Start();
  bool WriteAudio(const ffmpeg::Frame& frame);
  bool WriteVideo(const ffmpeg::Frame& frame);

  // Stops accepting frames and lets the delivery thread drain asynchronously.
  void RequestFinish() noexcept;
  // Discards queued frames and asks the delivery thread to stop.
  void RequestAbort() noexcept;
  // Drains queued frames and waits for delivery to stop.
  void Stop() noexcept;
  // Discards queued frames and waits for delivery to stop.
  void Abort() noexcept;

  std::size_t queue_depth() const;
  std::uint64_t dropped_frames() const noexcept;

 private:
  enum class State {
    kCreated,
    kRunning,
    kFinishing,
    kAborting,
    kStopped,
  };

  struct WorkItem {
    AVMediaType media_type;
    ffmpeg::Frame frame;
  };

  bool Write(AVMediaType media_type, const ffmpeg::Frame& frame);
  void Run() noexcept;
  void Deliver(WorkItem work);
  void StopSink() noexcept;
  void NotifyReady() noexcept;
  void NotifyCompleted() noexcept;
  void ReportFailure(const char* error) noexcept;
  void Join() noexcept;

  const std::size_t queue_capacity_;
  const std::unique_ptr<OutputSink> sink_;
  const Callbacks callbacks_;
  common::BlockingQueue<WorkItem> queue_;
  std::unique_ptr<common::Thread> thread_;
  std::mutex operation_mutex_;
  std::mutex join_mutex_;
  std::atomic<State> state_{State::kCreated};
  std::atomic<bool> sink_stopped_{false};
  std::atomic<std::uint64_t> dropped_frames_{0};
};

}  // namespace mw::streamer::output::internal

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_INTERNAL_OUTPUT_SINK_WORKER_H_
