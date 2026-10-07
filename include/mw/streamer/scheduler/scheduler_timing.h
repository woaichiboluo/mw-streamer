#ifndef MW_STREAMER_SCHEDULER_SCHEDULER_TIMING_H_
#define MW_STREAMER_SCHEDULER_SCHEDULER_TIMING_H_

#include <algorithm>
#include <cstdint>
#include <deque>

namespace mw::streamer::internal {

// Positive durations and a positive rate use truncating conversions.
inline std::int64_t AudioSamplesToNs(std::int64_t samples, int rate) noexcept {
  return (samples / rate) * 1000000000 + ((samples % rate) * 1000000000) / rate;
}

inline std::int64_t AudioNsToSamples(std::int64_t ns, int rate) noexcept {
  return (ns / 1000000000) * rate + ((ns % 1000000000) * rate) / 1000000000;
}

// Map source PTS to absolute system nanoseconds while preserving continuity.
// Source PTS and system time remain separate domains.
class SourceAudioTiming final {
 public:
  struct Mapping {
    std::int64_t timestamp_ns;
    bool append;
    bool reset_buffer;
  };

  void Reset() noexcept {
    timing_set_ = false;
    timing_adjust_ = 0;
    next_source_pts_ = 0;
    next_system_pts_ = 0;
  }

  void SetVideoTiming(std::int64_t source_pts,
                      std::int64_t system_ns) noexcept {
    timing_set_ = true;
    timing_adjust_ = system_ns - source_pts;
  }

  // Clear the mapping without clearing either timestamp prediction.
  void ClearTiming() noexcept { timing_set_ = false; }

  // QueueAudio uses the new FIFO origin after Map when replacing its buffer.
  // A source-domain jump applies that reset inside Map before updating the
  // final timestamp prediction.
  void ResetBufferTimestamp(std::int64_t timestamp) noexcept {
    next_system_pts_ = timestamp;
  }

  Mapping Map(std::int64_t source_pts, int samples, int rate,
              std::int64_t system_ns,
              std::int64_t resample_offset = 0) noexcept {
    auto timestamp = source_pts;
    bool append = false;
    bool reset_buffer = false;
    const bool direct = Distance(timestamp, system_ns) < kTimestampJump;
    if (direct) {
      timing_adjust_ = 0;
      timing_set_ = true;
    }

    if (!timing_set_) {
      SetVideoTiming(timestamp, system_ns);
    } else if (next_source_pts_ != 0) {
      const auto difference = Distance(next_source_pts_, timestamp);
      if (difference > kTimestampJump && !direct) {
        // Rebuild the mapping and request a PCM reset. Use the new system
        // base for the continuity check below.
        SetVideoTiming(timestamp, system_ns);
        next_system_pts_ = system_ns;
        reset_buffer = true;
      } else if (difference < kSmoothing) {
        timestamp = next_source_pts_;
      }
    }

    next_source_pts_ = timestamp + AudioSamplesToNs(samples, rate);
    timestamp += timing_adjust_;
    if (next_system_pts_ == timestamp) {
      append = true;
    } else if (next_system_pts_ != 0) {
      const auto difference = Distance(next_system_pts_, timestamp);
      if (difference < kSmoothing) {
        append = true;
      } else if (difference > kTimestampJump) {
        // A system-domain jump rebuilds only the mapping. QueueAudio decides
        // whether to reset PCM based on its current FIFO origin.
        SetVideoTiming(source_pts, system_ns);
        timestamp = source_pts + timing_adjust_;
      }
    }

    // Apply resample_offset after both continuity checks.
    next_system_pts_ = next_source_pts_ + timing_adjust_;
    return {timestamp - resample_offset, append, reset_buffer};
  }

  bool timing_set() const noexcept { return timing_set_; }
  std::int64_t timing_adjust() const noexcept { return timing_adjust_; }
  std::int64_t next_source_pts() const noexcept { return next_source_pts_; }
  std::int64_t next_system_pts() const noexcept { return next_system_pts_; }

 private:
  static std::uint64_t Distance(std::int64_t left,
                                std::int64_t right) noexcept {
    return left < right ? static_cast<std::uint64_t>(right) -
                              static_cast<std::uint64_t>(left)
                        : static_cast<std::uint64_t>(left) -
                              static_cast<std::uint64_t>(right);
  }

  static constexpr std::uint64_t kTimestampJump = 2000000000;
  static constexpr std::uint64_t kSmoothing = 70000000;
  bool timing_set_ = false;
  std::int64_t timing_adjust_ = 0;
  std::int64_t next_source_pts_ = 0;
  std::int64_t next_system_pts_ = 0;
};

// Track output windows and insert historical windows when audio arrives late.
// Buffering growth is limited across the session and waits suppress output.
class AudioOutputTiming final {
 public:
  struct Window {
    std::int64_t start;
    std::int64_t end;
  };

  explicit AudioOutputTiming(int max_buffering_ticks = 45) noexcept
      : max_buffering_ticks_(max_buffering_ticks) {}

  void Reset() noexcept {
    windows_.clear();
    buffered_timestamp_ = 0;
    total_buffering_ticks_ = 0;
    waiting_ticks_ = 0;
  }

  void Push(std::int64_t start, std::int64_t end) {
    windows_.push_back({start, end});
  }

  // Valid after Push and before the corresponding Finish.
  const Window& window() const noexcept { return windows_.front(); }

  void BufferTo(std::int64_t min_timestamp, int rate, int block_samples) {
    if (maxed() || min_timestamp >= window().start) return;
    if (!waiting_ticks_) buffered_timestamp_ = window().start;
    const auto frames = AudioNsToSamples(window().start - min_timestamp, rate);
    const auto requested = (frames + block_samples - 1) / block_samples;
    const auto available = max_buffering_ticks_ - total_buffering_ticks_;
    const int ticks =
        static_cast<int>(std::min<std::int64_t>(requested, available));
    total_buffering_ticks_ += ticks;

    auto start =
        buffered_timestamp_ -
        AudioSamplesToNs(
            static_cast<std::int64_t>(waiting_ticks_) * block_samples, rate);
    for (int i = 0; i < ticks; ++i) {
      const auto end = start;
      ++waiting_ticks_;
      start =
          buffered_timestamp_ -
          AudioSamplesToNs(
              static_cast<std::int64_t>(waiting_ticks_) * block_samples, rate);
      windows_.push_front({start, end});
    }
  }

  bool Finish() noexcept {
    windows_.pop_front();
    if (waiting_ticks_) {
      --waiting_ticks_;
      return false;
    }
    return true;
  }

  bool maxed() const noexcept {
    return total_buffering_ticks_ == max_buffering_ticks_;
  }
  int total_buffering_ticks() const noexcept { return total_buffering_ticks_; }
  int waiting_ticks() const noexcept { return waiting_ticks_; }

 private:
  const int max_buffering_ticks_;
  std::deque<Window> windows_;
  std::int64_t buffered_timestamp_ = 0;
  int total_buffering_ticks_ = 0;
  int waiting_ticks_ = 0;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_SCHEDULER_SCHEDULER_TIMING_H_
