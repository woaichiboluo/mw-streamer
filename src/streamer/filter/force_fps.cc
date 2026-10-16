#include "mw/streamer/filter/force_fps.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "mw/log.h"
#include "mw/streamer/performance/latency.h"

namespace mw::streamer {
namespace {

std::int64_t Rescale(std::int64_t pts, AVRational from, AVRational to) {
  const auto result = av_rescale_q(pts, from, to);
  if (result == AV_NOPTS_VALUE ||
      result == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("ForceFps 时间戳超出可表示范围");
  }
  return result;
}

bool ValidTimeBase(AVRational time_base) {
  return time_base.num > 0 && time_base.den > 0;
}

}  // namespace

class ForceFps::Impl final {
 public:
  explicit Impl(ForceFpsConfig config) : capacity_(config.input_capacity) {
    if (!ValidTimeBase(config.fps) || capacity_ == 0) {
      throw std::invalid_argument("ForceFps 帧率和输入容量必须为正");
    }
    av_reduce(&time_base_.num, &time_base_.den, config.fps.den, config.fps.num,
              std::numeric_limits<int>::max());
    MW_LOG_INFO(
        "streamer",
        "ForceFps 配置: instance={} fps={}/{} time_base={}/{} capacity={}",
        fmt::ptr(this), config.fps.num, config.fps.den, time_base_.num,
        time_base_.den, capacity_);
    latency_.Start("perf.filter.force_fps", this, "filter.force_fps");
  }

  bool TryPushFrame(const AvDecoderReadResult& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      MW_LOG_DEBUG("streamer", "ForceFps 停止后拒绝入队: instance={} stream={}",
                   fmt::ptr(this), input.stream_index);
      return false;
    }
    if (inputs_.size() >= capacity_) {
      MW_LOG_TRACE("streamer",
                   "ForceFps 队列满拒绝提交: instance={} queue={}/{}",
                   fmt::ptr(this), inputs_.size(), capacity_);
      return false;
    }
    if (input.status == AvDecoderReadStatus::kFrame) {
      if (!input.frame || !input.frame->get() ||
          input.frame->get()->nb_samples != 0 ||
          input.frame->get()->width <= 0 || input.frame->get()->height <= 0 ||
          input.frame->get()->pts == AV_NOPTS_VALUE ||
          !ValidTimeBase(input.time_base)) {
        throw std::invalid_argument("ForceFps 需要有效的视频帧和时间戳");
      }
      // Validate before accepting so a bad timestamp cannot poison the queue.
      Rescale(input.frame->get()->pts, input.time_base, time_base_);
    } else if (input.status == AvDecoderReadStatus::kEof) {
      if (input.frame || (input.end_pts != AV_NOPTS_VALUE &&
                          !ValidTimeBase(input.time_base))) {
        throw std::invalid_argument("ForceFps EOF 参数无效");
      }
      if (input.end_pts != AV_NOPTS_VALUE) {
        Rescale(input.end_pts, input.time_base, time_base_);
      }
    } else {
      throw std::invalid_argument("ForceFps 不接受空读取结果");
    }
    inputs_.push_back(input);
    const bool eof = input.status == AvDecoderReadStatus::kEof;
    MW_LOG_TRACE("streamer",
                 "ForceFps 接受入队: instance={} eof={} stream={} "
                 "pts={} time_base={}/{} queue={}/{}",
                 fmt::ptr(this), eof, input.stream_index,
                 eof ? input.end_pts : input.frame->get()->pts,
                 input.time_base.num, input.time_base.den, inputs_.size(),
                 capacity_);
    return true;
  }

  AvDecoderReadResult TryReadFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return {};
    // Measure this read's processing only, excluding queue and lock waits.
    const auto started = latency_.Begin();
    while (!inputs_.empty()) {
      const auto& input = inputs_.front();
      if (input.status == AvDecoderReadStatus::kEof) {
        auto output = input;
        if (output.end_pts != AV_NOPTS_VALUE) {
          output.end_pts =
              Rescale(output.end_pts, output.time_base, time_base_);
        }
        output.time_base = time_base_;
        MW_LOG_TRACE("streamer",
                     "ForceFps 输出 EOF: instance={} stream={} "
                     "input_end_pts={} input_time_base={}/{} end_pts={} "
                     "time_base={}/{}",
                     fmt::ptr(this), output.stream_index, input.end_pts,
                     input.time_base.num, input.time_base.den, output.end_pts,
                     time_base_.num, time_base_.den);
        PopInput();
        ClearHistory();
        return output;
      }

      const auto pts =
          Rescale(input.frame->get()->pts, input.time_base, time_base_);
      const bool discontinuity = IsDiscontinuity(pts);
      if (discontinuity) {
        MW_LOG_DEBUG(
            "streamer",
            "ForceFps PTS 跳变重新锚定: instance={} stream={} "
            "previous_input_pts={} next_pts={} current_pts={} time_base={}/{} "
            "input_pts={} input_time_base={}/{}",
            fmt::ptr(this), input.stream_index, *last_input_pts_,
            next_pts_.value_or(AV_NOPTS_VALUE), pts, time_base_.num,
            time_base_.den, input.frame->get()->pts, input.time_base.num,
            input.time_base.den);
      }
      if (next_pts_ && !discontinuity) {
        if (pts < *next_pts_) {
          MW_LOG_TRACE("streamer",
                       "ForceFps 跳过输入输出槽并缓存最新图像: instance={} "
                       "stream={} reason=pts<next current_pts={} next_pts={} "
                       "time_base={}/{} input_pts={} input_time_base={}/{}",
                       fmt::ptr(this), input.stream_index, pts, *next_pts_,
                       time_base_.num, time_base_.den, input.frame->get()->pts,
                       input.time_base.num, input.time_base.den);
          // Keep the latest picture even when it has no output slot yet.
          Remember(input, false);
          PopInput();
          continue;
        }
        if (pts > *next_pts_) {
          MW_LOG_TRACE("streamer",
                       "ForceFps 使用缓存图像补齐输出槽: instance={} "
                       "stream={} current_pts={} next_pts={} time_base={}/{} "
                       "duplicate={}",
                       fmt::ptr(this), input.stream_index, pts, *next_pts_,
                       time_base_.num, time_base_.den, last_used_);
          auto output = Output(*last_, *next_pts_, last_used_);
          last_used_ = true;
          ++*next_pts_;
          latency_.End(started);
          return output;
        }
      }
      auto output = Output(input, pts, false);
      Remember(input, true);
      last_input_pts_ = pts;
      next_pts_ = pts + 1;
      PopInput();
      latency_.End(started);
      return output;
    }
    return {};
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    latency_.Finish();
    stopped_ = true;
    MW_LOG_INFO("streamer", "ForceFps 停止: instance={} discarded_queue={}",
                fmt::ptr(this), inputs_.size());
    inputs_.clear();
    ClearHistory();
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    MW_LOG_INFO("streamer",
                "ForceFps 重置并清空历史: instance={} discarded_queue={}",
                fmt::ptr(this), inputs_.size());
    inputs_.clear();
    ClearHistory();
    latency_.Start("perf.filter.force_fps", this, "filter.force_fps");
    stopped_ = false;
  }

  AVRational time_base() const { return time_base_; }

 private:
  void PopInput() {
    const auto& input = inputs_.front();
    const bool eof = input.status == AvDecoderReadStatus::kEof;
    MW_LOG_TRACE("streamer",
                 "ForceFps 消费出队: instance={} eof={} stream={} "
                 "pts={} time_base={}/{} remaining_queue={}/{}",
                 fmt::ptr(this), eof, input.stream_index,
                 eof ? input.end_pts : input.frame->get()->pts,
                 input.time_base.num, input.time_base.den, inputs_.size() - 1,
                 capacity_);
    inputs_.pop_front();
  }

  bool IsDiscontinuity(std::int64_t pts) const {
    if (!last_input_pts_) return false;
    if (pts < *last_input_pts_) return true;
    // Unsigned subtraction avoids signed overflow for extreme forward jumps.
    const auto delta = static_cast<std::uint64_t>(pts) -
                       static_cast<std::uint64_t>(*last_input_pts_);
    if (delta >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return true;
    }
    return delta > 1 && av_compare_ts(static_cast<std::int64_t>(delta) - 1,
                                      time_base_, 1, AVRational{1, 2}) > 0;
  }

  AvDecoderReadResult Output(const AvDecoderReadResult& input, std::int64_t pts,
                             bool duplicate) const {
    MW_LOG_TRACE("streamer",
                 "ForceFps 输出帧: instance={} stream={} "
                 "duplicate={} input_pts={} input_time_base={}/{} pts={} "
                 "time_base={}/{}",
                 fmt::ptr(this), input.stream_index, duplicate,
                 input.frame->get()->pts, input.time_base.num,
                 input.time_base.den, pts, time_base_.num, time_base_.den);
    auto output = input;
    auto* frame = output.frame->get();
    frame->pts = pts;
    frame->best_effort_timestamp = pts;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->time_base = time_base_;
    frame->duration = 1;
    if (duplicate) {
      // Closed captions belong to the source frame, not every repeated copy.
      av_frame_remove_side_data(frame, AV_FRAME_DATA_A53_CC);
    }
    output.time_base = time_base_;
    return output;
  }

  void Remember(const AvDecoderReadResult& input, bool used) {
    if (last_ && !last_used_) {
      MW_LOG_TRACE("streamer",
                   "ForceFps 丢弃未使用缓存帧: instance={} "
                   "reason=replaced_before_output "
                   "stream={} pts={} time_base={}/{} "
                   "replacement_pts={} "
                   "replacement_time_base={}/{}",
                   fmt::ptr(this), last_->stream_index,
                   last_->frame->get()->pts, last_->time_base.num,
                   last_->time_base.den, input.frame->get()->pts,
                   input.time_base.num, input.time_base.den);
    }
    last_ = input;
    last_used_ = used;
  }

  void ClearHistory() {
    if (last_ && !last_used_) {
      MW_LOG_TRACE(
          "streamer",
          "ForceFps 丢弃未使用缓存帧: instance={} reason=history_reset "
          "stream={} pts={} time_base={}/{}",
          fmt::ptr(this), last_->stream_index, last_->frame->get()->pts,
          last_->time_base.num, last_->time_base.den);
    }
    last_.reset();
    last_used_ = false;
    last_input_pts_.reset();
    next_pts_.reset();
  }

  const std::size_t capacity_;
  AVRational time_base_{0, 1};
  std::mutex mutex_;
  internal::StageLatency latency_;
  bool stopped_ = false;
  std::deque<AvDecoderReadResult> inputs_;
  std::optional<AvDecoderReadResult> last_;
  bool last_used_ = false;
  std::optional<std::int64_t> last_input_pts_;
  std::optional<std::int64_t> next_pts_;
};

ForceFps::ForceFps(ForceFpsConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
ForceFps::~ForceFps() = default;
bool ForceFps::TryPushFrame(const AvDecoderReadResult& input) {
  return impl_->TryPushFrame(input);
}
AvDecoderReadResult ForceFps::TryReadFrame() { return impl_->TryReadFrame(); }
void ForceFps::Stop() { impl_->Stop(); }
void ForceFps::Reset() { impl_->Reset(); }
AVRational ForceFps::time_base() const { return impl_->time_base(); }

}  // namespace mw::streamer
