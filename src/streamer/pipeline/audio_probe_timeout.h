#ifndef MW_SRC_STREAMER_PIPELINE_AUDIO_PROBE_TIMEOUT_H_
#define MW_SRC_STREAMER_PIPELINE_AUDIO_PROBE_TIMEOUT_H_

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>

#include "mw/log.h"
#include "mw/streamer/input/av_input_proxy.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer::internal {

// Owns the AAC startup sampling budget. Expiration cancels this failed input
// session; packet consumers still use the ordinary blocking receive contract.
// Finish must precede decoding the sample or starting another input session.
class AudioProbeTimeout final {
 public:
  explicit AudioProbeTimeout(AvInputProxy& input) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!worker_.Start([this, &input, deadline]() noexcept {
          try {
            std::unique_lock<std::mutex> lock(mutex_);
            if (changed_.wait_until(lock, deadline,
                                    [this] { return finished_; }))
              return;
            expired_ = true;
          } catch (const std::exception& error) {
            failure_ = std::current_exception();
            MW_LOG_ERROR("streamer", "AAC 探测计时失败: {}", error.what());
          } catch (...) {
            failure_ = std::current_exception();
            MW_LOG_ERROR("streamer", "AAC 探测计时发生未知异常");
          }
          // Never cancel while holding the timer lock. Finish joins this call
          // so a late expiration cannot affect a subsequent input session.
          input.RequestStop();
        })) {
      throw std::runtime_error("AAC 探测计时线程启动失败");
    }
  }

  ~AudioProbeTimeout() { CancelAndJoin(); }
  AudioProbeTimeout(const AudioProbeTimeout&) = delete;
  AudioProbeTimeout& operator=(const AudioProbeTimeout&) = delete;

  // Serialized by the sampling owner. Expiration wins if already committed;
  // joining also makes expired_/failure_ visible without a second state lock.
  bool Finish() {
    CancelAndJoin();
    if (failure_) std::rethrow_exception(failure_);
    return expired_;
  }

 private:
  void CancelAndJoin() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
    }
    changed_.notify_all();
    worker_.Join();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool finished_ = false;
  bool expired_ = false;
  std::exception_ptr failure_;
  Thread worker_;
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_PIPELINE_AUDIO_PROBE_TIMEOUT_H_
