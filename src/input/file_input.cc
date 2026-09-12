#include "mw/input/file_input.h"

#include <atomic>
#include <exception>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/common/thread.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/performance/operation_recorder.h"

namespace mw::streamer {
namespace {

// Checking before acquiring the control mutex also catches a callback trying
// to stop itself while another thread is already joining it.
thread_local const void* current_file_input = nullptr;
constexpr std::uint64_t kGeneration = 1;

int FindBestStream(AVFormatContext& context, AVMediaType media_type) {
  const int best =
      av_find_best_stream(&context, media_type, -1, -1, nullptr, 0);
  if (best < 0) {
    return -1;
  }
  if (media_type != AVMEDIA_TYPE_VIDEO ||
      (context.streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
    return best;
  }
  for (unsigned int index = 0; index < context.nb_streams; ++index) {
    const auto* stream = context.streams[index];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

StreamsReady SelectStreams(AVFormatContext& context) {
  StreamsReady result{
      kGeneration, {}, StreamDeliveryMode::kOffline};
  for (const auto type : {AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_VIDEO}) {
    const int index = FindBestStream(context, type);
    if (index < 0) {
      continue;
    }
    const auto& stream = *context.streams[index];
    StreamInfo info;
    info.stream_index = stream.index;
    info.codec_parameters = CodecParameters(*stream.codecpar);
    info.time_base = stream.time_base;
    info.Validate();
    result.streams.push_back(std::move(info));
  }
  if (result.streams.empty()) {
    throw std::invalid_argument("文件不包含可处理的音频或视频");
  }
  return result;
}

}  // namespace

class FileInput::Impl final {
 public:
  explicit Impl(FileInputConfig config) : config_(std::move(config)) {}
  ~Impl() { Stop(); }

  void Start(Observer& observer) {
    if (current_file_input == this) {
      throw std::logic_error("FileInput控制操作不能在输入线程中调用");
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (started_ || stopped_) {
      throw std::logic_error("FileInput只能启动一次，停止后不能重新启动");
    }
    if (config_.path.empty()) {
      throw std::invalid_argument("输入文件路径不能为空");
    }
    observer_ = &observer;
    try {
      worker_ =
          std::make_unique<Thread>("file-input", [this] { Run(); });
      started_ = true;
    } catch (...) {
      observer_ = nullptr;
      throw;
    }
  }

  void Stop() noexcept {
    if (current_file_input == this) {
      std::terminate();
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    stop_requested_.store(true, std::memory_order_relaxed);
    if (worker_) {
      worker_->Join();
      worker_.reset();
    }
    observer_ = nullptr;
    state_.store(InputState::kStopped, std::memory_order_relaxed);
  }

  InputState state() const noexcept {
    return state_.load(std::memory_order_relaxed);
  }

  NodeSnapshot GetPerformance() const {
    return {{}, "FileInput", {performance_.GetSnapshot()}, {}};
  }

 private:
  static int InterruptIo(void* opaque) noexcept {
    return static_cast<Impl*>(opaque)->stop_requested_.load(
        std::memory_order_relaxed);
  }

  void SetState(InputState state, std::string error = {}) noexcept {
    state_.store(state, std::memory_order_relaxed);
    observer_->OnInputStateChanged(
        {kGeneration, state, std::move(error), false});
  }

  void ReadFile() {
    // An absolute path prevents FFmpeg interpreting a local name as a protocol.
    InputFormatContext context(
        std::filesystem::absolute(config_.path).string(), {InterruptIo, this});
    context.FindStreamInfo();
    if (stop_requested_.load(std::memory_order_relaxed)) {
      return;
    }
    const auto streams = SelectStreams(*context.get());
    streams_ready_ = true;
    observer_->OnStreamsReady(streams);
    SetState(InputState::kReady);

    PacketReady packet{kGeneration, Packet{}};
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      packet.packet.Unref();
      if (!context.ReadPacket(packet.packet)) {
        return;
      }
      for (const auto& stream : streams.streams) {
        if (packet.packet->stream_index == stream.stream_index) {
          packet.packet->time_base = stream.time_base;
          performance_.AddOutput(1, packet.packet->size);
          observer_->OnPacket(packet);
          break;
        }
      }
    }
  }

  void Run() noexcept {
    current_file_input = this;
    SetState(InputState::kConnecting);
    std::string error;
    auto state = InputState::kFailed;
    auto reason = StreamEndReason::kFailed;
    try {
      ReadFile();
      state = InputState::kEnded;
      reason = StreamEndReason::kEof;
    } catch (const std::exception& exception) {
      error = exception.what();
    } catch (...) {
      error = "未知文件输入错误";
    }
    if (stop_requested_.load(std::memory_order_relaxed)) {
      state = InputState::kStopped;
      reason = StreamEndReason::kStopped;
      error.clear();
    }
    state_.store(state, std::memory_order_relaxed);
    if (streams_ready_) {
      observer_->OnInputEnded({kGeneration, reason});
    }
    SetState(state, std::move(error));
    current_file_input = nullptr;
  }

  FileInputConfig config_;
  std::mutex control_mutex_;
  bool started_ = false;
  bool stopped_ = false;
  std::atomic<bool> stop_requested_{false};
  std::atomic<InputState> state_{InputState::kIdle};
  std::unique_ptr<Thread> worker_;
  // Borrowed until Join; accessed only on the file thread while running.
  Observer* observer_ = nullptr;
  bool streams_ready_ = false;
  OperationRecorder performance_{
      PerformanceType::kInput, PerformanceUnit::kNone,
      PerformanceUnit::kPacket};
};

FileInput::FileInput(FileInputConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

FileInput::~FileInput() = default;
void FileInput::Start(Observer& observer) { impl_->Start(observer); }
void FileInput::Stop() noexcept { impl_->Stop(); }
InputState FileInput::state() const noexcept { return impl_->state(); }
NodeSnapshot FileInput::GetPerformance() const {
  return impl_->GetPerformance();
}

}  // namespace mw::streamer
