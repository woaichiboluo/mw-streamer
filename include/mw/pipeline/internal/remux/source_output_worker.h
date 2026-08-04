#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_REMUX_SOURCE_OUTPUT_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_REMUX_SOURCE_OUTPUT_WORKER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/zlm/config.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::output {
class OutputSession;
}

namespace mw::streamer::pipeline::internal::remux {

enum class SourceOutputWorkerState {
  kWaitingStreams,
  kOpening,
  kRunning,
  kFailed,
  kStopping,
  kStopped,
};

// Writes source packets to one OutputSession through a bounded poller queue.
// A permanent branch failure stops this worker and notifies its owner.
class SourceOutputWorker final {
 public:
  using OnFailed = std::function<void(const char* error)>;

  SourceOutputWorker(std::vector<std::string> targets,
                     zlm::OutputConfig zlm_config, std::size_t queue_capacity,
                     const std::shared_ptr<toolkit::EventPoller>& player_poller,
                     OnFailed on_failed = {});
  ~SourceOutputWorker();

  SourceOutputWorker(const SourceOutputWorker&) = delete;
  SourceOutputWorker& operator=(const SourceOutputWorker&) = delete;

  // The first call opens the lifetime OutputSession. Later generations reuse
  // it and therefore do not create a new recording file or network publisher.
  void Open(const std::vector<ffmpeg::StreamInfo>& streams) noexcept;

  // Returns false when the branch is unavailable, stopped, or overloaded.
  bool Write(std::uint64_t generation, const ffmpeg::Packet& packet) noexcept;

  void RequestStop() noexcept;
  void Stop() noexcept;

  SourceOutputWorkerState state() const noexcept;
  std::size_t queue_depth() const;
  std::shared_ptr<output::OutputSession> output_session() const;

 private:
  struct WorkItem {
    ffmpeg::Packet packet;
  };

  void ScheduleDrain() noexcept;
  void DrainOnPoller() noexcept;
  void CloseOutputOnPoller() noexcept;
  void Fail(std::string reason) noexcept;

  const std::vector<std::string> targets_;
  const zlm::OutputConfig zlm_config_;
  const std::size_t queue_capacity_;
  const std::shared_ptr<toolkit::EventPoller> poller_;
  const OnFailed on_failed_;

  std::atomic<SourceOutputWorkerState> state_{
      SourceOutputWorkerState::kWaitingStreams};
  mutable std::mutex mutex_;
  std::deque<WorkItem> queue_;
  std::unordered_map<int, std::int64_t> last_dts_by_stream_;
  bool drain_scheduled_ = false;
  output::OutputSession* output_ = nullptr;
  std::shared_ptr<output::OutputSession> published_output_;
};

}  // namespace mw::streamer::pipeline::internal::remux

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_REMUX_SOURCE_OUTPUT_WORKER_H_
