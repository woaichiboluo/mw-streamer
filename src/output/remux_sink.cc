#include "mw/output/remux_sink.h"

#include <fmt/format.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"
#include "mw/common/blocking_queue.h"
#include "mw/init/internal/runtime.h"
#include "mw/log/logging.h"
#include "mw/output/internal/remux_output.h"
#include "mw/performance/operation_recorder.h"
#include "mw/sink/fatal_error.h"

namespace mw::streamer::output {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;
constexpr std::size_t kDrainBatchSize = 64;

bool SameStream(const ffmpeg::StreamInfo& left,
                const ffmpeg::StreamInfo& right) {
  const auto& a = *left.codec_parameters.get();
  const auto& b = *right.codec_parameters.get();
  if (left.stream_index != right.stream_index ||
      av_cmp_q(left.time_base, right.time_base) != 0 ||
      a.codec_type != b.codec_type || a.codec_id != b.codec_id ||
      a.format != b.format || a.width != b.width || a.height != b.height ||
      a.sample_rate != b.sample_rate || a.profile != b.profile ||
      a.level != b.level ||
      av_channel_layout_compare(&a.ch_layout, &b.ch_layout) != 0) {
    return false;
  }
  // As in PlayerProxy, video parameter sets can also arrive in-band.
  return a.codec_type != AVMEDIA_TYPE_AUDIO ||
         (a.extradata_size == b.extradata_size &&
          (a.extradata_size == 0 ||
           std::memcmp(a.extradata, b.extradata, a.extradata_size) == 0));
}

}  // namespace

class RemuxSink::Impl final {
 public:
  Impl(RemuxSink& owner, RemuxSinkConfig config)
      : owner_(owner), config_(std::move(config)) {
    output::internal::ValidateRemuxOutputConfig({config_.target, config_.zlm});
    if (config_.packet_queue_capacity == 0) {
      throw std::invalid_argument("RemuxSink包队列容量必须大于零");
    }
    init::internal::EnsureInitialized();
    poller_ = toolkit::EventPollerPool::Instance().getPoller(false);
    network_snapshot_.target = config_.target;
  }

  ~Impl() { Stop(); }

  void OnStreamsReady(const media::StreamsReady& streams) noexcept {
    Submit(streams, &Impl::OpenStreams);
  }

  void OnPacket(const media::PacketReady& packet) noexcept {
    Submit(packet, &Impl::WritePacket, true);
  }

  void OnTimelineReset(const media::TimelineReset& reset) noexcept {
    Submit(reset, &Impl::ResetTimeline);
  }

  void OnInputEnded(const media::StreamEnded& end) noexcept {
    Submit(end, &Impl::EndInput);
  }

  void Stop() noexcept {
    if (poller_->isCurrentThread()) {
      std::terminate();
    }
    std::lock_guard<std::mutex> stop_lock(stop_mutex_);
    if (stopped_) {
      return;
    }
    {
      // All async scheduling uses this mutex. No drain can enqueue another
      // raw-this callback after this point and beyond the following barrier.
      std::lock_guard<std::mutex> lock(submit_mutex_);
      stop_requested_ = true;
      queue_.Close();
    }
    poller_->sync([this]() {
      SetState(sink::PacketSinkState::kDraining);
      while (auto work = queue_.TryPop()) {
        Execute(*work);
      }
      CloseOutput();
      SetState(sink::PacketSinkState::kStopped);
    });
    stopped_ = true;
  }

  sink::PacketSinkState state() const noexcept { return state_.load(); }

  std::string error() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return error_;
  }

  std::size_t queue_depth() const { return queue_.size(); }

  performance::NodeSnapshot GetOwnPerformance() const {
    performance::NodeSnapshot snapshot;
    snapshot.name = fmt::format("RemuxSink ({})", config_.target);
    snapshot.operations.push_back(performance_.GetSnapshot());
    return snapshot;
  }

  performance::NetworkOutputSnapshot GetNetworkOutputSnapshot() const {
    performance::NetworkOutputSnapshot result;
    poller_->sync([this, &result]() {
      result =
          output_ ? output_->GetNetworkOutputSnapshot() : network_snapshot_;
    });
    return result;
  }

 private:
  struct Work {
    std::function<void()> run;
    bool packet;
  };

  template <typename Event>
  void Submit(const Event& event, void (Impl::*action)(const Event&),
              bool packet = false) noexcept {
    try {
      std::lock_guard<std::mutex> lock(submit_mutex_);
      if (stop_requested_ || Terminal()) {
        return;
      }
      if (!queue_.TryPush(
              {[this, event, action]() { (this->*action)(event); }, packet},
              config_.packet_queue_capacity,
              [](const Work& work) { return work.packet; })) {
        if (queue_.closed()) {
          return;
        }
        throw std::runtime_error("RemuxSink包队列已满");
      }
      ScheduleDrain();
    } catch (const std::exception& error) {
      Fail(error.what());
    } catch (...) {
      Fail("投递RemuxSink失败：未知异常");
    }
  }

  // submit_mutex_ is held by every caller; async must never execute inline.
  void ScheduleDrain() {
    if (!drain_scheduled_ && !stop_requested_) {
      poller_->async([this]() { Drain(); }, false);
      drain_scheduled_ = true;
    }
  }

  void Drain() noexcept {
    for (std::size_t count = 0; count < kDrainBatchSize; ++count) {
      auto work = queue_.TryPop();
      if (!work) {
        break;
      }
      Execute(*work);
    }
    bool failed;
    {
      std::lock_guard<std::mutex> lock(submit_mutex_);
      drain_scheduled_ = false;
      failed = state() == sink::PacketSinkState::kFailed;
      if (!failed && queue_.size() != 0) {
        try {
          ScheduleDrain();
        } catch (...) {
          // No queued lambda can be lost silently. Record outside the mutex.
          failed = true;
        }
      }
    }
    if (failed) {
      if (state() != sink::PacketSinkState::kFailed) {
        Fail("调度RemuxSink失败");
      }
      CloseOutput();
    }
  }

  void Execute(const Work& work) noexcept {
    if (Terminal()) {
      return;
    }
    try {
      work.run();
    } catch (const sink::FatalError& error) {
      Fail(error.what());
      owner_.ReportFatalError(error.what());
    } catch (const std::exception& error) {
      Fail(error.what());
    } catch (...) {
      Fail("RemuxSink处理失败：未知异常");
    }
  }

  void OpenStreams(const media::StreamsReady& streams) {
    if (streams.generation == 0 || streams.streams.empty()) {
      throw std::invalid_argument("RemuxSink输入代次和轨道不能为空");
    }
    if (generation_ != 0 && streams.generation == generation_ &&
        !pending_generation_) {
      return;
    }
    if (generation_ != 0 && pending_generation_ != streams.generation) {
      throw std::logic_error("RemuxSink新代次轨道必须先投递时间线重置");
    }
    for (const auto& stream : streams.streams) {
      stream.Validate();
    }
    if (!streams_.empty()) {
      if (streams_.size() != streams.streams.size()) {
        throw std::invalid_argument("RemuxSink不能跨代次改变轨道数量");
      }
      for (std::size_t index = 0; index < streams_.size(); ++index) {
        if (!SameStream(streams_[index], streams.streams[index])) {
          throw std::invalid_argument("RemuxSink不能跨代次改变轨道参数");
        }
      }
    } else {
      streams_ = streams.streams;
      for (const auto& stream : streams_) {
        if (!last_dts_.emplace(stream.stream_index, std::nullopt).second) {
          throw std::invalid_argument("RemuxSink轨道索引重复");
        }
      }
    }
    generation_ = streams.generation;
    pending_generation_.reset();
    input_ended_ = false;
    if (!output_) {
      OpenOutput();
    }
    SetState(sink::PacketSinkState::kRunning);
    owner_.StartMessages();
  }

  void WritePacket(const media::PacketReady& packet) {
    if (pending_generation_ || input_ended_ ||
        packet.generation != generation_) {
      throw std::logic_error("RemuxSink收到非当前就绪代次的数据包");
    }
    const auto* raw = packet.packet.get();
    if (!raw || raw->pts == AV_NOPTS_VALUE || raw->dts == AV_NOPTS_VALUE) {
      throw std::invalid_argument("RemuxSink数据包为空或缺少时间戳");
    }
    performance_.AddInput(1, raw->size);
    const auto track = last_dts_.find(raw->stream_index);
    if (track == last_dts_.end()) {
      throw std::invalid_argument("RemuxSink数据包轨道不存在");
    }
    if (track->second && raw->dts < *track->second) {
      Log::Warning(
          "RemuxSink DTS回退，重建输出：target={}, stream={}, {} -> {}",
          config_.target, raw->stream_index, *track->second, raw->dts);
      CloseOutput();
      if (state() == sink::PacketSinkState::kFailed) return;
      for (auto& entry : last_dts_) {
        entry.second.reset();
      }
      OpenOutput();
    }
    track->second = raw->dts;
    output_->Write(packet.packet);
  }

  void OpenOutput() {
    auto output = std::make_unique<output::internal::RemuxOutput>(
        output::internal::RemuxOutputConfig{config_.target, config_.zlm,
                                            config_.packet_queue_capacity},
        streams_, poller_, [this](const std::string& error) { Fail(error); },
        &performance_);
    output->Open();
    output_ = std::move(output);
  }

  void CloseOutput() noexcept {
    if (output_) {
      network_snapshot_ = output_->GetNetworkOutputSnapshot();
      if (state() != sink::PacketSinkState::kFailed) {
        try {
          output_->Finish();
        } catch (const std::exception& error) {
          Fail(error.what());
        } catch (...) {
          Fail("Remux输出收尾失败");
        }
      }
      output_->Close();
      output_.reset();
      network_snapshot_.connected = false;
    }
  }

  void EndInput(const media::StreamEnded& end) {
    if (pending_generation_ || input_ended_ || end.generation != generation_) {
      return;
    }
    input_ended_ = true;
    if (end.reason == media::StreamEndReason::kInterrupted) {
      return;
    }
    if (end.reason == media::StreamEndReason::kFailed) {
      Fail("RemuxSink输入失败");
      return;
    }
    SetState(sink::PacketSinkState::kDraining);
    CloseOutput();
    SetState(end.reason == media::StreamEndReason::kEof
                 ? sink::PacketSinkState::kEnded
                 : sink::PacketSinkState::kStopped);
    queue_.Close();
    queue_.Clear();
  }

  void ResetTimeline(const media::TimelineReset& reset) {
    if (reset.generation > generation_ &&
        (!pending_generation_ || reset.generation > *pending_generation_)) {
      pending_generation_ = reset.generation;
      input_ended_ = true;
    }
  }

  bool Terminal() const {
    const auto current = state();
    return current == sink::PacketSinkState::kFailed ||
           current == sink::PacketSinkState::kEnded ||
           current == sink::PacketSinkState::kStopped;
  }

  void SetState(sink::PacketSinkState next) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (state() != sink::PacketSinkState::kFailed) {
      state_.store(next);
    }
  }

  void Fail(const std::string& error) noexcept {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (state() == sink::PacketSinkState::kFailed) {
        return;
      }
      error_ = error;
      state_.store(sink::PacketSinkState::kFailed);
    }
    queue_.Close();
    queue_.Clear();
    Log::Error("RemuxSink失败：target={}, error={}", config_.target, error);
    // Asynchronous target notifications can occur inside muxer callbacks.
    // Defer destruction until that callback unwinds on the Poller.
    std::lock_guard<std::mutex> lock(submit_mutex_);
    try {
      ScheduleDrain();
    } catch (...) {
      // Stop's synchronous barrier still owns final cleanup.
    }
  }

  RemuxSink& owner_;
  const RemuxSinkConfig config_;
  toolkit::EventPoller::Ptr poller_;
  common::BlockingQueue<Work> queue_;
  std::mutex submit_mutex_;
  bool drain_scheduled_ = false;
  bool stop_requested_ = false;
  std::mutex stop_mutex_;
  bool stopped_ = false;
  mutable std::mutex status_mutex_;
  std::atomic<sink::PacketSinkState> state_{sink::PacketSinkState::kIdle};
  std::string error_;
  performance::OperationRecorder performance_{
      performance::PerformanceType::kRemux,
      performance::PerformanceUnit::kPacket,
      performance::PerformanceUnit::kPacket};
  // Remaining state is accessed only on poller_.
  std::unique_ptr<output::internal::RemuxOutput> output_;
  performance::NetworkOutputSnapshot network_snapshot_;
  std::vector<ffmpeg::StreamInfo> streams_;
  std::unordered_map<int, std::optional<std::int64_t>> last_dts_;
  std::uint64_t generation_ = 0;
  std::optional<std::uint64_t> pending_generation_;
  bool input_ended_ = true;
};

RemuxSink::RemuxSink(std::string id, RemuxSinkConfig config)
    : sink::Sink(std::move(id), sink::SinkMediaType::kPacket),
      impl_(std::make_unique<Impl>(*this, std::move(config))) {}

RemuxSink::~RemuxSink() { Stop(); }

void RemuxSink::OnStreamsReady(const media::StreamsReady& streams) noexcept {
  CloseRegistration();
  impl_->OnStreamsReady(streams);
}

void RemuxSink::OnPacket(const media::PacketReady& packet) noexcept {
  CloseRegistration();
  impl_->OnPacket(packet);
}

void RemuxSink::OnTimelineReset(const media::TimelineReset& reset) noexcept {
  CloseRegistration();
  impl_->OnTimelineReset(reset);
}

void RemuxSink::OnInputEnded(const media::StreamEnded& end) noexcept {
  CloseRegistration();
  impl_->OnInputEnded(end);
}

void RemuxSink::Stop() noexcept {
  StopMessages();
  impl_->Stop();
}

sink::PacketSinkState RemuxSink::state() const noexcept {
  return impl_->state();
}

std::string RemuxSink::error() const { return impl_->error(); }

std::size_t RemuxSink::queue_depth() const { return impl_->queue_depth(); }

performance::NodeSnapshot RemuxSink::GetOwnPerformance() const {
  return impl_->GetOwnPerformance();
}

performance::NetworkOutputSnapshot RemuxSink::GetNetworkOutputSnapshot() const {
  return impl_->GetNetworkOutputSnapshot();
}

}  // namespace mw::streamer::output
