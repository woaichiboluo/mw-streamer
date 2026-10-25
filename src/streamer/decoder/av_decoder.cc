#include "mw/streamer/decoder/av_decoder.h"

#include <chrono>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/performance/latency.h"
#include "mw/streamer/platform/blocking_queue.h"
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
        context_(FindDecoder(stream, config_, device)),
        packets_(config_.packet_capacity),
        frames_(config_.frame_capacity) {
    if (config_.packet_capacity == 0 || config_.frame_capacity == 0 ||
        config_.thread_count < 0) {
      throw std::invalid_argument("AvDecoder 缓存容量必须为正，线程数不能为负");
    }
    auto* context = context_.get();
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context, stream_.codec_parameters.get()),
        "avcodec_parameters_to_context");
    const bool video = context->codec_type == AVMEDIA_TYPE_VIDEO;
    latency_.Start(video ? PerformanceStage::kDecoderVideo
                         : PerformanceStage::kDecoderAudio,
                   this, stream_.stream_index);
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
    MW_LOG_DEBUG(
        "streamer",
        "AvDecoder 配置: instance={} stream={} codec={} type={} "
        "time_base={}/{} threads={} packet_capacity={} frame_capacity={} "
        "hardware={}",
        fmt::ptr(this), stream_.stream_index, context->codec->name,
        av_get_media_type_string(context->codec_type), stream_.time_base.num,
        stream_.time_base.den, context->thread_count, config_.packet_capacity,
        config_.frame_capacity, context->hw_device_ctx != nullptr);
    if (!worker_.Start([this]() noexcept { Run(); })) {
      throw std::runtime_error("启动 AvDecoder 解码线程失败");
    }
    MW_LOG_INFO("streamer", "AvDecoder 已启动: instance={} stream={} codec={}",
                fmt::ptr(this), stream_.stream_index, context->codec->name);
  }

  ~Impl() { Stop(); }

  void SetOnError(OnError callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(callback);
  }

  void SetReadSignal(std::weak_ptr<internal::WaitSignal> signal) {
    frames_.SetReadSignal(std::move(signal));
  }

  bool PushPacket(const ffmpeg::Packet& packet, bool wait = true) {
    if (!packet.get() || !packet->data || packet->size <= 0 ||
        packet->stream_index != stream_.stream_index) {
      throw std::invalid_argument("AvDecoder Packet 为空或不属于当前轨道");
    }
    return Push(InputKind::kPacket, &packet, wait);
  }

  bool PushEof(bool wait = true) {
    return Push(InputKind::kEof, nullptr, wait);
  }

  bool Reset(bool wait = true) {
    return Push(InputKind::kReset, nullptr, wait);
  }

  AvDecoderReadResult ReadFrame(bool wait) {
    AvDecoderReadResult result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result.stream_index = stream_.stream_index;
      result.time_base = stream_.time_base;
      if (stopping_) return result;
    }
    const auto read = wait ? frames_.Pop(result) : frames_.TryPop(result);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return {AvDecoderReadStatus::kNull,
                {},
                stream_.stream_index,
                stream_.time_base,
                AV_NOPTS_VALUE};
      }
      if (read == internal::BlockingQueueResult::kSuccess) {
        MW_LOG_TRACE(
            "streamer",
            "AvDecoder 消费输出: instance={} stream={} kind={} pts={} "
            "duration={} time_base={}/{} queue={}/{}",
            fmt::ptr(this), stream_.stream_index,
            result.status == AvDecoderReadStatus::kFrame ? "frame" : "eof",
            result.frame ? result.frame->get()->pts : result.end_pts,
            result.frame ? result.frame->get()->duration : 0,
            stream_.time_base.num, stream_.time_base.den, frames_.Size(),
            config_.frame_capacity);
      }
    }
    return result;
  }

  std::size_t buffered_packets() const { return packets_.Size(); }

  std::size_t buffered_frames() const { return frames_.Size(); }

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
    packets_.Close();
    frames_.Close();
  }

  void Stop() noexcept {
    const bool had_worker = worker_.joinable();
    RequestStop();
    worker_.Join();
    latency_.Finish();
    DiscardInput();
    AvDecoderReadResult discarded;
    while (frames_.TryPop(discarded) ==
           internal::BlockingQueueResult::kSuccess) {
      discarded = {};
    }
    if (had_worker) {
      MW_LOG_INFO("streamer", "AvDecoder 已停止: instance={} stream={}",
                  fmt::ptr(this), stream_.stream_index);
    }
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
    InputKind kind = InputKind::kPacket;
    std::optional<ffmpeg::Packet> packet;
  };

  bool Push(InputKind kind, const ffmpeg::Packet* packet, bool wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_ || error_) return false;
    if (submitted_eof_ && kind != InputKind::kReset) {
      throw std::logic_error("AvDecoder 提交 EOF 后需要先 Reset 才能继续输入");
    }
    if (!wait && packets_.Size() >= config_.packet_capacity) {
      MW_LOG_TRACE("streamer",
                   "AvDecoder 输入队列满: instance={} stream={} queue={}/{}",
                   fmt::ptr(this), stream_.stream_index, packets_.Size(),
                   config_.packet_capacity);
      return false;
    }
    Input input{kind,
                packet ? std::optional<ffmpeg::Packet>(*packet) : std::nullopt};
    internal::BlockingQueueResult result;
    if (wait) {
      lock.unlock();
      result = packets_.Push(std::move(input));
      lock.lock();
    } else {
      // A single producer owns admission; only the consumer can reduce the
      // size after the capacity check. Stop/failure use the state lock first.
      result = packets_.Push(std::move(input),
                             std::chrono::steady_clock::duration::zero());
    }
    if (result != internal::BlockingQueueResult::kSuccess) return false;
    submitted_eof_ = kind == InputKind::kEof;
    if (packet) {
      MW_LOG_TRACE("streamer",
                   "AvDecoder 包入队: instance={} stream={} pts={} dts={} "
                   "duration={} time_base={}/{} bytes={} queue={}/{}",
                   fmt::ptr(this), stream_.stream_index, (*packet)->pts,
                   (*packet)->dts, (*packet)->duration, stream_.time_base.num,
                   stream_.time_base.den, (*packet)->size, packets_.Size(),
                   config_.packet_capacity);
    } else {
      MW_LOG_DEBUG(
          "streamer",
          "AvDecoder 控制入队: instance={} stream={} kind={} queue={}/{}",
          fmt::ptr(this), stream_.stream_index,
          kind == InputKind::kEof ? "eof" : "reset", packets_.Size(),
          config_.packet_capacity);
    }
    return true;
  }

  void DiscardInput() {
    Input discarded;
    while (packets_.TryPop(discarded) ==
           internal::BlockingQueueResult::kSuccess) {
      discarded = {};
    }
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
    MW_LOG_TRACE("streamer",
                 "AvDecoder 提交输出: instance={} stream={} kind={} pts={} "
                 "duration={} time_base={}/{} queue={}/{}",
                 fmt::ptr(this), stream_.stream_index,
                 result.status == AvDecoderReadStatus::kFrame ? "frame" : "eof",
                 result.frame ? result.frame->get()->pts : result.end_pts,
                 result.frame ? result.frame->get()->duration : 0,
                 stream_.time_base.num, stream_.time_base.den, frames_.Size(),
                 config_.frame_capacity);
    return frames_.Push(std::move(result)) ==
           internal::BlockingQueueResult::kSuccess;
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
            MW_LOG_DEBUG("streamer",
                         "AvDecoder 排空完成: instance={} stream={} "
                         "next_pts={} time_base={}/{}",
                         fmt::ptr(this), stream_.stream_index, next_pts,
                         stream_.time_base.num, stream_.time_base.den);
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
        if (packets_.Pop(input) != internal::BlockingQueueResult::kSuccess)
          return;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (stopping_) return;
        }
        if (input.kind == InputKind::kReset) {
          MW_LOG_DEBUG("streamer",
                       "AvDecoder 执行 Reset: instance={} stream={}",
                       fmt::ptr(this), stream_.stream_index);
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
        if (input.packet) {
          MW_LOG_TRACE(
              "streamer",
              "AvDecoder 提交 codec: instance={} stream={} pts={} dts={} "
              "duration={} time_base={}/{} bytes={}",
              fmt::ptr(this), stream_.stream_index, (*input.packet)->pts,
              (*input.packet)->dts, (*input.packet)->duration,
              stream_.time_base.num, stream_.time_base.den,
              (*input.packet)->size);
        } else {
          MW_LOG_DEBUG("streamer", "AvDecoder 提交 EOF: instance={} stream={}",
                       fmt::ptr(this), stream_.stream_index);
        }
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
        callback = std::move(on_error_);
      }
      packets_.Close();
      frames_.Close();
      DiscardInput();
      try {
        std::rethrow_exception(failure);
      } catch (const ffmpeg::FfmpegException& error) {
        MW_LOG_ERROR(
            "streamer",
            "AvDecoder 解码失败: instance={} stream={} code={} error={}",
            fmt::ptr(this), stream_.stream_index, error.error_code(),
            error.what());
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer",
                     "AvDecoder 解码失败: instance={} stream={} error={}",
                     fmt::ptr(this), stream_.stream_index, error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer",
                     "AvDecoder 解码失败: instance={} stream={} 未知异常",
                     fmt::ptr(this), stream_.stream_index);
      }
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
  ffmpeg::CodecContext context_;
  mutable std::mutex mutex_;
  bool stopping_ = false;
  bool submitted_eof_ = false;
  std::exception_ptr error_;
  internal::BlockingQueue<Input> packets_;
  internal::BlockingQueue<AvDecoderReadResult> frames_;
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
void AvDecoder::SetReadSignal(std::weak_ptr<internal::WaitSignal> signal) {
  impl_->SetReadSignal(std::move(signal));
}
bool AvDecoder::PushPacket(const ffmpeg::Packet& packet) {
  return impl_->PushPacket(packet);
}
bool AvDecoder::PushEof() { return impl_->PushEof(); }
bool AvDecoder::Reset() { return impl_->Reset(); }
bool AvDecoder::TryPushPacket(const ffmpeg::Packet& packet) {
  return impl_->PushPacket(packet, false);
}
bool AvDecoder::TryPushEof() { return impl_->PushEof(false); }
bool AvDecoder::TryReset() { return impl_->Reset(false); }
AvDecoderReadResult AvDecoder::ReadFrame() { return impl_->ReadFrame(true); }
AvDecoderReadResult AvDecoder::TryReadFrame() {
  return impl_->ReadFrame(false);
}
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
