#include "mw/sink/sink.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "mw/log/logging.h"

namespace mw::streamer::sink {
class Sink::Impl final {
 public:
  Impl(std::string id, SinkMediaType input, SinkMediaType output)
      : id_(std::move(id)), input_(input), output_(output) {
    if (id_.empty()) throw std::invalid_argument("Sink ID不能为空");
  }

  void RequireSetup() const {
    if (registration_closed_.load()) {
      throw std::logic_error("Sink连接只能在输入或停止之前设置");
    }
  }

  const std::string id_;
  const SinkMediaType input_;
  const SinkMediaType output_;
  std::vector<std::unique_ptr<Sink>> children_;
  MessageSender sender_;
  OnFatalError fatal_handler_;
  std::atomic<bool> registration_closed_{false};
  std::atomic<bool> fatal_reported_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> ready_{false};
  std::mutex message_dispatch_mutex_;
  std::mutex children_stop_mutex_;
  bool children_stopped_ = false;
};

Sink::Sink(std::string id, SinkMediaType input_type, SinkMediaType output_type)
    : impl_(std::make_unique<Impl>(std::move(id), input_type, output_type)) {}

Sink::~Sink() { Sink::Stop(); }

void Sink::AddSink(std::unique_ptr<Sink> sink) {
  impl_->RequireSetup();
  if (!sink) throw std::invalid_argument("下游Sink不能为空");
  if (sink.get() == this || output_type() == SinkMediaType::kNone ||
      output_type() != sink->input_type()) {
    throw std::invalid_argument("Sink上下游媒体类型不匹配");
  }
  sink->impl_->RequireSetup();
  sink->SetOnFatalError(
      [this](const std::string& error) { HandleFatalError(error); });
  impl_->children_.push_back(std::move(sink));
}

const std::string& Sink::id() const noexcept { return impl_->id_; }

SinkMediaType Sink::input_type() const noexcept { return impl_->input_; }
SinkMediaType Sink::output_type() const noexcept { return impl_->output_; }

void Sink::SetMessageSender(MessageSender sender) {
  impl_->RequireSetup();
  impl_->sender_ = std::move(sender);
}

void Sink::SendMessage(const SinkMessage& message) const {
  if (!impl_->stopping_.load() && impl_->sender_) impl_->sender_(message);
}

void Sink::DispatchMessage(const SinkMessage& message) noexcept {
  try {
    std::lock_guard<std::mutex> lock(impl_->message_dispatch_mutex_);
    if (impl_->ready_.load() && !impl_->stopping_.load()) OnMessage(message);
  } catch (const FatalError& error) {
    impl_->ready_.store(false);
    // Report outside the dispatch lock: failure handling may stop this sink.
    HandleFatalError(error.what());
  } catch (const std::exception& error) {
    log::Module<log::LogModule::kStreamer>::Error("Sink消息处理失败: {}",
                                                  error.what());
  } catch (...) {
    log::Module<log::LogModule::kStreamer>::Error("Sink消息处理发生未知异常");
  }
}

void Sink::SetOnFatalError(OnFatalError callback) {
  impl_->RequireSetup();
  impl_->fatal_handler_ = std::move(callback);
}

void Sink::OnStreamsReady(const media::StreamsReady&) {
  if (input_type() != SinkMediaType::kPacket)
    throw std::logic_error("Sink不消费Packet");
  StartMessages();
}

void Sink::OnStreamsReady(const media::FrameStreamsReady&) {
  if (input_type() != SinkMediaType::kFrame)
    throw std::logic_error("Sink不消费Frame");
  StartMessages();
}

void Sink::OnPacket(const media::PacketReady&) {
  throw std::logic_error("Sink未实现Packet处理");
}
void Sink::OnAudioFrame(const media::FrameReady&) {
  throw std::logic_error("Sink未实现音频处理");
}
void Sink::OnVideoFrame(const media::FrameReady&) {
  throw std::logic_error("Sink未实现视频处理");
}
void Sink::OnTimelineReset(const media::TimelineReset&) {}
void Sink::OnInputEnded(const media::StreamEnded&) {}
void Sink::OnMessage(const SinkMessage&) {}

void Sink::RequestStop() noexcept {
  for (const auto& child : downstream()) {
    child->RequestStop();
  }
}

void Sink::Stop() noexcept {
  StopMessages();
  StopDownstream();
}

void Sink::CloseRegistration() noexcept {
  impl_->registration_closed_.store(true);
}
void Sink::StartMessages() {
  CloseRegistration();
  impl_->ready_.store(true);
}
void Sink::StopMessages() noexcept {
  CloseRegistration();
  impl_->stopping_.store(true);
  // Do not acquire this mutex in StartMessages: it can hold the Processor's
  // lifecycle lock while a dispatched message is waiting to acquire it.
  std::lock_guard<std::mutex> lock(impl_->message_dispatch_mutex_);
}

void Sink::StopDownstream() noexcept {
  std::lock_guard<std::mutex> lock(impl_->children_stop_mutex_);
  if (impl_->children_stopped_) return;
  impl_->children_stopped_ = true;
  for (const auto& child : impl_->children_) child->Stop();
}

const std::vector<std::unique_ptr<Sink>>& Sink::downstream() const noexcept {
  return impl_->children_;
}

performance::NodeSnapshot Sink::GetOwnPerformance() const {
  return {{}, "Sink", {}, {}};
}

performance::NodeSnapshot Sink::GetPerformance() const {
  auto result = GetOwnPerformance();
  result.id = id();
  for (const auto& child : impl_->children_)
    result.downstream.push_back(child->GetPerformance());
  return result;
}

void Sink::HandleFatalError(const std::string& error) noexcept {
  ReportFatalError(error);
}

void Sink::ReportFatalError(const std::string& error) noexcept {
  if (!impl_->fatal_reported_.exchange(true) && impl_->fatal_handler_)
    impl_->fatal_handler_(error);
}

void Sink::SendStreamsReady(const media::StreamsReady& streams) {
  for (const auto& child : impl_->children_) child->OnStreamsReady(streams);
}
void Sink::SendStreamsReady(const media::FrameStreamsReady& streams) {
  for (const auto& child : impl_->children_) child->OnStreamsReady(streams);
}
void Sink::SendPacket(const media::PacketReady& packet) {
  for (const auto& child : impl_->children_) child->OnPacket(packet);
}
void Sink::SendAudioFrame(const media::FrameReady& frame) {
  for (const auto& child : impl_->children_) child->OnAudioFrame(frame);
}
void Sink::SendVideoFrame(const media::FrameReady& frame) {
  for (const auto& child : impl_->children_) child->OnVideoFrame(frame);
}
void Sink::SendTimelineReset(const media::TimelineReset& reset) {
  for (const auto& child : impl_->children_) child->OnTimelineReset(reset);
}
void Sink::SendInputEnded(const media::StreamEnded& end) {
  for (const auto& child : impl_->children_) child->OnInputEnded(end);
}

}  // namespace mw::streamer::sink
