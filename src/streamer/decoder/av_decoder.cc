#include "mw/streamer/decoder/av_decoder.h"

#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/performance/latency.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

const AVCodec* FindDecoder(const ffmpeg::StreamInfo& stream,
                           const AvDecoderConfig& config,
                           const ffmpeg::HwDeviceContext* device) {
  stream.Validate();
  if (config.decoder_name.find('\0') != std::string::npos) {
    throw std::invalid_argument("AvDecoder 解码器名称不能包含空字符");
  }
  const auto* parameters = stream.codec_parameters.get();
  const auto id = parameters->codec_id;
  const AVCodec* codec = nullptr;
  if (!config.decoder_name.empty()) {
    codec = avcodec_find_decoder_by_name(config.decoder_name.c_str());
  } else {
    // Prefer libvpx for software WebM alpha. With a GPU device, select the
    // native decoder's hardware path.
    const bool hardware =
        device && device->get() && parameters->codec_type == AVMEDIA_TYPE_VIDEO;
    if (!hardware && id == AV_CODEC_ID_VP9) {
      codec = avcodec_find_decoder_by_name("libvpx-vp9");
    } else if (!hardware && id == AV_CODEC_ID_VP8) {
      codec = avcodec_find_decoder_by_name("libvpx");
    }
    if (!codec) codec = avcodec_find_decoder(id);
  }
  if (!codec) {
    throw ffmpeg::FfmpegException(AVERROR_DECODER_NOT_FOUND,
                                  "avcodec_find_decoder");
  }
  if (codec->id != id || codec->type != parameters->codec_type) {
    throw std::invalid_argument("AvDecoder 指定解码器与轨道编码不匹配");
  }
  return codec;
}

AVPixelFormat HardwareFormat(AVCodecContext*,
                             const AVPixelFormat* formats) noexcept {
  for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    if (*format == AV_PIX_FMT_CUDA) return *format;
  }
  return AV_PIX_FMT_NONE;
}

AVPixelFormat AlphaFormat(AVPixelFormat format) {
  switch (format) {
    case AV_PIX_FMT_YUV420P:
      return AV_PIX_FMT_YUVA420P;
    case AV_PIX_FMT_YUV422P:
      return AV_PIX_FMT_YUVA422P;
    case AV_PIX_FMT_YUV444P:
      return AV_PIX_FMT_YUVA444P;
    default:
      return format;
  }
}

}  // namespace

class AvDecoder::Impl final {
 public:
  Impl(const ffmpeg::StreamInfo& stream, AvDecoderConfig config,
       const ffmpeg::HwDeviceContext* device = nullptr)
      : stream_(stream),
        config_(std::move(config)),
        context_(FindDecoder(stream, config_, device)) {
    if (config_.packet_capacity == 0 || config_.frame_capacity == 0 ||
        config_.thread_count < 0) {
      throw std::invalid_argument("AvDecoder 缓存容量必须为正，线程数不能为负");
    }
    auto* context = context_.get();
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context, stream_.codec_parameters.get()),
        "avcodec_parameters_to_context");
    const bool video = context->codec_type == AVMEDIA_TYPE_VIDEO;
    latency_.Start(video ? "perf.decoder.video" : "perf.decoder.audio", this,
                   video ? "decode.video" : "decode.audio",
                   stream_.stream_index);
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
    if (latency_.enabled()) context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
#endif
    context->pkt_timebase = stream_.time_base;
    context->thread_count = config_.thread_count;
    if (context->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (stream_.frame_rate.num > 0 && stream_.frame_rate.den > 0) {
        context->framerate = stream_.frame_rate;
      }
      if (stream_.sample_aspect_ratio.num > 0 &&
          stream_.sample_aspect_ratio.den > 0) {
        context->sample_aspect_ratio = stream_.sample_aspect_ratio;
      }
      const auto alpha = stream_.metadata.find("alpha_mode");
      if (alpha != stream_.metadata.end() && !alpha->second.empty() &&
          alpha->second.front() == '1') {
        context->pix_fmt = AlphaFormat(context->pix_fmt);
      }
    }
    if (device && device->get() && context->codec_type == AVMEDIA_TYPE_VIDEO) {
      ConfigureDevice(*device);
    }
    if ((context->codec->capabilities & AV_CODEC_CAP_HARDWARE) &&
        !context->hw_device_ctx) {
      throw std::invalid_argument("AvDecoder 硬件解码器需要外部设备上下文");
    }
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(context, context->codec, nullptr), "avcodec_open2");
    if (!worker_.Start([this]() noexcept { Run(); })) {
      throw std::runtime_error("启动 AvDecoder 解码线程失败");
    }
  }

  ~Impl() { Stop(); }

  void SetOnError(OnError callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(callback);
  }

  void SetOnInputSpace(OnInputSpace callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_input_space_ = std::move(callback);
  }

  bool TryPushPacket(const ffmpeg::Packet& packet) {
    if (!packet.get() || !packet->data || packet->size <= 0 ||
        packet->stream_index != stream_.stream_index) {
      throw std::invalid_argument("AvDecoder Packet 为空或不属于当前轨道");
    }
    return TryPush(InputKind::kPacket, &packet);
  }

  bool TryPushEof() { return TryPush(InputKind::kEof); }

  bool TryReset() { return TryPush(InputKind::kReset); }

  AvDecoderReadResult TryReadFrame() {
    AvDecoderReadResult result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!frames_.empty() && !stopping_) {
        result = std::move(frames_.front());
        frames_.pop_front();
      } else {
        result.stream_index = stream_.stream_index;
        result.time_base = stream_.time_base;
      }
    }
    changed_.notify_all();
    return result;
  }

  std::size_t buffered_packets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_.size();
  }

  std::size_t buffered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }

  std::exception_ptr error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

  const ffmpeg::StreamInfo& stream_info() const { return stream_; }

  void RequestStop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
  }

  void Stop() noexcept {
    RequestStop();
    worker_.Join();
    latency_.Finish();
    std::lock_guard<std::mutex> lock(mutex_);
    packets_.clear();
    frames_.clear();
  }

 private:
  void ConfigureDevice(const ffmpeg::HwDeviceContext& device) {
    const auto* native =
        reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
    if (native->type != AV_HWDEVICE_TYPE_CUDA) {
      throw std::invalid_argument("AvDecoder 硬解只支持 CUDA 设备");
    }
    auto* context = context_.get();
    for (int index = 0;; ++index) {
      const auto* supported = avcodec_get_hw_config(context->codec, index);
      if (!supported) {
        throw std::invalid_argument("AvDecoder 指定解码器不支持 CUDA 设备");
      }
      if (supported->device_type == AV_HWDEVICE_TYPE_CUDA &&
          supported->pix_fmt == AV_PIX_FMT_CUDA &&
          (supported->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
        break;
      }
    }
    context->hw_device_ctx = av_buffer_ref(device.get());
    if (!context->hw_device_ctx) throw std::bad_alloc();
    context->get_format = HardwareFormat;
    context->thread_count =
        config_.thread_count == 0 ? 2 : config_.thread_count;
  }

  enum class InputKind { kPacket, kEof, kReset };

  struct Input {
    InputKind kind;
    std::optional<ffmpeg::Packet> packet;
  };

  bool TryPush(InputKind kind, const ffmpeg::Packet* packet = nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || error_) return false;
      if (submitted_eof_ && kind != InputKind::kReset) {
        throw std::logic_error(
            "AvDecoder 提交 EOF 后需要先 Reset 才能继续输入");
      }
      if (packets_.size() >= config_.packet_capacity) return false;
      packets_.push_back({kind, packet ? std::optional<ffmpeg::Packet>(*packet)
                                       : std::nullopt});
      submitted_eof_ = kind == InputKind::kEof;
    }
    changed_.notify_all();
    return true;
  }

  std::int64_t FrameEndPts(const ffmpeg::Frame& frame) const {
    if (frame->pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    auto duration = frame->duration;
    const auto* context = context_.get();
    if (duration <= 0) {
      if (context->codec_type == AVMEDIA_TYPE_VIDEO &&
          context->framerate.num > 0 && context->framerate.den > 0) {
        // No AVStream/parser is borrowed from the demux connection. Its nominal
        // rate provides the estimate when decoded duration is unavailable.
        duration =
            av_rescale_q(1, av_inv_q(context->framerate), stream_.time_base);
      } else if (context->codec_type == AVMEDIA_TYPE_AUDIO &&
                 context->sample_rate > 0) {
        duration =
            av_rescale_q(frame->nb_samples, AVRational{1, context->sample_rate},
                         stream_.time_base);
      }
    }
    if (duration <= 0 ||
        frame->pts > std::numeric_limits<std::int64_t>::max() - duration) {
      return AV_NOPTS_VALUE;
    }
    return frame->pts + duration;
  }

  bool Emit(AvDecoderReadResult result) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] {
      return stopping_ || frames_.size() < config_.frame_capacity;
    });
    if (stopping_) return false;
    frames_.push_back(std::move(result));
    return true;
  }

  void Run() noexcept {
    try {
      bool started = false;
      bool drained = false;
      std::int64_t next_pts = AV_NOPTS_VALUE;
      for (;;) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (stopping_) return;
        }
        // Receive buffered frames first. Only EAGAIN requests another packet.
        if (started && !drained) {
          ffmpeg::Frame frame;
          const int received =
              avcodec_receive_frame(context_.get(), frame.get());
          if (received == AVERROR_EOF) {
            if (!Emit({AvDecoderReadStatus::kEof,
                       {},
                       stream_.stream_index,
                       stream_.time_base,
                       next_pts})) {
              return;
            }
            drained = true;
            continue;
          }
          if (received != AVERROR(EAGAIN)) {
            ffmpeg::FfmpegException::throwIfError(received,
                                                  "avcodec_receive_frame");
            if (context_.get()->hw_device_ctx &&
                (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx)) {
              throw ffmpeg::FfmpegException(AVERROR_INVALIDDATA,
                                            "AvDecoder 硬解未交付硬件帧");
            }
            latency_.Complete(frame->opaque_ref);
            frame->pts = frame->best_effort_timestamp;
            frame->time_base = stream_.time_base;
            next_pts = FrameEndPts(frame);
            if (!Emit({AvDecoderReadStatus::kFrame, std::move(frame),
                       stream_.stream_index, stream_.time_base,
                       AV_NOPTS_VALUE})) {
              return;
            }
            continue;
          }
        }
        Input input;
        OnInputSpace on_input_space;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait(lock,
                        [this] { return stopping_ || !packets_.empty(); });
          if (stopping_) return;
          input = std::move(packets_.front());
          packets_.pop_front();
          on_input_space = on_input_space_;
        }
        if (on_input_space) {
          try {
            on_input_space();
          } catch (const std::exception& observer_error) {
            MW_LOG_ERROR("streamer", "AvDecoder 输入空间通知异常: {}",
                         observer_error.what());
          } catch (...) {
            MW_LOG_ERROR("streamer", "AvDecoder 输入空间通知发生未知异常");
          }
        }
        if (input.kind == InputKind::kReset) {
          context_.FlushBuffers();
          started = false;
          drained = false;
          next_pts = AV_NOPTS_VALUE;
          continue;
        }
        // EOF is ordered behind all submitted packets. After a drain the codec
        // waits for Reset, instead of closing its worker/context.
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
        if (input.packet) {
          latency_.Attach(input.packet->get()->opaque_ref, latency_.Begin());
        }
#endif
        ffmpeg::FfmpegException::throwIfError(
            avcodec_send_packet(context_.get(),
                                input.packet ? input.packet->get() : nullptr),
            "avcodec_send_packet");
        started = true;
      }
    } catch (...) {
      const auto failure = std::current_exception();
      OnError callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = failure;
        packets_.clear();
        callback = std::move(on_error_);
      }
      MW_LOG_ERROR("streamer", "AvDecoder 轨道 {} 解码失败",
                   stream_.stream_index);
      if (callback) {
        try {
          callback(failure);
        } catch (const std::exception& observer_error) {
          MW_LOG_ERROR("streamer", "AvDecoder 错误通知异常: {}",
                       observer_error.what());
        } catch (...) {
          MW_LOG_ERROR("streamer", "AvDecoder 错误通知发生未知异常");
        }
      }
    }
  }

  const ffmpeg::StreamInfo stream_;
  const AvDecoderConfig config_;
  OnError on_error_;
  OnInputSpace on_input_space_;
  ffmpeg::CodecContext context_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  bool submitted_eof_ = false;
  std::exception_ptr error_;
  std::deque<Input> packets_;
  std::deque<AvDecoderReadResult> frames_;
  internal::StageLatency latency_;
  internal::Thread worker_;
};

AvDecoder::AvDecoder(const ffmpeg::StreamInfo& stream, AvDecoderConfig config)
    : impl_(std::make_unique<Impl>(stream, std::move(config))) {}
AvDecoder::AvDecoder(const ffmpeg::StreamInfo& stream,
                     const ffmpeg::HwDeviceContext& device,
                     AvDecoderConfig config)
    : impl_(std::make_unique<Impl>(stream, std::move(config), &device)) {}
AvDecoder::~AvDecoder() = default;
void AvDecoder::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
void AvDecoder::SetOnInputSpace(OnInputSpace callback) {
  impl_->SetOnInputSpace(std::move(callback));
}
bool AvDecoder::TryPushPacket(const ffmpeg::Packet& packet) {
  return impl_->TryPushPacket(packet);
}
bool AvDecoder::TryPushEof() { return impl_->TryPushEof(); }
bool AvDecoder::TryReset() { return impl_->TryReset(); }
AvDecoderReadResult AvDecoder::TryReadFrame() { return impl_->TryReadFrame(); }
std::size_t AvDecoder::buffered_packets() const {
  return impl_->buffered_packets();
}
std::size_t AvDecoder::buffered_frames() const {
  return impl_->buffered_frames();
}
std::exception_ptr AvDecoder::error() const { return impl_->error(); }
const ffmpeg::StreamInfo& AvDecoder::stream_info() const {
  return impl_->stream_info();
}
void AvDecoder::RequestStop() noexcept { impl_->RequestStop(); }
void AvDecoder::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
