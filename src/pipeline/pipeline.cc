#include "mw/pipeline/pipeline.h"

#include <fmt/format.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"
#include "mw/common/thread.h"
#include "mw/init/internal/runtime.h"
#include "mw/log/logging.h"
#include "mw/processor/analysis_processor_sink.h"
#include "mw/processor/transform_processor_sink.h"

namespace mw::streamer::pipeline {
namespace {

std::atomic<std::uint64_t> next_performance_id{1};

struct OwnedMessage {
  std::string sink_id;
  std::string type;
  std::vector<std::uint8_t> payload;
  std::optional<MwStreamerMediaTimestamp> timestamp;

  explicit OwnedMessage(const sink::SinkMessage& message)
      : sink_id(message.sink_id),
        type(message.type),
        timestamp(message.timestamp) {
    if (message.payload_size != 0) {
      if (!message.payload)
        throw std::invalid_argument("Sink消息payload不能为空");
      payload.resize(message.payload_size);
      std::memcpy(payload.data(), message.payload, message.payload_size);
    }
  }
  sink::SinkMessage view() const {
    return {sink_id, type, payload.data(), payload.size(), timestamp};
  }
};

}  // namespace

class Pipeline::Impl final : public input::Input::Observer {
 public:
  explicit Impl(std::unique_ptr<input::Input> input)
      : input_(std::move(input)) {
    if (!input_) {
      throw std::invalid_argument("Pipeline输入不能为空");
    }
  }

  ~Impl() override {
    Stop();
    // Release backend resources while every borrowed sink is still alive.
    input_.reset();
    // Injected senders borrow this Impl; retain message facilities until every
    // sink destructor has finished.
    sinks_.clear();
  }

  void AddSink(std::unique_ptr<sink::Sink> sink) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (started_ || stopped_) {
      throw std::logic_error("Sink只能在Pipeline启动或停止之前注册");
    }
    if (!sink) {
      throw std::invalid_argument("Sink不能为空");
    }
    if (sink->input_type() != sink::SinkMediaType::kPacket) {
      throw std::invalid_argument("Pipeline输入下游必须消费Packet");
    }
    sink->SetOnFatalError(
        [this](const std::string& error) { RequestFatalStop(error); });
    sinks_.push_back(std::move(sink));
  }

  void SetMessageReceiver(std::string sender_id, std::string receiver_id) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (started_ || stopped_) {
      throw std::logic_error("消息接收者只能在Pipeline启动或停止之前设置");
    }
    if (sender_id.empty() || receiver_id.empty()) {
      throw std::invalid_argument("消息路由的Sink ID不能为空");
    }
    message_routes_.insert_or_assign(std::move(sender_id),
                                     std::move(receiver_id));
  }

  void SetProcessorConfig(std::string processor_id, std::string config) {
    if (processor_id.empty()) {
      throw std::invalid_argument("Processor ID不能为空");
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (stopped_) {
      throw std::logic_error("Pipeline停止后不能更新Processor配置");
    }
    sink::Sink* target = FindSink(processor_id);
    if (!target) {
      throw std::invalid_argument(
          fmt::format("Processor不存在: {}", processor_id));
    }
    if (auto* analysis =
            dynamic_cast<processor::AnalysisProcessorSink*>(target)) {
      analysis->UpdateConfig(std::move(config));
      return;
    }
    if (auto* transform =
            dynamic_cast<processor::TransformProcessorSink*>(target)) {
      transform->UpdateConfig(std::move(config));
      return;
    }
    throw std::invalid_argument(
        fmt::format("Sink不是Processor: {}", processor_id));
  }

  void Start() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (started_ || stopped_) {
      throw std::logic_error("Pipeline只能启动一次，停止后不能重新启动");
    }
    if (sinks_.empty()) {
      throw std::logic_error("Pipeline至少需要一个Sink");
    }
    started_ = true;
    try {
      StartMessages();
      stop_thread_ = std::make_unique<common::Thread>("mw-pipeline-stop",
                                                      [this]() { RunStop(); });
      SetState(PipelineState::kRunning);
      input_->Start(*this);
    } catch (const std::exception& error) {
      RequestFatalStop(error.what());
      StopComponents();
      throw;
    } catch (...) {
      RequestFatalStop("Pipeline启动发生未知异常");
      StopComponents();
      throw;
    }
  }

  void Stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      if (!stopped_) {
        StopComponents();
      }
    }
    // The fatal control thread may be waiting for control_mutex_. Never join
    // it under that mutex; concurrent external Stop calls also serialize Join.
    std::lock_guard<std::mutex> lock(join_mutex_);
    if (stop_thread_) {
      stop_thread_->Join();
    }
  }

  PipelineState state() const noexcept { return state_.load(); }

  std::string error() const {
    std::lock_guard<std::mutex> lock(request_mutex_);
    return fatal_error_;
  }

  input::InputStateChanged input_status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return input_status_;
  }

  performance::PipelineSnapshot GetPerformance() const {
    performance::PipelineSnapshot snapshot;
    snapshot.pipeline_id = performance_id_;
    snapshot.input = input_->GetPerformance();
    snapshot.input.id = "input";
    snapshot.sinks.reserve(sinks_.size());
    // Ownership stays stable through Stop. Avoid control_mutex_: Stop can be
    // waiting for a slow business callback whose in-flight stats we need to
    // see.
    for (std::size_t i = 0; i < sinks_.size(); ++i) {
      auto node = sinks_[i]->GetPerformance();
      snapshot.sinks.push_back(std::move(node));
    }
    snapshot.sampled_at = std::chrono::steady_clock::now();
    return snapshot;
  }

 private:
  sink::Sink* FindSink(const std::string& id) const {
    if (!sinks_by_id_.empty()) {
      const auto found = sinks_by_id_.find(id);
      return found == sinks_by_id_.end() ? nullptr : found->second;
    }
    for (const auto& sink : sinks_) {
      if (auto* found = FindSink(*sink, id)) return found;
    }
    return nullptr;
  }

  static sink::Sink* FindSink(sink::Sink& sink, const std::string& id) {
    if (sink.id() == id) return &sink;
    for (const auto& child : sink.downstream()) {
      if (auto* found = FindSink(*child, id)) return found;
    }
    return nullptr;
  }

  void IndexSink(sink::Sink& sink) {
    if (!sinks_by_id_.emplace(sink.id(), &sink).second) {
      throw std::invalid_argument(fmt::format("Sink ID重复: {}", sink.id()));
    }
    for (const auto& child : sink.downstream()) IndexSink(*child);
  }

  void StartMessages() {
    for (const auto& sink : sinks_) IndexSink(*sink);
    for (const auto& [sender, receiver] : message_routes_) {
      if (!sinks_by_id_.count(sender) || !sinks_by_id_.count(receiver)) {
        throw std::invalid_argument(fmt::format(
            "消息路由引用不存在的Sink: {} -> {}", sender, receiver));
      }
      sinks_by_id_.at(sender)->SetMessageSender(
          [this, target_id = receiver](const sink::SinkMessage& message) {
            PostMessage(target_id, message);
          });
    }
    for (const auto& [id, sink] : sinks_by_id_) sink->CloseRegistration();
    message_poller_ = toolkit::EventPollerPool::Instance().extractPoller();
    std::lock_guard<std::mutex> lock(message_submission_mutex_);
    messages_open_ = true;
  }

  void PostMessage(const std::string& target_id,
                   const sink::SinkMessage& message) {
    // Poller already serializes its task queue. This lock only orders task
    // submission before Stop's barrier, preventing a late captured Impl.
    std::lock_guard<std::mutex> lock(message_submission_mutex_);
    if (!messages_open_ || state() == PipelineState::kFailed) return;
    message_poller_->async(
        [this, target_id, copy = OwnedMessage(message)] {
          {
            std::lock_guard<std::mutex> lock(message_submission_mutex_);
            if (!messages_open_ || state() == PipelineState::kFailed) return;
          }
          // Never hold the submission lock across business callbacks: they
          // may send another message. All sinks outlive Stop's barrier.
          sinks_by_id_.at(target_id)->DispatchMessage(copy.view());
        },
        false);
  }

  void StopMessages() noexcept {
    {
      std::lock_guard<std::mutex> lock(message_submission_mutex_);
      messages_open_ = false;
    }
    // Pending tasks skip business dispatch after closure. Wait outside the
    // submission lock for them and any active callback before releasing sinks.
    if (message_poller_) message_poller_->sync([] {});
  }

  void RequestFatalStop(const std::string& error) noexcept {
    {
      std::lock_guard<std::mutex> lock(request_mutex_);
      if (fatal_requested_) {
        return;
      }
      fatal_error_ = error;
      fatal_requested_ = true;
      state_.store(PipelineState::kFailed);
    }
    stop_requested_.notify_one();
    log::Module<log::LogModule::kStreamer>::Error("Pipeline发生fatal错误: {}",
                                                  error);
  }

  void RunStop() noexcept {
    {
      std::unique_lock<std::mutex> lock(request_mutex_);
      stop_requested_.wait(
          lock, [this]() { return fatal_requested_ || exit_requested_; });
      if (exit_requested_) {
        return;
      }
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!stopped_) {
      StopComponents();
    }
  }

  void SetState(PipelineState state) noexcept {
    auto current = state_.load();
    while (current != PipelineState::kFailed &&
           !state_.compare_exchange_weak(current, state)) {
    }
  }

  void StopComponents() noexcept {
    SetState(PipelineState::kStopping);
    StopMessages();
    for (const auto& sink : sinks_) {
      sink->RequestStop();
    }
    input_->Stop();
    for (const auto& sink : sinks_) {
      sink->Stop();
    }
    stopped_ = true;
    SetState(PipelineState::kStopped);
    // Stop before Start has no observer notification. Reflect it in the
    // snapshot too, after all possible input callbacks have completed.
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      input_status_.state = input_->state();
      input_status_.will_retry = false;
    }
    {
      std::lock_guard<std::mutex> lock(request_mutex_);
      exit_requested_ = true;
    }
    stop_requested_.notify_one();
  }

  void OnStreamsReady(const media::StreamsReady& streams) noexcept override {
    for (const auto& sink : sinks_) {
      if (state() == PipelineState::kFailed) {
        return;
      }
      sink->OnStreamsReady(streams);
    }
  }

  void OnPacket(const media::PacketReady& packet) noexcept override {
    for (const auto& sink : sinks_) {
      if (state() == PipelineState::kFailed) {
        return;
      }
      sink->OnPacket(packet);
    }
  }

  void OnTimelineReset(const media::TimelineReset& reset) noexcept override {
    for (const auto& sink : sinks_) {
      if (state() == PipelineState::kFailed) {
        return;
      }
      sink->OnTimelineReset(reset);
    }
  }

  void OnInputEnded(const media::StreamEnded& end) noexcept override {
    for (const auto& sink : sinks_) {
      if (state() == PipelineState::kFailed) {
        return;
      }
      sink->OnInputEnded(end);
    }
  }

  void OnInputStateChanged(
      const input::InputStateChanged& state) noexcept override {
    std::lock_guard<std::mutex> lock(status_mutex_);
    input_status_ = state;
  }

  std::mutex control_mutex_;
  const std::uint64_t performance_id_ =
      next_performance_id.fetch_add(1, std::memory_order_relaxed);
  std::mutex join_mutex_;
  mutable std::mutex status_mutex_;
  mutable std::mutex request_mutex_;
  std::condition_variable stop_requested_;
  bool fatal_requested_ = false;
  bool exit_requested_ = false;
  std::string fatal_error_;
  std::atomic<PipelineState> state_{PipelineState::kIdle};
  bool started_ = false;
  bool stopped_ = false;
  input::InputStateChanged input_status_{
      0, input::InputState::kIdle, {}, false};
  // Setup-only routes and immutable runtime index; ownership stays in sinks_.
  std::unordered_map<std::string, std::string> message_routes_;
  std::unordered_map<std::string, sink::Sink*> sinks_by_id_;
  std::mutex message_submission_mutex_;
  toolkit::EventPoller::Ptr message_poller_;
  bool messages_open_ = false;
  std::vector<std::unique_ptr<sink::Sink>> sinks_;
  std::unique_ptr<input::Input> input_;
  std::unique_ptr<common::Thread> stop_thread_;
};

Pipeline::Pipeline(std::unique_ptr<input::Input> input)
    : impl_([&input] {
        init::internal::EnsureInitialized();
        return std::make_unique<Impl>(std::move(input));
      }()) {}

Pipeline::~Pipeline() = default;

void Pipeline::AddSink(std::unique_ptr<sink::Sink> sink) {
  impl_->AddSink(std::move(sink));
}

void Pipeline::SetMessageReceiver(std::string sender_id,
                                  std::string receiver_id) {
  impl_->SetMessageReceiver(std::move(sender_id), std::move(receiver_id));
}

void Pipeline::SetProcessorConfig(std::string processor_id,
                                  std::string config) {
  impl_->SetProcessorConfig(std::move(processor_id), std::move(config));
}

void Pipeline::Start() { impl_->Start(); }

void Pipeline::Stop() noexcept { impl_->Stop(); }

PipelineState Pipeline::state() const noexcept { return impl_->state(); }

std::string Pipeline::error() const { return impl_->error(); }

input::InputStateChanged Pipeline::input_status() const {
  return impl_->input_status();
}

performance::PipelineSnapshot Pipeline::GetPerformance() const {
  return impl_->GetPerformance();
}

}  // namespace mw::streamer::pipeline
