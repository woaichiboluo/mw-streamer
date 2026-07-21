#include "mw/streamer/internal/output_worker.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

#include "fmt/format.h"
#include "spdlog/spdlog.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"
#include "mw/streamer/internal/fragmented_mp4_muxer.h"
#include "mw/streamer/internal/network_muxer.h"

namespace mw::streamer::internal {
namespace {

bool IsNetworkProtocol(OutputProtocol protocol) {
  return protocol == OutputProtocol::kRtmp ||
         protocol == OutputProtocol::kRtsp || protocol == OutputProtocol::kSrt;
}

std::int64_t PacketDtsUs(const EncodedPacket& packet) {
  const AVPacket* raw_packet = packet.packet.get();
  if (raw_packet == nullptr || raw_packet->dts == AV_NOPTS_VALUE ||
      !IsValidTimeBase(raw_packet->time_base)) {
    ThrowError(StatusCode::kInvalidArgument,
               "encoded packet is missing DTS or time base");
  }
  return RescaleTimestamp(raw_packet->dts, raw_packet->time_base,
                          AV_TIME_BASE_Q);
}

}  // namespace

EncodedPacket::EncodedPacket(EncodedMediaType media_type, ffmpeg::Packet packet)
    : media_type(media_type), packet(std::move(packet)) {}

EncodedPacket EncodedPacket::Ref() const {
  return EncodedPacket(media_type, packet.Ref());
}

void OutputPacketGate::Reset() noexcept {
  waiting_for_keyframe_ = true;
  cutover_dts_us_ = 0;
}

bool OutputPacketGate::ShouldWrite(const EncodedPacket& packet) {
  const AVPacket* raw_packet = packet.packet.get();
  if (waiting_for_keyframe_) {
    if (packet.media_type == EncodedMediaType::kAudio ||
        raw_packet == nullptr || (raw_packet->flags & AV_PKT_FLAG_KEY) == 0) {
      return false;
    }
    cutover_dts_us_ = PacketDtsUs(packet);
    waiting_for_keyframe_ = false;
    return true;
  }
  return PacketDtsUs(packet) >= cutover_dts_us_;
}

OutputWorker::OutputWorker(OutputConfig config,
                           const ffmpeg::CodecParameters& video_parameters,
                           AVRational video_time_base,
                           const ffmpeg::CodecParameters* audio_parameters,
                           AVRational audio_time_base,
                           OutputEventReporter reporter)
    : config_(std::move(config)),
      video_parameters_(video_parameters.Clone()),
      video_time_base_(video_time_base),
      audio_parameters_(audio_parameters == nullptr
                            ? nullptr
                            : std::make_unique<ffmpeg::CodecParameters>(
                                  audio_parameters->Clone())),
      audio_time_base_(audio_time_base),
      reporter_(std::move(reporter)),
      queue_(config_.packet_queue_capacity),
      reconnect_backoff_(config_.reconnect),
      random_engine_(std::random_device{}()) {}

OutputWorker::~OutputWorker() noexcept { Stop(); }

void OutputWorker::Start() {
  if (started_) {
    ThrowError(StatusCode::kInvalidState,
               "output worker can only be started once");
  }
  started_ = true;

  if (config_.protocol == OutputProtocol::kFile) {
    try {
      file_muxer_ = std::make_unique<FragmentedMp4Muxer>();
      file_muxer_->Open(std::filesystem::path(config_.url), video_parameters_,
                        video_time_base_, audio_parameters_.get(),
                        audio_time_base_);
    } catch (...) {
      Fail(CurrentExceptionStatus("open"));
      return;
    }
  } else if (IsNetworkProtocol(config_.protocol)) {
    network_error_ = OpenNetworkSession();
    if (!network_error_.ok()) {
      if (!reconnect_backoff_.CanRetry()) {
        Fail(network_error_);
        return;
      }
      Report(EndpointEventType::kReconnecting, network_error_);
    }
  } else {
    Fail(Status(StatusCode::kInvalidArgument,
                fmt::format("output '{}': unsupported protocol", config_.id)));
    return;
  }

  accepting_.store(true, std::memory_order_release);
  try {
    thread_ = std::thread(&OutputWorker::Run, this);
  } catch (...) {
    accepting_.store(false, std::memory_order_release);
    AbortNetworkSession();
    file_muxer_.reset();
    throw;
  }
}

bool OutputWorker::Enqueue(const EncodedPacket& packet) {
  if (!accepting_.load(std::memory_order_acquire)) {
    return false;
  }
  if (queue_.TryPush(packet.Ref())) {
    return true;
  }
  if (!accepting_.load(std::memory_order_acquire)) {
    return false;
  }

  if (config_.protocol == OutputProtocol::kFile) {
    overflow_pending_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    queue_.Cancel();
  } else {
    queue_.Clear();
    MarkActiveNetworkSessionOverflow();
    wait_condition_.notify_all();
  }
  return false;
}

void OutputWorker::Close() noexcept {
  if (!started_) {
    return;
  }
  accepting_.store(false, std::memory_order_release);
  close_requested_.store(true, std::memory_order_release);
  queue_.Close();
  wait_condition_.notify_all();
}

void OutputWorker::Wait() noexcept {
  if (thread_.joinable()) {
    thread_.join();
  }
}

void OutputWorker::RequestStop() noexcept {
  if (!started_) {
    return;
  }
  accepting_.store(false, std::memory_order_release);
  stop_requested_.store(true, std::memory_order_release);
  queue_.Cancel();
  CancelActiveNetworkOperation();
  wait_condition_.notify_all();
}

void OutputWorker::Stop() noexcept {
  RequestStop();
  Wait();
  AbortNetworkSession();
  file_muxer_.reset();
}

bool OutputWorker::is_accepting() const noexcept {
  return accepting_.load(std::memory_order_acquire);
}

void OutputWorker::Run() noexcept {
  try {
    if (config_.protocol == OutputProtocol::kFile) {
      RunFile();
    } else {
      RunNetwork();
    }
  } catch (...) {
    if (!stop_requested_.load(std::memory_order_acquire)) {
      Fail(CurrentExceptionStatus("worker"));
    }
  }
}

void OutputWorker::RunFile() {
  while (true) {
    if (overflow_pending_.exchange(false, std::memory_order_acq_rel)) {
      Fail(Status(
          StatusCode::kUnavailable,
          fmt::format("output '{}': packet queue overflow", config_.id)));
      file_muxer_.reset();
      return;
    }

    std::optional<EncodedPacket> packet = queue_.Pop();
    if (!packet.has_value()) {
      break;
    }
    WriteFile(std::move(*packet));
  }

  if (stop_requested_.load(std::memory_order_acquire)) {
    file_muxer_.reset();
    return;
  }
  file_muxer_->Finish();
  file_muxer_.reset();
}

void OutputWorker::RunNetwork() {
  bool reconnecting = !network_error_.ok();
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (close_requested_.load(std::memory_order_acquire) &&
        network_muxer_ == nullptr) {
      return;
    }

    if (network_muxer_ == nullptr) {
      if (!reconnect_backoff_.CanRetry()) {
        Fail(Status(StatusCode::kUnavailable,
                    fmt::format("output '{}': reconnect retries exhausted: {}",
                                config_.id, network_error_.message())));
        return;
      }

      const std::chrono::milliseconds delay =
          reconnect_backoff_.NextDelay(NextJitterSample());
      std::unique_lock<std::mutex> lock(wait_mutex_);
      wait_condition_.wait_for(lock, delay, [this] {
        return stop_requested_.load(std::memory_order_acquire) ||
               close_requested_.load(std::memory_order_acquire);
      });
      lock.unlock();
      if (stop_requested_.load(std::memory_order_acquire) ||
          close_requested_.load(std::memory_order_acquire)) {
        return;
      }

      network_error_ = OpenNetworkSession();
      if (!network_error_.ok()) {
        continue;
      }
      packet_gate_.Reset();
    }

    if (overflow_pending_.exchange(false, std::memory_order_acq_rel)) {
      network_error_ =
          Status(StatusCode::kUnavailable,
                 fmt::format("output '{}': packet queue overflow", config_.id));
      AbortNetworkSession();
      queue_.Clear();
      if (!reconnecting) {
        Report(EndpointEventType::kReconnecting, network_error_);
        reconnecting = true;
      }
      continue;
    }

    std::optional<EncodedPacket> packet =
        queue_.PopFor(std::chrono::milliseconds(100));
    if (!packet.has_value()) {
      if (queue_.is_closed()) {
        break;
      }
      continue;
    }
    if (!packet_gate_.ShouldWrite(*packet)) {
      continue;
    }
    try {
      WriteNetwork(std::move(*packet));
      if (reconnecting) {
        reconnect_backoff_.Reset();
        Report(EndpointEventType::kRecovered, Status::Ok());
        reconnecting = false;
      }
    } catch (...) {
      if (overflow_pending_.exchange(false, std::memory_order_acq_rel)) {
        network_error_ = Status(
            StatusCode::kUnavailable,
            fmt::format("output '{}': packet queue overflow", config_.id));
      } else {
        network_error_ = CurrentExceptionStatus("write");
      }
      AbortNetworkSession();
      queue_.Clear();
      if (!reconnecting) {
        Report(EndpointEventType::kReconnecting, network_error_);
        reconnecting = true;
      }
    }
  }

  if (stop_requested_.load(std::memory_order_acquire)) {
    AbortNetworkSession();
    return;
  }
  try {
    network_muxer_->Finish();
    {
      std::lock_guard<std::mutex> lock(interrupt_mutex_);
      active_interrupt_ = nullptr;
    }
    network_muxer_.reset();
    network_interrupt_.reset();
  } catch (...) {
    const Status status = CurrentExceptionStatus("finish");
    AbortNetworkSession();
    Fail(status);
  }
}

void OutputWorker::WriteFile(EncodedPacket packet) {
  if (packet.media_type == EncodedMediaType::kVideo) {
    file_muxer_->WriteVideo(std::move(packet.packet));
  } else {
    file_muxer_->WriteAudio(std::move(packet.packet));
  }
}

void OutputWorker::WriteNetwork(EncodedPacket packet) {
  if (packet.media_type == EncodedMediaType::kVideo) {
    network_muxer_->WriteVideo(std::move(packet.packet));
  } else {
    network_muxer_->WriteAudio(std::move(packet.packet));
  }
}

Status OutputWorker::OpenNetworkSession() {
  auto interrupt = std::make_unique<FfmpegInterrupt>();
  auto muxer = std::make_unique<NetworkMuxer>();
  {
    std::lock_guard<std::mutex> lock(interrupt_mutex_);
    overflow_pending_.store(false, std::memory_order_release);
    active_interrupt_ = interrupt.get();
  }
  try {
    muxer->Open(config_, video_parameters_, video_time_base_,
                audio_parameters_.get(), audio_time_base_, interrupt.get());
  } catch (...) {
    const Status status = CurrentExceptionStatus("open");
    {
      std::lock_guard<std::mutex> lock(interrupt_mutex_);
      active_interrupt_ = nullptr;
    }
    muxer->Abort();
    return status;
  }

  network_interrupt_ = std::move(interrupt);
  network_muxer_ = std::move(muxer);
  return Status::Ok();
}

void OutputWorker::MarkActiveNetworkSessionOverflow() noexcept {
  std::lock_guard<std::mutex> lock(interrupt_mutex_);
  if (active_interrupt_ == nullptr) {
    return;
  }
  overflow_pending_.store(true, std::memory_order_release);
  active_interrupt_->Cancel();
}

void OutputWorker::AbortNetworkSession() noexcept {
  {
    std::lock_guard<std::mutex> lock(interrupt_mutex_);
    active_interrupt_ = nullptr;
  }
  if (network_muxer_ != nullptr) {
    network_muxer_->Abort();
    network_muxer_.reset();
  }
  network_interrupt_.reset();
}

void OutputWorker::CancelActiveNetworkOperation() noexcept {
  std::lock_guard<std::mutex> lock(interrupt_mutex_);
  if (active_interrupt_ != nullptr) {
    active_interrupt_->Cancel();
  }
}

void OutputWorker::Report(EndpointEventType type,
                          const Status& status) noexcept {
  if (stop_requested_.load(std::memory_order_acquire) || !reporter_) {
    return;
  }
  try {
    reporter_(type, status);
  } catch (const std::exception& error) {
    spdlog::warn("output '{}' event reporter threw: {}", config_.id,
                 error.what());
  } catch (...) {
    spdlog::warn("output '{}' event reporter threw an unknown exception",
                 config_.id);
  }
}

void OutputWorker::Fail(const Status& status) noexcept {
  accepting_.store(false, std::memory_order_release);
  queue_.Cancel();
  Report(EndpointEventType::kFailed, status);
}

Status OutputWorker::CurrentExceptionStatus(const char* stage) const {
  try {
    throw;
  } catch (const Error& error) {
    return Status(error.code(), fmt::format("output '{}' {} failed: {}",
                                            config_.id, stage, error.what()));
  } catch (const std::exception& error) {
    return Status(StatusCode::kInternal,
                  fmt::format("output '{}' {} failed: {}", config_.id, stage,
                              error.what()));
  } catch (...) {
    return Status(StatusCode::kInternal,
                  fmt::format("output '{}' {} failed with an unknown error",
                              config_.id, stage));
  }
}

double OutputWorker::NextJitterSample() {
  return jitter_distribution_(random_engine_);
}

}  // namespace mw::streamer::internal
