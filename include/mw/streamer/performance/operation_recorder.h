#ifndef MW_STREAMER_PERFORMANCE_OPERATION_RECORDER_H_
#define MW_STREAMER_PERFORMANCE_OPERATION_RECORDER_H_

#include <chrono>
#include <cstdint>
#include <mutex>

#include "mw/streamer/performance/pipeline_snapshot.h"

struct hdr_histogram;

namespace mw::streamer {

// Shared recording primitive for Input and Sink implementations. Recording and
// snapshots are thread-safe; its mutex is never held across processing work.
// Readers never rotate or reset counters. The owner outlives every active Call.
class OperationRecorder final {
 public:
  OperationRecorder(PerformanceType type, PerformanceUnit input_unit,
                    PerformanceUnit output_unit);
  ~OperationRecorder();
  OperationRecorder(const OperationRecorder&) = delete;
  OperationRecorder& operator=(const OperationRecorder&) = delete;

  void AddInput(std::uint64_t count, std::uint64_t bytes = 0) noexcept;
  void AddOutput(std::uint64_t count, std::uint64_t bytes = 0) noexcept;
  OperationSnapshot GetSnapshot() const;

  // A processing invocation, not a queue submission. Exception unwinding marks
  // the invocation failed. Finish may close the scope before downstream work.
  // Pause/Resume exclude nested synchronous delivery and may be nested. A Call
  // and its optional active slot belong to one execution thread.
  class Call final {
   public:
    explicit Call(OperationRecorder& recorder) noexcept;
    // Borrows a callback-visible slot, restoring its previous value on Finish.
    Call(OperationRecorder& recorder, Call*& active_slot) noexcept;
    ~Call();
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    void Pause() noexcept;
    void Resume() noexcept;
    void Finish() noexcept;

   private:
    OperationRecorder& recorder_;
    std::chrono::steady_clock::time_point started_at_;
    std::chrono::nanoseconds elapsed_{0};
    int exceptions_;
    unsigned int pause_depth_ = 0;
    bool finished_ = false;
    Call** active_slot_ = nullptr;
    Call* previous_call_ = nullptr;
  };

  class Suspension final {
   public:
    explicit Suspension(Call* call) noexcept : call_(call) {
      if (call_) call_->Pause();
    }
    ~Suspension() {
      if (call_) call_->Resume();
    }
    Suspension(const Suspension&) = delete;
    Suspension& operator=(const Suspension&) = delete;

   private:
    Call* call_;
  };

 private:
  void BeginCall() noexcept;
  void EndCall(std::chrono::nanoseconds elapsed, bool failed) noexcept;

  mutable std::mutex mutex_;
  OperationSnapshot snapshot_;
  hdr_histogram* histogram_ = nullptr;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PERFORMANCE_OPERATION_RECORDER_H_
