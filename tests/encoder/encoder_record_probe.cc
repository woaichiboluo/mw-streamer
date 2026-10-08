#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "mw/streamer/encoder/encoder.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct CloseFormat {
  void operator()(AVFormatContext* format) const noexcept {
    if (format->pb) avio_closep(&format->pb);
    avformat_free_context(format);
  }
};

mw::streamer::SchedulerConfig SchedulerConfig(int fps) {
  mw::streamer::SchedulerConfig config;
  config.video_frame_rate = {fps, 1};
  return config;
}

mw::streamer::FfmpegInputConfig InputConfig() {
  mw::streamer::FfmpegInputConfig config;
  config.auto_reconnect = false;
  return config;
}

class Recording final {
 public:
  Recording(std::string path, const ffmpeg::HwDeviceContext& device, int fps)
      : path_(std::move(path)),
        device_(device),
        scheduler_(SchedulerConfig(fps)),
        input_(device, InputConfig()) {
    config_.fps = {fps, 1};
    config_.gop_size = fps * 2;
    config_.video_bit_rate = 30000000;
    config_.video_encoder_name = device.get() ? "hevc_nvenc" : "libx264";
    config_.video_options =
        device.get() ? std::map<std::string, std::string>{{"preset", "p2"}}
                     : std::map<std::string, std::string>{
                           {"preset", "ultrafast"}, {"tune", "zerolatency"}};
    encoder_.SetOnReady([this](const auto& streams) noexcept {
      Capture([&] { Open(streams); });
    });
    encoder_.SetOnPacket([this](const auto& packet, std::int64_t) noexcept {
      Capture([&] { Write(packet); });
    });
    encoder_.SetOnEnded([this]() noexcept {
      std::lock_guard lock(state_mutex_);
      encoded_ended_ = true;
      wake_.notify_all();
    });
    encoder_.SetOnError([this](int error, std::string_view message) noexcept {
      Capture([&] { throw ffmpeg::FfmpegException(error, message); });
    });
    scheduler_.SetOnVideo([this](const auto& frame) noexcept {
      Capture([&] {
        if (!encoder_.SubmitVideo(processor_.ProcessVideo(frame))) {
          throw std::runtime_error("编码器拒绝视频帧");
        }
        ++video_frames_;
      });
    });
    scheduler_.SetOnAudio([this](const auto& frame) noexcept {
      Capture([&] {
        if (!encoder_.SubmitAudio(processor_.ProcessAudio(frame))) {
          throw std::runtime_error("编码器拒绝音频帧");
        }
        ++audio_frames_;
      });
    });
    scheduler_.SetOnEnded([this]() noexcept {
      processor_.End();
      encoder_.Drain();
    });
    input_.SetOnReady([this](const auto& streams) noexcept {
      Capture([&] {
        auto encoding_streams = streams;
        for (auto& stream : encoding_streams) {
          auto* parameters = stream.codec_parameters.get();
          if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            parameters->format = AV_SAMPLE_FMT_FLTP;
            parameters->sample_rate = 48000;
          } else if (device_.get()) {
            const auto* description = av_pix_fmt_desc_get(
                static_cast<AVPixelFormat>(parameters->format));
            if (!description || description->nb_components != 3 ||
                description->log2_chroma_w != 1 ||
                description->log2_chroma_h != 1 ||
                (description->comp[0].depth != 8 &&
                 description->comp[0].depth != 10)) {
              throw std::runtime_error(
                  "探针只接受已知 CUDA 8/10-bit 4:2:0 布局");
            }
            parameters->format = description->comp[0].depth == 8
                                     ? AV_PIX_FMT_NV12
                                     : AV_PIX_FMT_P010;
          }
        }
        if (!processor_.Start(streams, device_)) {
          throw std::runtime_error("Processor 初始化失败");
        }
        encoder_.Start(config_, encoding_streams, device_);
        Rethrow();
        if (!scheduler_.Start(streams)) {
          throw std::runtime_error("Scheduler 初始化失败");
        }
        ready_.store(true);
      });
    });
    input_.SetOnFrame([this](int, const auto& frame) noexcept {
      Capture([&] {
        const bool accepted = frame->width > 0 ? scheduler_.SubmitVideo(frame)
                                               : scheduler_.SubmitAudio(frame);
        if (!accepted) throw std::runtime_error("Scheduler 拒绝解码帧");
      });
    });
    input_.SetOnStateChanged([this](mw::streamer::InputState state, int error,
                                    std::string_view message) noexcept {
      Capture([&] {
        if (state == mw::streamer::InputState::kFailed) {
          throw ffmpeg::FfmpegException(error, message);
        }
        if (state == mw::streamer::InputState::kEnded) {
          std::lock_guard lock(state_mutex_);
          input_ended_ = true;
          wake_.notify_all();
        }
      });
    });
  }

  ~Recording() {
    Stop();
    if (header_written_ && !trailer_attempted_) av_write_trailer(format_.get());
  }

  void Run(std::string_view url, int seconds) {
    input_.Start(url);
    const auto startup_deadline = Clock::now() + 60s;
    {
      std::unique_lock lock(state_mutex_);
      for (;;) {
        if (failed_.load() || input_ended_) break;
        const auto deadline =
            first_packet_at_ ? *first_packet_at_ + std::chrono::seconds(seconds)
                             : startup_deadline;
        if (Clock::now() >= deadline) {
          if (!first_packet_at_) {
            throw std::runtime_error("录制启动六十秒内未产生编码包");
          }
          break;
        }
        wake_.wait_until(lock, deadline);
      }
    }
    input_.Stop();
    if (ready_.load() && !failed_.load()) {
      scheduler_.Drain();
      std::unique_lock lock(state_mutex_);
      if (!wake_.wait_for(lock, 30s,
                          [&] { return encoded_ended_ || failed_.load(); })) {
        throw std::runtime_error("录制排空超时");
      }
    }
    Stop();
    Rethrow();
    if (!header_written_ || !encoded_ended_ ||
        video_packets_ + audio_packets_ == 0) {
      throw std::runtime_error("录制未成功初始化或排空");
    }
    trailer_attempted_ = true;
    ffmpeg::FfmpegException::throwIfError(av_write_trailer(format_.get()),
                                          "写入 MP4 尾部");
    fmt::print(
        "SUMMARY video_frames={} audio_frames={} video_packets={} "
        "audio_packets={} output={}\n",
        video_frames_.load(), audio_frames_.load(), video_packets_,
        audio_packets_, path_);
  }

 private:
  template <typename Function>
  void Capture(Function&& function) noexcept {
    if (failed_.load()) return;
    try {
      function();
    } catch (...) {
      std::lock_guard lock(state_mutex_);
      if (!error_) error_ = std::current_exception();
      failed_.store(true);
      wake_.notify_all();
    }
  }

  void Rethrow() {
    std::lock_guard lock(state_mutex_);
    if (error_) std::rethrow_exception(error_);
  }

  void Stop() noexcept {
    input_.Stop();
    scheduler_.Stop();
    processor_.Stop();
    encoder_.Stop();
  }

  void Open(const std::vector<ffmpeg::StreamInfo>& streams) {
    AVFormatContext* raw = nullptr;
    const int result =
        avformat_alloc_output_context2(&raw, nullptr, "mp4", path_.c_str());
    format_.reset(raw);
    ffmpeg::FfmpegException::throwIfError(result, "创建 MP4 输出");
    if (!format_) throw std::runtime_error("MP4 输出上下文为空");
    format_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    for (const auto& source : streams) {
      auto* stream = avformat_new_stream(format_.get(), nullptr);
      if (!stream) throw std::bad_alloc();
      ffmpeg::FfmpegException::throwIfError(
          avcodec_parameters_copy(stream->codecpar,
                                  source.codec_parameters.get()),
          "复制编码输出轨道");
      if (stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
      }
      stream->time_base = source.time_base;
      if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        stream->avg_frame_rate = source.codec_parameters.get()->framerate;
      }
      tracks_.emplace(source.stream_index, stream);
    }
    ffmpeg::FfmpegException::throwIfError(
        avio_open(&format_->pb, path_.c_str(), AVIO_FLAG_WRITE),
        "打开 MP4 文件");
    ffmpeg::FfmpegException::throwIfError(
        avformat_write_header(format_.get(), nullptr), "写入 MP4 文件头");
    header_written_ = true;
  }

  void Write(const ffmpeg::Packet& source) {
    std::lock_guard lock(mux_mutex_);
    auto* stream = tracks_.at(source->stream_index);
    auto packet = source.Ref();
    av_packet_rescale_ts(packet.get(), source->time_base, stream->time_base);
    packet->stream_index = stream->index;
    packet->time_base = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        av_interleaved_write_frame(format_.get(), packet.get()), "写入编码包");
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      ++video_packets_;
    } else {
      ++audio_packets_;
    }
    std::lock_guard state_lock(state_mutex_);
    if (!first_packet_at_) {
      first_packet_at_ = Clock::now();
      wake_.notify_all();
    }
  }

  std::string path_;
  ffmpeg::HwDeviceContext device_;
  std::mutex state_mutex_;
  std::mutex mux_mutex_;
  std::condition_variable wake_;
  std::exception_ptr error_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> ready_{false};
  bool input_ended_ = false;
  bool encoded_ended_ = false;
  std::optional<Clock::time_point> first_packet_at_;
  std::unique_ptr<AVFormatContext, CloseFormat> format_;
  std::map<int, AVStream*> tracks_;
  bool header_written_ = false;
  bool trailer_attempted_ = false;
  std::atomic<std::uint64_t> video_frames_{0};
  std::atomic<std::uint64_t> audio_frames_{0};
  std::uint64_t video_packets_ = 0;
  std::uint64_t audio_packets_ = 0;
  mw::streamer::EncoderConfig config_;
  mw::streamer::Encoder encoder_;
  mw::streamer::Processor processor_;
  mw::streamer::Scheduler scheduler_;
  mw::streamer::FfmpegInput input_;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 6) {
    fmt::print(stderr, "Usage: {} url output.mp4 seconds cpu|cuda fps\n",
               argv[0]);
    return 2;
  }
  try {
    const int seconds = std::stoi(argv[3]);
    const int fps = std::stoi(argv[5]);
    const std::string backend = argv[4];
    if (seconds <= 0 || fps <= 0 || fps > 240 ||
        (backend != "cpu" && backend != "cuda")) {
      throw std::invalid_argument("录制时长、帧率或设备类型无效");
    }
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 1;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
    const ffmpeg::HwDeviceContext device(backend == "cuda"
                                             ? ffmpeg::HwDeviceType::kCuda
                                             : ffmpeg::HwDeviceType::kCpu);
    Recording recording(argv[2], device, fps);
    recording.Run(argv[1], seconds);
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Recording failed: {}\n", error.what());
    return 1;
  }
}
