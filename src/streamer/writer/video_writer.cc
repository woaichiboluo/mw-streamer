#include "mw/streamer/writer/video_writer.h"

#include <array>
#include <atomic>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/encoder/av_encoder.h"
#include "mw/streamer/filter/audio_resampler.h"
#include "mw/streamer/platform/wait_signal.h"
#include "mw/streamer/platform/write_signal.h"
#include "streamer/recorder/mp4_file_writer.h"

namespace mw::streamer {
namespace {

std::int64_t SubtractTime(std::int64_t value, std::int64_t offset) {
  if (value == AV_NOPTS_VALUE) return value;
  if ((offset > 0 &&
       value <= std::numeric_limits<std::int64_t>::min() + offset) ||
      (offset < 0 && value > std::numeric_limits<std::int64_t>::max() + offset))
    throw std::overflow_error("VideoWriter 时间戳平移溢出");
  return value - offset;
}

}  // namespace

class VideoWriter::Impl final {
 public:
  explicit Impl(const ffmpeg::HwDeviceContext& device) : encoder_(device) {
    encoder_.SetWriteSignal(true, video_space_);
    encoder_.SetWriteSignal(false, audio_space_);
    encoder_.SetOnError([this](std::exception_ptr error) { Fail(error); });
    encoder_.SetOnPacket([this](const ffmpeg::Packet& packet) {
      std::lock_guard lock(mutex_);
      CheckFailureLocked();
      if (!origin_) {
        pending_.push_back(packet.Ref());
      } else {
        WritePacket(packet);
      }
    });
  }

  ~Impl() {
    RequestStop();
    encoder_.Stop();
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void Open(std::string_view path, const AvEncoderConfig& config) {
    if (opened_) throw std::logic_error("VideoWriter 仅允许 Open 一次");
    opened_ = true;
    try {
      CheckFailure();
      const std::string owned_path(path);
      if (path.empty() || path.find('\0') != std::string_view::npos ||
          path.find("://") != std::string_view::npos ||
          std::filesystem::u8path(owned_path).extension() != ".mp4")
        throw std::invalid_argument("VideoWriter 需要本地 .mp4 文件路径");
      enabled_ = {config.video_pixel_format != AV_PIX_FMT_NONE,
                  config.audio_sample_format != AV_SAMPLE_FMT_NONE};
      if (!encoder_.Start(config)) {
        if (const auto error = encoder_.error()) std::rethrow_exception(error);
        throw std::runtime_error("VideoWriter 编码器启动失败");
      }
      streams_ = encoder_.streams();
      if (enabled_[1]) {
        AudioResamplerConfig audio;
        audio.sample_rate = config.audio_sample_rate;
        audio.sample_format = config.audio_sample_format;
        audio.channel_layout = config.audio_channel_layout;
        audio_ = std::make_unique<AudioResampler>(audio);
      }
      file_ = std::make_unique<internal::Mp4FileWriter>(streams_, owned_path,
                                                        false);
      CheckFailure();
      active_ = true;
      MW_LOG_INFO("streamer", "VideoWriter 已打开: path={}", owned_path);
    } catch (...) {
      Fail(std::current_exception());
      encoder_.Stop();
      file_.reset();
      throw;
    }
  }

  void Write(const ffmpeg::Frame& frame, bool video) {
    CheckActive();
    const std::size_t index = video ? 0 : 1;
    try {
      if (!enabled_[index])
        throw std::invalid_argument("VideoWriter 输入轨道未启用");
      if (!frame.get() || !frame->data[0] || frame->pts == AV_NOPTS_VALUE ||
          frame->time_base.num <= 0 || frame->time_base.den <= 0 ||
          frame->duration < 0)
        throw std::invalid_argument("VideoWriter 输入帧或时间戳无效");
      const VideoWriterSourceTime time{frame->pts, frame->time_base};
      if (last_[index] &&
          av_compare_ts(time.pts, time.time_base, last_[index]->pts,
                        last_[index]->time_base) <= 0)
        throw std::invalid_argument("VideoWriter 每轨来源 PTS 必须严格递增");
      if (video) {
        PushEncoder(frame, true);
      } else {
        AvDecoderReadResult input;
        input.status = AvDecoderReadStatus::kFrame;
        input.frame = frame.Ref();
        input.stream_index = 1;
        input.time_base = time.time_base;
        if (!audio_->TryPushFrame(input))
          throw std::logic_error("VideoWriter 音频转换输入未排空");
      }
      last_[index] = time;
      SetFirst(index, time);
      if (!video) DrainAudio();
      CheckFailure();
    } catch (...) {
      Fail(std::current_exception());
      throw;
    }
  }

  void Finish() {
    if (finished_) return;
    CheckActive();
    try {
      {
        std::lock_guard lock(mutex_);
        if (!origin_)
          throw std::runtime_error("VideoWriter 缺少启用轨道的首帧");
      }
      if (audio_) {
        AvDecoderReadResult eof;
        eof.status = AvDecoderReadStatus::kEof;
        eof.stream_index = 1;
        eof.time_base = last_[1]->time_base;
        if (!audio_->TryPushFrame(eof))
          throw std::logic_error("VideoWriter 音频 EOF 未受理");
        DrainAudio();
      }
      // No mux lock while joining: encoder packet callbacks need it.
      encoder_.Stop();
      if (const auto error = encoder_.error()) std::rethrow_exception(error);
      CheckFailure();
      file_->Finish();
      file_.reset();
      active_ = false;
      finished_ = true;
      MW_LOG_INFO("streamer", "VideoWriter 文件写入完成");
    } catch (...) {
      Fail(std::current_exception());
      encoder_.Stop();
      file_.reset();
      active_ = false;
      throw;
    }
  }

  void RequestStop() noexcept {
    cancelled_.store(true, std::memory_order_release);
    signal_->Notify();
  }

  std::optional<VideoWriterSourceTime> source_start_time() const {
    std::lock_guard lock(mutex_);
    return origin_;
  }

 private:
  void CheckFailureLocked() const {
    if (error_) std::rethrow_exception(error_);
    if (cancelled_.load(std::memory_order_acquire))
      throw std::runtime_error("VideoWriter 已取消");
  }

  void CheckFailure() const {
    std::lock_guard lock(mutex_);
    CheckFailureLocked();
  }

  void CheckActive() const {
    CheckFailure();
    if (!active_) throw std::logic_error("VideoWriter 尚未 Open 或已经结束");
  }

  void Fail(std::exception_ptr error) {
    {
      std::lock_guard lock(mutex_);
      if (!error_) error_ = error;
    }
    signal_->Notify();
  }

  void PushEncoder(const ffmpeg::Frame& frame, bool video) {
    for (;;) {
      const auto observed = signal_->GetRevision();
      CheckFailure();
      if (video ? encoder_.TryPushVideo(frame) : encoder_.TryPushAudio(frame))
        return;
      CheckFailure();
      signal_->Wait(observed);
    }
  }

  void DrainAudio() {
    for (;;) {
      auto output = audio_->TryReadFrame();
      if (output.status != AvDecoderReadStatus::kFrame) return;
      PushEncoder(*output.frame, false);
    }
  }

  void SetFirst(std::size_t index, VideoWriterSourceTime time) {
    std::lock_guard lock(mutex_);
    CheckFailureLocked();
    if (first_[index]) return;
    first_[index] = time;
    if ((enabled_[0] && !first_[0]) || (enabled_[1] && !first_[1])) return;
    origin_ = first_[0] ? first_[0] : first_[1];
    if (first_[1] && av_compare_ts(first_[1]->pts, first_[1]->time_base,
                                   origin_->pts, origin_->time_base) < 0)
      origin_ = first_[1];
    for (const auto& packet : pending_) WritePacket(packet);
    pending_.clear();
  }

  void WritePacket(const ffmpeg::Packet& packet) {
    CheckFailureLocked();
    auto owned = packet.Ref();
    // Translate in encoder units before the file muxer rescales to its own
    // time base. Keep native reorder delay and audio padding/side data.
    const auto offset =
        av_rescale_q(origin_->pts, origin_->time_base, owned->time_base);
    if (offset == AV_NOPTS_VALUE ||
        offset == std::numeric_limits<std::int64_t>::max())
      throw std::overflow_error("VideoWriter 起点换算溢出");
    owned->pts = SubtractTime(owned->pts, offset);
    owned->dts = SubtractTime(owned->dts, offset);
    file_->Write(std::move(owned));
  }

  const std::shared_ptr<internal::WaitSignal> signal_ =
      std::make_shared<internal::WaitSignal>();
  const std::shared_ptr<internal::WriteSignal> video_space_ =
      std::make_shared<internal::WriteSignal>(signal_);
  const std::shared_ptr<internal::WriteSignal> audio_space_ =
      std::make_shared<internal::WriteSignal>(signal_);
  mutable std::mutex mutex_;
  std::atomic<bool> cancelled_{false};
  std::exception_ptr error_;
  std::array<bool, 2> enabled_{};
  std::array<std::optional<VideoWriterSourceTime>, 2> first_{};
  std::array<std::optional<VideoWriterSourceTime>, 2> last_{};
  std::optional<VideoWriterSourceTime> origin_;
  std::deque<ffmpeg::Packet> pending_;
  std::vector<ffmpeg::StreamInfo> streams_;
  std::unique_ptr<AudioResampler> audio_;
  std::unique_ptr<internal::Mp4FileWriter> file_;
  // Must finish workers before releasing anything their callbacks reference.
  AvEncoder encoder_;
  bool opened_ = false;
  bool active_ = false;
  bool finished_ = false;
};

VideoWriter::VideoWriter(const ffmpeg::HwDeviceContext& device)
    : impl_(std::make_unique<Impl>(device)) {}
VideoWriter::~VideoWriter() = default;
void VideoWriter::Open(std::string_view path, const AvEncoderConfig& config) {
  impl_->Open(path, config);
}
void VideoWriter::WriteVideo(const ffmpeg::Frame& frame) {
  impl_->Write(frame, true);
}
void VideoWriter::WriteAudio(const ffmpeg::Frame& frame) {
  impl_->Write(frame, false);
}
void VideoWriter::Finish() { impl_->Finish(); }
void VideoWriter::RequestStop() noexcept { impl_->RequestStop(); }
std::optional<VideoWriterSourceTime> VideoWriter::source_start_time() const {
  return impl_->source_start_time();
}

}  // namespace mw::streamer
