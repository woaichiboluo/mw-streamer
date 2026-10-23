#include "mw/streamer/encoder/av_encoder.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/audio_frame_adapter.h"
#include "mw/streamer/ffmpeg/av_dictionary.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/performance/latency.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

constexpr std::size_t kFrameCapacity = 3;
constexpr int kVideoIndex = 0;
constexpr int kAudioIndex = 1;

void ValidateTiming(const ffmpeg::Frame& frame) {
  if (frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
      frame->time_base.den <= 0 || frame->duration < 0) {
    throw std::invalid_argument("编码输入需要有效的 PTS、时间基和非负时长");
  }
}

ffmpeg::Frame InTimeBase(const ffmpeg::Frame& frame, AVRational time_base) {
  auto result = frame.Ref();
  result->pts = av_rescale_q(frame->pts, frame->time_base, time_base);
  result->duration = av_rescale_q(frame->duration, frame->time_base, time_base);
  result->time_base = time_base;
  if (result->pts == AV_NOPTS_VALUE ||
      result->pts == std::numeric_limits<std::int64_t>::max() ||
      result->duration < 0 ||
      result->duration == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("编码输入时间基换算溢出");
  }
  return result;
}

bool ReservedOption(std::string_view key) {
  constexpr std::string_view reserved[] = {
      "b",           "bit_rate",     "g",
      "gop_size",    "bf",           "max_b_frames",
      "maxrate",     "rc_max_rate",  "minrate",
      "rc_min_rate", "bufsize",      "rc_buffer_size",
      "rc",          "cbr",          "framerate",
      "r",           "time_base",    "video_size",
      "s",           "width",        "height",
      "pix_fmt",     "pixel_format", "sample_fmt",
      "sample_rate", "ar",           "channel_layout",
      "ac"};
  return std::find(std::begin(reserved), std::end(reserved), key) !=
         std::end(reserved);
}

void CopyOptions(const std::map<std::string, std::string>& source,
                 ffmpeg::AvDictionary& target) {
  for (const auto& [key, value] : source) {
    if (ReservedOption(key)) {
      throw std::invalid_argument("扩展选项不能覆盖正式编码字段: " + key);
    }
    target.Set(key.c_str(), value);
  }
}

bool IsNvenc(std::string_view name) {
  return name.size() >= 6 && name.substr(name.size() - 6) == "_nvenc";
}

void ConfigureRateControl(const AvEncoderConfig& config,
                          ffmpeg::AvDictionary& options) {
  const auto& name = config.video_encoder_name;
  const bool cbr = config.rate_control == RateControl::kCbr;
  if (config.rate_control != RateControl::kCbr &&
      config.rate_control != RateControl::kVbr) {
    throw std::invalid_argument("未知码率控制模式");
  }
  if (config.video_bit_rate <= 0 || config.max_bit_rate < 0 ||
      (config.max_bit_rate > 0 &&
       (config.max_bit_rate < config.video_bit_rate ||
        (cbr && config.max_bit_rate != config.video_bit_rate)))) {
    throw std::invalid_argument("视频目标码率与峰值码率不兼容");
  }
  const auto maximum = cbr ? config.video_bit_rate : config.max_bit_rate;
  options.Set("b", std::to_string(config.video_bit_rate));
  options.Set("maxrate", std::to_string(maximum));
  if (cbr) options.Set("minrate", std::to_string(config.video_bit_rate));
  if (maximum > 0) {
    if (maximum > std::numeric_limits<int>::max() / 2) {
      throw std::invalid_argument("两秒码率缓冲超过 FFmpeg 支持范围");
    }
    options.Set("bufsize", std::to_string(maximum * 2));
  }
  if (IsNvenc(name)) {
    options.Set("rc", cbr ? "cbr" : "vbr");
  } else if (cbr && name == "libx264") {
    options.Set("nal-hrd", "cbr");
  } else if (cbr && name == "libx265") {
    std::string parameters;
    const auto supplied = config.video_options.find("x265-params");
    if (supplied != config.video_options.end() && !supplied->second.empty()) {
      parameters = supplied->second + ":";
    }
    options.Set("x265-params", parameters + "strict-cbr=1:hrd=1");
  }
}

void ValidatePixelFormat(const AVCodecContext& context,
                         AVPixelFormat pixel_format,
                         bool hardware_layout = false) {
  const void* configurations = nullptr;
  int count = 0;
  ffmpeg::FfmpegException::throwIfError(
      avcodec_get_supported_config(&context, nullptr,
                                   AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   &configurations, &count),
      "查询编码器像素格式");
  if (!configurations) return;
  const auto* formats = static_cast<const AVPixelFormat*>(configurations);
  bool accepts_software = false;
  for (int index = 0; index < count; ++index) {
    if (formats[index] == pixel_format) return;
    const auto* description = av_pix_fmt_desc_get(formats[index]);
    accepts_software |=
        description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL);
  }
  // Hardware-only encoders expose surface formats here; their software
  // layouts are validated by the device and hardware frame pool instead.
  if (hardware_layout && !accepts_software) return;
  throw std::invalid_argument("当前编码器不支持指定像素格式");
}

void ConfigureHardware(AVCodecContext& context,
                       const ffmpeg::HwDeviceContext& device,
                       AVPixelFormat sw_format) {
  auto* codec_context = &context;
  codec_context->pix_fmt = sw_format;
  codec_context->sw_pix_fmt = sw_format;
  const auto* codec = codec_context->codec;
  // A software encoder keeps CPU storage even when a shared GPU exists.
  if (!(codec->capabilities & AV_CODEC_CAP_HARDWARE) &&
      !avcodec_get_hw_config(codec, 0)) {
    ValidatePixelFormat(*codec_context, sw_format);
    return;
  }
  if (!device.get()) {
    throw std::invalid_argument("硬件编码器需要外部硬件设备上下文");
  }
  const auto* native =
      reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
  const AVCodecHWConfig* hardware = nullptr;
  for (int index = 0;; ++index) {
    const auto* supported = avcodec_get_hw_config(codec, index);
    if (!supported) break;
    const auto* description = av_pix_fmt_desc_get(supported->pix_fmt);
    if (supported->device_type == native->type && description &&
        (description->flags & AV_PIX_FMT_FLAG_HWACCEL) &&
        (supported->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)) {
      hardware = supported;
      break;
    }
  }
  if (!hardware) {
    throw std::invalid_argument("指定编码器不支持共享设备的硬件帧编码");
  }
  ValidatePixelFormat(*codec_context, sw_format, true);
  ValidatePixelFormat(*codec_context, hardware->pix_fmt);
  codec_context->hw_device_ctx = av_buffer_ref(device.get());
  if (!codec_context->hw_device_ctx) throw std::bad_alloc();
  const auto free_constraints = [](AVHWFramesConstraints* value) {
    av_hwframe_constraints_free(&value);
  };
  const std::unique_ptr<AVHWFramesConstraints, decltype(free_constraints)>
      constraints(av_hwdevice_get_hwframe_constraints(
                      codec_context->hw_device_ctx, nullptr),
                  free_constraints);
  if (constraints && constraints->valid_sw_formats) {
    bool supported = false;
    for (const auto* format = constraints->valid_sw_formats;
         *format != AV_PIX_FMT_NONE; ++format) {
      supported |= *format == sw_format;
    }
    if (!supported) {
      throw std::invalid_argument("共享编码设备不支持指定像素布局");
    }
  }
  codec_context->pix_fmt = hardware->pix_fmt;
  codec_context->sw_pix_fmt = sw_format;
  codec_context->hw_frames_ctx =
      av_hwframe_ctx_alloc(codec_context->hw_device_ctx);
  if (!codec_context->hw_frames_ctx) throw std::bad_alloc();
  auto* frames =
      reinterpret_cast<AVHWFramesContext*>(codec_context->hw_frames_ctx->data);
  frames->format = hardware->pix_fmt;
  frames->sw_format = sw_format;
  frames->width = context.width;
  frames->height = context.height;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_ctx_init(codec_context->hw_frames_ctx),
      "初始化硬件编码帧描述");
}

ffmpeg::Frame PrepareVideoFrame(const AVCodecContext& context,
                                const ffmpeg::Frame& source) {
  const auto* codec = &context;
  const auto* frame = source.get();
  if (!frame || !frame->data[0] || frame->width != codec->width ||
      frame->height != codec->height) {
    throw std::invalid_argument("视频搬运帧数据或尺寸不匹配");
  }
  const auto* target = codec->hw_frames_ctx
                           ? reinterpret_cast<const AVHWFramesContext*>(
                                 codec->hw_frames_ctx->data)
                           : nullptr;
  const auto* origin = frame->hw_frames_ctx
                           ? reinterpret_cast<const AVHWFramesContext*>(
                                 frame->hw_frames_ctx->data)
                           : nullptr;
  const auto layout = target ? target->sw_format : codec->pix_fmt;
  if ((origin ? origin->sw_format : frame->format) != layout ||
      (origin && frame->format != origin->format)) {
    throw std::invalid_argument("视频搬运像素布局不匹配");
  }
  if (!target && !origin) return source.Ref();
  if (target && origin) {
    if (origin->format != target->format || !origin->device_ref ||
        origin->device_ref->data != codec->hw_device_ctx->data) {
      throw std::invalid_argument("GPU 视频帧不属于共享编码设备");
    }
    return source.Ref();
  }
  ffmpeg::Frame result;
  if (target) {
    ffmpeg::FfmpegException::throwIfError(
        av_hwframe_get_buffer(codec->hw_frames_ctx, result.get(), 0),
        "分配上传 GPU 帧");
  } else {
    result->format = layout;
  }
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_transfer_data(result.get(), frame, 0), "搬运 CPU/GPU 视频帧");
  result.CopyPropertiesFrom(source);
  return result;
}

const AVCodec* FindEncoder(const std::string& name, AVMediaType type) {
  const auto* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec)
    throw ffmpeg::FfmpegException(AVERROR_ENCODER_NOT_FOUND,
                                  "查找编码器 " + name);
  if (codec->type != type)
    throw std::invalid_argument("编码器类型与输出轨道不匹配");
  return codec;
}

void OpenCodec(AVCodecContext& context, ffmpeg::AvDictionary& options,
               bool measure_latency) {
#if defined(AV_CODEC_FLAG_COPY_OPAQUE) && \
    defined(AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE)
  if (measure_latency &&
      (context.codec->capabilities & AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE)) {
    context.flags |= AV_CODEC_FLAG_COPY_OPAQUE;
  }
#else
  (void)measure_latency;
#endif
  // Preserve independent parameter sets for downstream track initialization.
  context.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (context.codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    context.thread_type = FF_THREAD_SLICE;
  if (IsNvenc(context.codec->name) && !options.Get("preset"))
    options.Set("preset", "p2");
  ffmpeg::FfmpegException::throwIfError(
      avcodec_open2(&context, context.codec, options.address()),
      "avcodec_open2");
  if (const auto* unused =
          av_dict_get(options.get(), "", nullptr, AV_DICT_IGNORE_SUFFIX))
    throw std::invalid_argument(std::string("未识别的编码器选项: ") +
                                unused->key);
}

std::unique_ptr<ffmpeg::CodecContext> OpenVideo(
    const AvEncoderConfig& config, const ffmpeg::HwDeviceContext& device,
    bool measure_latency) {
  const auto* pixel = av_pix_fmt_desc_get(config.video_pixel_format);
  if (config.width <= 0 || config.height <= 0 || !pixel ||
      (pixel->flags & AV_PIX_FMT_FLAG_HWACCEL) || config.gop_size < 0 ||
      config.max_b_frames < 0)
    throw std::invalid_argument("视频编码格式或 GOP 配置无效");
  auto owned = std::make_unique<ffmpeg::CodecContext>(
      FindEncoder(config.video_encoder_name, AVMEDIA_TYPE_VIDEO));
  auto* context = owned->get();
  context->width = config.width;
  context->height = config.height;
  context->framerate = config.fps;
  context->time_base = av_inv_q(config.fps);
  context->sample_aspect_ratio = {1, 1};
  context->bit_rate = config.video_bit_rate;
  context->gop_size = config.gop_size;
  context->max_b_frames = config.max_b_frames;
  context->flags |= AV_CODEC_FLAG_FRAME_DURATION;
  ConfigureHardware(*context, device, config.video_pixel_format);
  ffmpeg::AvDictionary options;
  CopyOptions(config.video_options, options);
  ConfigureRateControl(config, options);
  OpenCodec(*context, options, measure_latency);
  return owned;
}

std::unique_ptr<ffmpeg::CodecContext> OpenAudio(const AvEncoderConfig& config,
                                                bool measure_latency) {
  if (config.audio_sample_rate <= 0 || config.audio_bit_rate < 0 ||
      av_get_bytes_per_sample(config.audio_sample_format) <= 0 ||
      !av_channel_layout_check(&config.audio_channel_layout))
    throw std::invalid_argument("音频编码配置无效");
  auto owned = std::make_unique<ffmpeg::CodecContext>(
      FindEncoder(config.audio_encoder_name, AVMEDIA_TYPE_AUDIO));
  auto* context = owned->get();
  context->sample_fmt = config.audio_sample_format;
  const void* supported = nullptr;
  int count = 0;
  ffmpeg::FfmpegException::throwIfError(
      avcodec_get_supported_config(context, nullptr,
                                   AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &supported,
                                   &count),
      "查询音频编码格式");
  if (supported && count > 0) {
    const auto* formats = static_cast<const AVSampleFormat*>(supported);
    if (std::find(formats, formats + count, context->sample_fmt) ==
        formats + count)
      context->sample_fmt = formats[0];
  }
  context->sample_rate = config.audio_sample_rate;
  context->time_base = {1, config.audio_sample_rate};
  context->bit_rate = config.audio_bit_rate;
  ffmpeg::FfmpegException::throwIfError(
      av_channel_layout_copy(&context->ch_layout, &config.audio_channel_layout),
      "复制编码声道布局");
  ffmpeg::AvDictionary options;
  CopyOptions(config.audio_options, options);
  OpenCodec(*context, options, measure_latency);
  return owned;
}

class AudioFilter final {
 public:
  AudioFilter(AVSampleFormat input, AVSampleFormat output, int rate,
              const AVChannelLayout& layout, int frame_size)
      : adapter_(input, output, rate, layout, frame_size) {
    latency_.Start(PerformanceStage::kEncoderAudioFilter, this, kAudioIndex);
  }

  void Submit(const ffmpeg::Frame& frame) {
    auto submitted = frame.Ref();
    if (latency_.enabled()) {
      latency_.Attach(submitted->opaque_ref, latency_.Begin());
    }
    adapter_.Submit(submitted);
  }
  void End() { adapter_.End(); }
  bool Receive(ffmpeg::Frame& frame) {
    if (!adapter_.Receive(frame)) return false;
    latency_.Complete(frame->opaque_ref);
    return true;
  }

 private:
  internal::StageLatency latency_;
  ffmpeg::internal::AudioFrameAdapter adapter_;
};

}  // namespace

class AvEncoder::Impl final {
 public:
  explicit Impl(const ffmpeg::HwDeviceContext& device)
      : device_(device.Ref()) {}
  ~Impl() { Stop(); }

  void SetOnPacket(OnPacket callback) {
    CheckConfigurable();
    on_packet_ = std::move(callback);
  }
  void SetOnError(OnError callback) {
    CheckConfigurable();
    on_error_ = std::move(callback);
  }
  void SetOnInputSpace(OnInputSpace callback) {
    CheckConfigurable();
    on_input_space_ = std::move(callback);
  }

  bool Start(const AvEncoderConfig& config) noexcept {
    if (HasWorkers()) {
      MW_LOG_ERROR("streamer", "AvEncoder 启动失败: 重新启动前需要 Stop");
      return false;
    }
    try {
      {
        std::lock_guard lock(mutex_);
        streams_.clear();
        error_ = {};
      }
      auto session = std::make_unique<Session>(config);
      std::vector<ffmpeg::StreamInfo> streams;
      const bool video = config.video_pixel_format != AV_PIX_FMT_NONE;
      const bool audio = config.audio_sample_format != AV_SAMPLE_FMT_NONE;
      if (!video && !audio) {
        throw std::invalid_argument("AvEncoder 需要至少一条轨道");
      }
      if (video && (config.fps.num <= 0 || config.fps.den <= 0)) {
        throw std::invalid_argument("AvEncoder 视频帧率无效");
      }
      if (video) {
        session->video_latency.Start(PerformanceStage::kEncoderVideo, this,
                                     kVideoIndex);
        session->video =
            OpenVideo(config, device_, session->video_latency.enabled());
        streams.push_back(ffmpeg::StreamInfo::FromCodecContext(
            *session->video->get(), kVideoIndex));
      }
      if (audio) {
        session->audio_latency.Start(PerformanceStage::kEncoderAudio, this,
                                     kAudioIndex);
        session->audio = OpenAudio(config, session->audio_latency.enabled());
        const auto* context = session->audio->get();
        streams.push_back(
            ffmpeg::StreamInfo::FromCodecContext(*context, kAudioIndex));
        ffmpeg::FfmpegException::throwIfError(
            av_channel_layout_copy(&session->audio_format->ch_layout,
                                   &config.audio_channel_layout),
            "复制编码输入音频布局");
        session->config.audio_channel_layout = session->audio_format->ch_layout;
        session->filter = std::make_unique<AudioFilter>(
            config.audio_sample_format, context->sample_fmt,
            context->sample_rate, context->ch_layout,
            context->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE
                ? 0
                : context->frame_size);
      } else {
        session->config.audio_channel_layout = {};
      }
      {
        std::lock_guard lock(mutex_);
        for (const auto& stream : streams) {
          const auto* parameters = stream.codec_parameters.get();
          MW_LOG_DEBUG(
              "streamer",
              "AvEncoder 轨道配置: instance={} stream={} codec={} "
              "time_base={}/{} size={}x{} sample_rate={} channels={} "
              "bit_rate={} frame_capacity={}",
              fmt::ptr(this), stream.stream_index,
              avcodec_get_name(parameters->codec_id), stream.time_base.num,
              stream.time_base.den, parameters->width, parameters->height,
              parameters->sample_rate, parameters->ch_layout.nb_channels,
              parameters->bit_rate, kFrameCapacity);
        }
        streams_ = std::move(streams);
        session_ = std::move(session);
        // Workers cannot leave their initial queue wait until this lock is
        // released. A partial startup failure marks the session aborted first.
        try {
          if (video && !video_.worker.Start([this] { Run(true); }))
            throw std::runtime_error("启动 AvEncoder 视频线程失败");
          if (audio && !audio_.worker.Start([this] { Run(false); }))
            throw std::runtime_error("启动 AvEncoder 音频线程失败");
        } catch (...) {
          accepting_ = false;
          error_ = std::current_exception();
          throw;
        }
        accepting_ = true;
      }
      MW_LOG_INFO("streamer", "AvEncoder 已启动: instance={} video={} audio={}",
                  fmt::ptr(this), video, audio);
      return true;
    } catch (...) {
      const auto failure = std::current_exception();
      Stop();
      {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        session_.reset();
        streams_.clear();
        error_ = failure;
      }
      try {
        std::rethrow_exception(failure);
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer", "AvEncoder 启动失败: {}", error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "AvEncoder 启动失败: 未知异常");
      }
      return false;
    }
  }

  std::vector<ffmpeg::StreamInfo> streams() const {
    std::lock_guard lock(mutex_);
    return streams_;
  }
  std::exception_ptr error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

  bool TryPush(const ffmpeg::Frame& frame, bool video) {
    std::lock_guard lock(mutex_);
    if (!accepting_) return false;
    const auto& config = session_->config;
    if (video ? !session_->video : !session_->audio)
      throw std::invalid_argument("编码输入轨道未启用");
    if (!frame.get() || !frame->data[0])
      throw std::invalid_argument("编码输入内容为空");
    if (!video && (frame->nb_samples <= 0 ||
                   frame->format != config.audio_sample_format ||
                   frame->sample_rate != config.audio_sample_rate ||
                   av_channel_layout_compare(
                       &frame->ch_layout, &config.audio_channel_layout) != 0))
      throw std::invalid_argument("编码音频内容或格式不匹配");
    ValidateTiming(frame);
    auto& track = video ? video_ : audio_;
    if (track.frames.size() >= kFrameCapacity) {
      if (!track.backpressured) {
        track.backpressured = true;
        MW_LOG_DEBUG("streamer",
                     "AvEncoder 输入背压开始: instance={} track={} stream={} "
                     "queue={}/{}",
                     fmt::ptr(this), video ? "video" : "audio",
                     video ? kVideoIndex : kAudioIndex, track.frames.size(),
                     kFrameCapacity);
      }
      MW_LOG_TRACE(
          "streamer",
          "AvEncoder 输入队列满，未受理新帧: instance={} track={} pts={} "
          "duration={} time_base={}/{} queue={}/{}",
          fmt::ptr(this), video ? "video" : "audio", frame->pts,
          frame->duration, frame->time_base.num, frame->time_base.den,
          track.frames.size(), kFrameCapacity);
      return false;
    }
    track.frames.push_back(frame.Ref());
    MW_LOG_TRACE("streamer",
                 "AvEncoder 帧入队: instance={} track={} stream={} pts={} "
                 "duration={} time_base={}/{} queue={}/{}",
                 fmt::ptr(this), video ? "video" : "audio",
                 video ? kVideoIndex : kAudioIndex, frame->pts, frame->duration,
                 frame->time_base.num, frame->time_base.den,
                 track.frames.size(), kFrameCapacity);
    wake_.notify_all();
    return true;
  }

  void Stop() noexcept {
    if (!HasWorkers()) return;
    {
      std::lock_guard lock(mutex_);
      accepting_ = false;
      MW_LOG_DEBUG(
          "streamer",
          "AvEncoder 停止接收并排空: instance={} video_queue={} audio_queue={}",
          fmt::ptr(this), video_.frames.size(), audio_.frames.size());
    }
    wake_.notify_all();
    video_.worker.Join();
    audio_.worker.Join();
    std::lock_guard lock(mutex_);
    video_.frames.clear();
    audio_.frames.clear();
    video_.backpressured = false;
    audio_.backpressured = false;
    session_.reset();
    MW_LOG_INFO("streamer", "AvEncoder 已停止: instance={}", fmt::ptr(this));
  }

 private:
  struct Track {
    internal::Thread worker;
    std::deque<ffmpeg::Frame> frames;
    bool backpressured = false;
  };

  struct Session {
    explicit Session(AvEncoderConfig value) : config(std::move(value)) {}
    AvEncoderConfig config;
    ffmpeg::Frame audio_format;
    internal::StageLatency video_latency;
    internal::StageLatency audio_latency;
    std::unique_ptr<ffmpeg::CodecContext> video;
    std::unique_ptr<ffmpeg::CodecContext> audio;
    std::unique_ptr<AudioFilter> filter;
  };

  bool HasWorkers() const {
    return video_.worker.joinable() || audio_.worker.joinable();
  }

  void CheckConfigurable() const {
    if (HasWorkers())
      throw std::logic_error("配置或重新启动 AvEncoder 前需要 Stop");
  }
  internal::StageLatency& Latency(int index) {
    return index == kVideoIndex ? session_->video_latency
                                : session_->audio_latency;
  }
  int Receive(AVCodecContext& context, int index) {
    for (;;) {
      ffmpeg::Packet packet;
      const int result = avcodec_receive_packet(&context, packet.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
      ffmpeg::FfmpegException::throwIfError(result, "avcodec_receive_packet");
      Latency(index).Complete(packet->opaque_ref);
      packet->stream_index = index;
      packet->time_base = context.time_base;
      MW_LOG_TRACE("streamer",
                   "AvEncoder 输出包: instance={} stream={} pts={} dts={} "
                   "duration={} time_base={}/{} bytes={} keyframe={}",
                   fmt::ptr(this), index, packet->pts, packet->dts,
                   packet->duration, packet->time_base.num,
                   packet->time_base.den, packet->size,
                   (packet->flags & AV_PKT_FLAG_KEY) != 0);
      if (on_packet_) on_packet_(packet);
    }
  }
  void Encode(AVCodecContext& context, const ffmpeg::Frame& frame, int index) {
    auto submitted = InTimeBase(frame, context.time_base);
    if (context.codec_type == AVMEDIA_TYPE_VIDEO) {
      submitted->pict_type = AV_PICTURE_TYPE_NONE;
      submitted->pkt_dts = AV_NOPTS_VALUE;
    }
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
    if (context.flags & AV_CODEC_FLAG_COPY_OPAQUE) {
      auto& latency = Latency(index);
      latency.Attach(submitted->opaque_ref, latency.Begin());
    }
#endif
    MW_LOG_TRACE(
        "streamer",
        "AvEncoder 提交 codec: instance={} stream={} pts={} duration={} "
        "time_base={}/{} samples={}",
        fmt::ptr(this), index, submitted->pts, submitted->duration,
        context.time_base.num, context.time_base.den, submitted->nb_samples);
    for (;;) {
      const int result = avcodec_send_frame(&context, submitted.get());
      if (result != AVERROR(EAGAIN)) {
        ffmpeg::FfmpegException::throwIfError(result, "avcodec_send_frame");
        break;
      }
      Receive(context, index);
    }
    Receive(context, index);
  }
  void DrainCodec(AVCodecContext& context, int index) {
    MW_LOG_DEBUG("streamer", "AvEncoder 提交 EOF: instance={} stream={}",
                 fmt::ptr(this), index);
    for (;;) {
      const int result = avcodec_send_frame(&context, nullptr);
      if (result != AVERROR(EAGAIN)) {
        if (result != AVERROR_EOF)
          ffmpeg::FfmpegException::throwIfError(result,
                                                "avcodec_send_frame EOF");
        break;
      }
      Receive(context, index);
    }
    if (Receive(context, index) != AVERROR_EOF)
      throw std::runtime_error("编码器排空未到达 EOF");
    MW_LOG_DEBUG("streamer", "AvEncoder 排空完成: instance={} stream={}",
                 fmt::ptr(this), index);
  }
  void Video(const ffmpeg::Frame* frame) {
    if (frame) {
      const auto prepared = PrepareVideoFrame(*session_->video->get(), *frame);
      Encode(*session_->video->get(), prepared, kVideoIndex);
    } else {
      DrainCodec(*session_->video->get(), kVideoIndex);
    }
  }
  void Audio(const ffmpeg::Frame* frame) {
    if (frame)
      session_->filter->Submit(*frame);
    else
      session_->filter->End();
    for (;;) {
      ffmpeg::Frame filtered;
      if (!session_->filter->Receive(filtered)) break;
      Encode(*session_->audio->get(), filtered, kAudioIndex);
    }
    if (!frame) DrainCodec(*session_->audio->get(), kAudioIndex);
  }
  void Run(bool video) noexcept {
    try {
      auto& track = video ? video_ : audio_;
      for (;;) {
        std::optional<ffmpeg::Frame> frame;
        {
          std::unique_lock lock(mutex_);
          wake_.wait(lock, [this, &track] {
            return !accepting_ || error_ || !track.frames.empty();
          });
          if (error_) return;
          if (!track.frames.empty()) {
            frame = std::move(track.frames.front());
            track.frames.pop_front();
            MW_LOG_TRACE(
                "streamer",
                "AvEncoder 消费输入: instance={} track={} stream={} pts={} "
                "duration={} time_base={}/{} queue={}/{}",
                fmt::ptr(this), video ? "video" : "audio",
                video ? kVideoIndex : kAudioIndex, (*frame)->pts,
                (*frame)->duration, (*frame)->time_base.num,
                (*frame)->time_base.den, track.frames.size(), kFrameCapacity);
            if (track.backpressured) {
              track.backpressured = false;
              MW_LOG_DEBUG("streamer",
                           "AvEncoder 输入背压恢复: instance={} track={} "
                           "stream={} queue={}/{}",
                           fmt::ptr(this), video ? "video" : "audio",
                           video ? kVideoIndex : kAudioIndex,
                           track.frames.size(), kFrameCapacity);
            }
          }
        }
        if (frame && on_input_space_) {
          try {
            on_input_space_(video);
          } catch (const std::exception& error) {
            MW_LOG_ERROR("streamer",
                         "AvEncoder OnInputSpace 回调异常: instance={} "
                         "track={} error={}",
                         fmt::ptr(this), video ? "video" : "audio",
                         error.what());
          } catch (...) {
            MW_LOG_ERROR(
                "streamer",
                "AvEncoder OnInputSpace 回调抛出未知异常: instance={} track={}",
                fmt::ptr(this), video ? "video" : "audio");
          }
        }
        // Closing admission drains each track's accepted FIFO before codec
        // EOF. Audio also drains its reblocking filter before codec EOF.
        if (video)
          Video(frame ? &*frame : nullptr);
        else
          Audio(frame ? &*frame : nullptr);
        if (!frame) break;
      }
    } catch (...) {
      const auto failure = std::current_exception();
      bool first = false;
      {
        std::lock_guard lock(mutex_);
        if (!error_) {
          error_ = failure;
          first = true;
        }
        accepting_ = false;
        video_.frames.clear();
        audio_.frames.clear();
      }
      wake_.notify_all();
      try {
        std::rethrow_exception(failure);
      } catch (const ffmpeg::FfmpegException& error) {
        MW_LOG_ERROR(
            "streamer",
            "AvEncoder 编码失败: instance={} track={} code={} error={}",
            fmt::ptr(this), video ? "video" : "audio", error.error_code(),
            error.what());
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer",
                     "AvEncoder 编码失败: instance={} track={} error={}",
                     fmt::ptr(this), video ? "video" : "audio", error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer",
                     "AvEncoder 编码失败: instance={} track={} 未知异常",
                     fmt::ptr(this), video ? "video" : "audio");
      }
      if (first && on_error_) {
        try {
          on_error_(failure);
        } catch (const std::exception& error) {
          MW_LOG_ERROR("streamer",
                       "AvEncoder OnError 回调异常: instance={} error={}",
                       fmt::ptr(this), error.what());
        } catch (...) {
          MW_LOG_ERROR("streamer",
                       "AvEncoder OnError 回调抛出未知异常: instance={}",
                       fmt::ptr(this));
        }
      }
    }
  }

  ffmpeg::HwDeviceContext device_;
  Track video_;
  Track audio_;
  OnPacket on_packet_;
  OnError on_error_;
  OnInputSpace on_input_space_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool accepting_ = false;
  std::exception_ptr error_;
  std::unique_ptr<Session> session_;
  std::vector<ffmpeg::StreamInfo> streams_;
};

AvEncoder::AvEncoder(const ffmpeg::HwDeviceContext& device)
    : impl_(std::make_unique<Impl>(device)) {}
AvEncoder::~AvEncoder() = default;
void AvEncoder::SetOnPacket(OnPacket callback) {
  impl_->SetOnPacket(std::move(callback));
}
void AvEncoder::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
void AvEncoder::SetOnInputSpace(OnInputSpace callback) {
  impl_->SetOnInputSpace(std::move(callback));
}
bool AvEncoder::Start(const AvEncoderConfig& config) noexcept {
  return impl_->Start(config);
}
std::vector<ffmpeg::StreamInfo> AvEncoder::streams() const {
  return impl_->streams();
}
bool AvEncoder::TryPushVideo(const ffmpeg::Frame& frame) {
  return impl_->TryPush(frame, true);
}
bool AvEncoder::TryPushAudio(const ffmpeg::Frame& frame) {
  return impl_->TryPush(frame, false);
}
std::exception_ptr AvEncoder::error() const { return impl_->error(); }
void AvEncoder::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
