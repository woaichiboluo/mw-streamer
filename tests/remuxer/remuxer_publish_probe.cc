#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "mw/streamer/encoder/encoder.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/remuxer/remuxer.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using namespace std::chrono_literals;

class Publishing final {
 public:
  Publishing() : device_(ffmpeg::HwDeviceType::kCpu), input_(device_) {
    input_.SetOnReady([this](const auto& streams) noexcept {
      Capture([&] {
        std::unique_lock lock(mutex_);
        streams_ = streams;
        input_ready_ = true;
        changed_.notify_all();
        // Downstream Start calls run on the main thread. Hold the first input
        // delivery until every stage has been configured from these streams.
        changed_.wait(lock, [&] { return released_ || stopping_ || error_; });
      });
    });
    input_.SetOnFrame([this](int, const auto& frame) noexcept {
      Capture([&] {
        if (!active_.load()) return;
        const auto accepted = frame->width > 0 ? scheduler_.SubmitVideo(frame)
                                               : scheduler_.SubmitAudio(frame);
        if (!accepted) throw std::runtime_error("Scheduler拒绝输入帧");
      });
    });
    input_.SetOnStateChanged([this](mw::streamer::InputState state, int error,
                                    std::string_view message) noexcept {
      Capture([&] {
        if (state == mw::streamer::InputState::kFailed)
          throw ffmpeg::FfmpegException(error, message);
        if (state == mw::streamer::InputState::kEnded)
          throw std::runtime_error("输入流已结束");
        if (state == mw::streamer::InputState::kWaitingRetry)
          fmt::print(stderr, "输入等待重连: {} {}\n", error, message);
      });
    });
    scheduler_.SetOnVideo([this](const auto& frame) noexcept {
      Capture([&] {
        if (!encoder_.SubmitVideo(processor_.ProcessVideo(frame)))
          throw std::runtime_error("Encoder拒绝处理后的视频帧");
      });
    });
    scheduler_.SetOnAudio([this](const auto& frame) noexcept {
      Capture([&] {
        if (!encoder_.SubmitAudio(processor_.ProcessAudio(frame)))
          throw std::runtime_error("Encoder拒绝处理后的音频帧");
      });
    });
    encoder_.SetOnReady([this](const auto& streams) noexcept {
      Capture([&] { encoded_streams_ = streams; });
    });
    encoder_.SetOnPacket(
        [this](const auto& packet, std::int64_t dts_ns) noexcept {
          Capture([&] {
            if (!remuxer_.SubmitPacket(packet, dts_ns))
              throw std::runtime_error("Remuxer拒绝编码包");
            ++packets_;
          });
        });
    encoder_.SetOnEnded([this]() noexcept {
      std::lock_guard lock(mutex_);
      encoder_ended_ = true;
      changed_.notify_all();
    });
    encoder_.SetOnError([this](int error, std::string_view message) noexcept {
      Capture([&] { throw ffmpeg::FfmpegException(error, message); });
    });
    remuxer_.SetOnError([this](std::string_view target, int error,
                               std::string_view message) noexcept {
      Capture([&] {
        throw ffmpeg::FfmpegException(error, std::string("Remuxer ") +
                                                 std::string(target) + ": " +
                                                 std::string(message));
      });
    });
  }

  ~Publishing() { Stop(); }

  void Run(const std::string& input_url, std::uint16_t port,
           const std::filesystem::path& stop_file) {
    input_.Start(input_url);
    for (;;) {
      if (std::filesystem::exists(stop_file)) {
        Stop();
        Rethrow();
        return;
      }
      std::unique_lock lock(mutex_);
      if (error_) break;
      if (input_ready_) {
        lock.unlock();
        Start(port);
        break;
      }
      changed_.wait_for(lock, 250ms);
    }
    for (;;) {
      if (std::filesystem::exists(stop_file)) break;
      std::unique_lock lock(mutex_);
      if (error_) break;
      changed_.wait_for(lock, 250ms);
    }
    Stop();
    Rethrow();
    fmt::print("STOPPED encoded_packets={}\n", packets_.load());
  }

 private:
  template <class Function>
  void Capture(Function&& function) noexcept {
    try {
      function();
    } catch (...) {
      std::lock_guard lock(mutex_);
      if (!error_) error_ = std::current_exception();
      changed_.notify_all();
    }
  }

  void Rethrow() {
    std::lock_guard lock(mutex_);
    if (error_) std::rethrow_exception(error_);
  }

  void Start(std::uint16_t port) {
    auto encoding_streams = streams_;
    for (auto& stream : encoding_streams) {
      auto* parameters = stream.codec_parameters.get();
      if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
        // Scheduler provides FLTP/48 kHz, retaining the source channel layout.
        parameters->format = AV_SAMPLE_FMT_FLTP;
        parameters->sample_rate = 48000;
      }
    }
    // The verified Tencent source is CPU YUV420P, accepted by libx264; the
    // Processor's existing default pass-through retains its layout and PTS.
    if (!processor_.Start(streams_, device_))
      throw std::runtime_error("Processor初始化失败");
    mw::streamer::EncoderConfig config;
    config.fps = {30, 1};
    config.gop_size = 60;
    config.video_encoder_name = "libx264";
    config.audio_encoder_name = "aac";
    config.video_options = {{"preset", "veryfast"}, {"tune", "zerolatency"}};
    encoder_.Start(config, encoding_streams, device_);
    encoder_started_ = true;
    Rethrow();
    remuxer_.Start(encoded_streams_);
    remuxer_.AddRtspPublish("live", "tencent", "0.0.0.0", port);
    if (!scheduler_.Start(streams_))
      throw std::runtime_error("Scheduler初始化失败");
    {
      std::lock_guard lock(mutex_);
      active_ = true;
      released_ = true;
    }
    changed_.notify_all();
    fmt::print("READY rtsp://127.0.0.1:{}/live/tencent\n", port);
    std::fflush(stdout);
  }

  void Stop() noexcept {
    if (stopped_) return;
    stopped_ = true;
    {
      std::lock_guard lock(mutex_);
      active_ = false;
      stopping_ = true;
    }
    changed_.notify_all();
    Capture([&] { input_.Stop(); });
    scheduler_.Stop();
    processor_.Stop();
    if (encoder_started_) {
      encoder_.Drain();
      {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 15s,
                               [&] { return encoder_ended_ || error_; }) &&
            !error_) {
          error_ =
              std::make_exception_ptr(std::runtime_error("Encoder排空超时"));
        }
      }
    }
    encoder_.Stop();
    remuxer_.Drain();
    remuxer_.Stop();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  std::exception_ptr error_;
  bool input_ready_ = false, released_ = false, stopping_ = false;
  bool encoder_started_ = false, encoder_ended_ = false, stopped_ = false;
  std::atomic<bool> active_{false};
  std::atomic<std::uint64_t> packets_{0};
  std::vector<ffmpeg::StreamInfo> streams_, encoded_streams_;
  ffmpeg::HwDeviceContext device_;
  mw::streamer::Remuxer remuxer_;
  mw::streamer::Encoder encoder_;
  mw::streamer::Processor processor_;
  mw::streamer::Scheduler scheduler_;
  mw::streamer::FfmpegInput input_;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    fmt::print(stderr, "Usage: {} input_url rtsp_port stop_file\n", argv[0]);
    return 2;
  }
  try {
    const auto port = std::stoi(argv[2]);
    if (port <= 0 || port > 65535)
      throw std::invalid_argument("RTSP端口必须在1至65535之间");
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
    Publishing publishing;
    publishing.Run(argv[1], static_cast<std::uint16_t>(port),
                   std::filesystem::u8path(argv[3]));
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "发布探针失败: {}\n", error.what());
    return 1;
  }
}
