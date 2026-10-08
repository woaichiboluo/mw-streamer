#include "mw/streamer/encoder/encoder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer {
namespace {

constexpr AVRational kNanoseconds{1, 1000000000};
constexpr std::size_t kVideoCacheSize = 6;

class Options final {
 public:
  ~Options() { av_dict_free(&value_); }
  void Set(const char* name, const std::string& value) {
    ffmpeg::FfmpegException::throwIfError(
        av_dict_set(&value_, name, value.c_str(), 0), "设置编码选项");
  }
  AVDictionary* get() const noexcept { return value_; }

 private:
  AVDictionary* value_ = nullptr;
};

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
                 Options& target) {
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

void ConfigureRateControl(const EncoderConfig& config, Options& options) {
  const auto& name = config.video_encoder_name;
  const bool cbr = config.rate_control == RateControl::kCbr;
  if (config.rate_control != RateControl::kCbr &&
      config.rate_control != RateControl::kVbr) {
    throw std::invalid_argument("未知码率控制模式");
  }
  if (!IsNvenc(name) && name != "libx264" && name != "libx265") {
    throw std::invalid_argument(
        "当前码率控制支持 libx264、libx265 和 NVENC 编码器");
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
  } else if (cbr) {
    std::string parameters;
    const auto supplied = config.video_options.find("x265-params");
    if (supplied != config.video_options.end() && !supplied->second.empty()) {
      parameters = supplied->second + ":";
    }
    options.Set("x265-params", parameters + "strict-cbr=1:hrd=1");
  }
}

std::int64_t Timestamp(const ffmpeg::Frame& frame) {
  if (frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
      frame->time_base.den <= 0) {
    throw std::invalid_argument("编码输入需要有效 PTS 和时间基");
  }
  return av_rescale_q(frame->pts, frame->time_base, kNanoseconds);
}

}  // namespace

Encoder::~Encoder() { Stop(); }

void Encoder::SetOnReady(OnReady callback) noexcept {
  on_ready_ = std::move(callback);
}
void Encoder::SetOnPacket(OnPacket callback) noexcept {
  on_packet_ = std::move(callback);
}
void Encoder::SetOnEnded(OnEnded callback) noexcept {
  on_ended_ = std::move(callback);
}
void Encoder::SetOnError(OnError callback) noexcept {
  on_error_ = std::move(callback);
}

void Encoder::Start(const EncoderConfig& config,
                    const std::vector<ffmpeg::StreamInfo>& streams,
                    const ffmpeg::HwDeviceContext& video_device) {
  {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("Encoder 已经启动");
  }
  const ffmpeg::StreamInfo* video = nullptr;
  const ffmpeg::StreamInfo* audio = nullptr;
  for (const auto& stream : streams) {
    stream.Validate();
    const auto type = stream.codec_parameters.get()->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO) {
      if (video) throw std::invalid_argument("Encoder 只支持一条视频轨道");
      video = &stream;
    } else if (type == AVMEDIA_TYPE_AUDIO) {
      if (audio) throw std::invalid_argument("Encoder 只支持一条音频轨道");
      audio = &stream;
    }
  }
  if (video && audio && video->stream_index == audio->stream_index) {
    throw std::invalid_argument("音视频轨道索引不能相同");
  }
  try {
    config_ = config;
    std::vector<ffmpeg::StreamInfo> output;
    if (video) {
      if (config.fps.num <= 0 || config.fps.den <= 0 || config.width < 0 ||
          config.height < 0 || ((config.width == 0) != (config.height == 0))) {
        throw std::invalid_argument("视频尺寸或帧率配置无效");
      }
      const auto* parameters = video->codec_parameters.get();
      ffmpeg::VideoEncoderConfig native;
      native.encoder_name = config.video_encoder_name;
      native.width = config.width ? config.width : parameters->width;
      native.height = config.height ? config.height : parameters->height;
      native.pixel_format = static_cast<AVPixelFormat>(parameters->format);
      native.frame_rate = config.fps;
      native.time_base = av_inv_q(config.fps);
      native.bit_rate = config.video_bit_rate;
      native.gop_size = config.gop_size;
      native.max_b_frames = config.max_b_frames;
      native.sample_aspect_ratio = parameters->sample_aspect_ratio;
      if (native.sample_aspect_ratio.den <= 0) {
        native.sample_aspect_ratio = {1, 1};
      }
      native.color_range = parameters->color_range;
      native.color_space = parameters->color_space;
      native.color_primaries = parameters->color_primaries;
      native.color_trc = parameters->color_trc;
      Options options;
      CopyOptions(config.video_options, options);
      ConfigureRateControl(config, options);
      if (IsNvenc(native.encoder_name)) {
        video_encoder_ = std::make_unique<ffmpeg::VideoEncoder>(
            native, video_device, options.get());
      } else {
        video_encoder_ =
            std::make_unique<ffmpeg::VideoEncoder>(native, options.get());
      }
      video_stream_index_ = video->stream_index;
      output.push_back(video_encoder_->stream_info(video_stream_index_));
    }
    if (audio) {
      const auto* parameters = audio->codec_parameters.get();
      if (config.audio_bit_rate <= 0) {
        throw std::invalid_argument("音频码率必须大于零");
      }
      ffmpeg::AudioEncoderConfig native;
      native.encoder_name = config.audio_encoder_name;
      native.sample_format = static_cast<AVSampleFormat>(parameters->format);
      native.sample_rate = parameters->sample_rate;
      native.channel_layout = parameters->ch_layout;
      native.bit_rate = config.audio_bit_rate;
      Options options;
      CopyOptions(config.audio_options, options);
      audio_encoder_ =
          std::make_unique<ffmpeg::AudioEncoder>(native, options.get());
      audio_stream_index_ = audio->stream_index;
      audio_format_ = native.sample_format;
      audio_rate_ = native.sample_rate;
      ffmpeg::FfmpegException::throwIfError(
          av_channel_layout_copy(&audio_layout_, &native.channel_layout),
          "复制音频编码声道布局");
      audio_capabilities_ =
          avcodec_find_encoder_by_name(native.encoder_name.c_str())
              ->capabilities;
      output.push_back(audio_encoder_->stream_info(audio_stream_index_));
    }
    {
      std::lock_guard lock(mutex_);
      started_ = true;
      stopping_ = false;
      draining_ = false;
      failed_ = false;
      video_done_ = !video_encoder_;
      audio_done_ = !audio_encoder_;
      ended_notified_ = false;
      performance_.Start(this, static_cast<bool>(video_encoder_),
                         static_cast<bool>(audio_encoder_));
    }
    if (on_ready_) on_ready_(output);
    if (video_encoder_) video_thread_ = std::thread(&Encoder::RunVideo, this);
    if (audio_encoder_) audio_thread_ = std::thread(&Encoder::RunAudio, this);
    MW_LOG_INFO("streamer", "Encoder started: video={} audio={}",
                static_cast<bool>(video_encoder_),
                static_cast<bool>(audio_encoder_));
  } catch (...) {
    Stop();
    throw;
  }
}

bool Encoder::SubmitVideo(const ffmpeg::Frame& frame) {
  std::lock_guard lock(mutex_);
  if (!started_ || stopping_ || draining_ || failed_ || !video_encoder_) {
    return false;
  }
  const bool skipped_picture = video_frames_.size() == kVideoCacheSize;
  if (skipped_picture) {
    ++video_frames_.back().count;
  } else {
    video_frames_.push_back({frame.Ref(), Timestamp(frame)});
  }
  performance_.AcceptVideo(video_frames_.size(), skipped_picture);
  wake_.notify_all();
  return true;
}

bool Encoder::SubmitAudio(const ffmpeg::Frame& frame) {
  std::lock_guard lock(mutex_);
  if (!started_ || stopping_ || draining_ || failed_ || !audio_encoder_) {
    return false;
  }
  const auto timestamp = Timestamp(frame);
  if (frame->format != audio_format_ || frame->sample_rate != audio_rate_ ||
      frame->nb_samples <= 0 ||
      av_channel_layout_compare(&frame->ch_layout, &audio_layout_) != 0) {
    throw std::invalid_argument("音频输入格式与编码配置不匹配");
  }
  audio_frames_.push_back(frame.Ref());
  performance_.AcceptAudio(frame->nb_samples, audio_frames_.size());
  if (!first_audio_ns_) first_audio_ns_ = timestamp;
  wake_.notify_all();
  return true;
}

void Encoder::Drain() noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || failed_) return;
    draining_ = true;
    if (video_done_ && audio_done_ && !ended_notified_) {
      ended_notified_ = true;
      notify = true;
      ++callback_calls_;
    }
  }
  wake_.notify_all();
  if (notify) {
    if (on_ended_) on_ended_();
    {
      std::lock_guard lock(mutex_);
      --callback_calls_;
    }
    wake_.notify_all();
  }
}

void Encoder::Stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (video_thread_.joinable()) video_thread_.join();
  if (audio_thread_.joinable()) audio_thread_.join();
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [this] { return callback_calls_ == 0; });
  performance_.Finish();
  Clear();
}

void Encoder::Clear() noexcept {
  video_frames_.clear();
  audio_frames_.clear();
  video_encoder_.reset();
  audio_encoder_.reset();
  av_channel_layout_uninit(&audio_layout_);
  first_audio_ns_.reset();
  video_start_ns_.reset();
  video_dts_offset_ns_.reset();
  audio_start_ns_ = 0;
  video_stream_index_ = -1;
  audio_stream_index_ = -1;
  started_ = false;
  video_done_ = true;
  audio_done_ = true;
}

bool Encoder::Cancelled() noexcept {
  std::lock_guard lock(mutex_);
  return stopping_ || failed_;
}

ffmpeg::EncodeResult Encoder::ReceivePackets(ffmpeg::Encoder& encoder,
                                             bool video) {
  ffmpeg::Packet packet;
  for (;;) {
    if (Cancelled()) return ffmpeg::EncodeResult::kEnd;
    const auto result = encoder.ReceivePacket(packet);
    if (result != ffmpeg::EncodeResult::kPacket) return result;
    {
      std::lock_guard lock(mutex_);
      performance_.Packet(video, packet->size);
    }
    packet->stream_index = video ? video_stream_index_ : audio_stream_index_;
    auto dts_ns = packet->dts;
    if (dts_ns != AV_NOPTS_VALUE) {
      dts_ns = av_rescale_q(dts_ns, packet->time_base, kNanoseconds);
      if (video) {
        if (!video_dts_offset_ns_) video_dts_offset_ns_ = dts_ns;
        dts_ns += *video_start_ns_ - *video_dts_offset_ns_;
      } else {
        dts_ns += audio_start_ns_;
      }
    }
    if (!Cancelled() && on_packet_) on_packet_(packet, dts_ns);
  }
}

void Encoder::EncodeFrame(ffmpeg::Encoder& encoder, const ffmpeg::Frame& frame,
                          bool video) {
  while (!Cancelled() && !encoder.SendFrame(frame)) {
    ReceivePackets(encoder, video);
  }
  if (!Cancelled()) ReceivePackets(encoder, video);
}

void Encoder::DrainCodec(ffmpeg::Encoder& encoder, bool video) {
  while (!Cancelled() && !encoder.Drain()) ReceivePackets(encoder, video);
  if (!Cancelled() &&
      ReceivePackets(encoder, video) != ffmpeg::EncodeResult::kEnd) {
    throw std::runtime_error("编码器排空后仍要求输入");
  }
}

void Encoder::RunVideo() noexcept {
  try {
    std::int64_t next_pts = 0;
    for (;;) {
      ffmpeg::Frame source;
      internal::EncoderPerformance::TimePoint work_started;
      bool repeated = false;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || failed_ || draining_ ||
                 (!video_frames_.empty() &&
                  (!audio_encoder_ || first_audio_ns_));
        });
        if (stopping_ || failed_) break;
        if (video_frames_.empty()) {
          if (draining_) break;
          continue;
        }
        if (audio_encoder_ && !first_audio_ns_) {
          if (draining_) {
            video_frames_.clear();
            performance_.Abandon(true);
            break;
          }
          continue;
        }
        auto& slot = video_frames_.front();
        const auto source_timestamp =
            slot.timestamp_ns +
            av_rescale_q(static_cast<std::int64_t>(slot.consumed),
                         av_inv_q(config_.fps), kNanoseconds);
        if (!video_start_ns_ && audio_encoder_ &&
            source_timestamp < *first_audio_ns_) {
          if (++slot.consumed == slot.count) video_frames_.pop_front();
          performance_.VideoDiscarded(video_frames_.size());
          continue;
        }
        if (!video_start_ns_) {
          video_start_ns_ = source_timestamp;
          wake_.notify_all();
        }
        source = slot.frame.Ref();
        repeated = slot.consumed > 0;
        work_started = performance_.Begin(true);
      }
      auto prepared = video_encoder_->PrepareFrame(source);
      prepared->pts = next_pts++;
      prepared->duration = 1;
      prepared->time_base = av_inv_q(config_.fps);
      EncodeFrame(*video_encoder_, prepared, true);
      {
        std::lock_guard lock(mutex_);
        auto& slot = video_frames_.front();
        if (++slot.consumed == slot.count) video_frames_.pop_front();
        if (!stopping_ && !failed_) {
          performance_.Encoded(true, work_started, 0, repeated,
                               video_frames_.size());
        }
      }
    }
    if (!Cancelled()) DrainCodec(*video_encoder_, true);
  } catch (const ffmpeg::FfmpegException& error) {
    Fail(error.error_code(), error.what(), true);
  } catch (const std::exception& error) {
    Fail(AVERROR_EXTERNAL, error.what(), true);
  } catch (...) {
    Fail(AVERROR_EXTERNAL, "未知视频编码错误", true);
  }
  FinishTrack(true);
}

void Encoder::RunAudio() noexcept {
  try {
    using AudioFifo =
        std::unique_ptr<AVAudioFifo, decltype(&av_audio_fifo_free)>;
    AudioFifo fifo(
        av_audio_fifo_alloc(audio_format_, audio_layout_.nb_channels,
                            std::max(1, audio_encoder_->frame_size())),
        &av_audio_fifo_free);
    if (!fifo) throw std::bad_alloc();
    std::int64_t next_pts = 0;
    bool aligned = false;
    std::optional<ffmpeg::Frame> properties;
    auto encode_samples = [&](int samples) {
      internal::EncoderPerformance::TimePoint work_started;
      {
        std::lock_guard lock(mutex_);
        work_started = performance_.Begin(false);
      }
      ffmpeg::Frame output;
      output->format = audio_format_;
      output->sample_rate = audio_rate_;
      output->nb_samples = samples;
      ffmpeg::FfmpegException::throwIfError(
          av_channel_layout_copy(&output->ch_layout, &audio_layout_),
          "设置编码音频声道布局");
      ffmpeg::FfmpegException::throwIfError(
          av_frame_get_buffer(output.get(), 0), "分配编码音频帧");
      if (properties) output.CopyPropertiesFrom(*properties);
      const auto read = av_audio_fifo_read(
          fifo.get(), reinterpret_cast<void**>(output->extended_data), samples);
      if (read != samples) throw std::runtime_error("读取音频编码 FIFO 失败");
      output->pts = next_pts;
      output->duration = samples;
      output->time_base = {1, audio_rate_};
      next_pts += samples;
      EncodeFrame(*audio_encoder_, output, false);
      {
        std::lock_guard lock(mutex_);
        if (!stopping_ && !failed_) {
          performance_.Encoded(false, work_started, samples);
        }
      }
    };
    for (;;) {
      ffmpeg::Frame source;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || failed_ ||
                 ((!video_encoder_ || video_start_ns_) &&
                  (!audio_frames_.empty() || draining_)) ||
                 (draining_ && video_done_);
        });
        if (stopping_ || failed_) break;
        if (video_encoder_ && !video_start_ns_) {
          if (draining_ && video_done_) {
            audio_frames_.clear();
            performance_.Abandon(false);
            break;
          }
          continue;
        }
        if (audio_frames_.empty()) {
          if (draining_) break;
          continue;
        }
        if (!aligned && next_pts == 0) {
          audio_start_ns_ =
              video_encoder_ ? *video_start_ns_ : *first_audio_ns_;
        }
        source = std::move(audio_frames_.front());
        audio_frames_.pop_front();
        performance_.AudioDequeued(audio_frames_.size());
      }
      if (!properties) properties = source.Ref();
      int offset = 0;
      if (!aligned) {
        const auto timestamp = Timestamp(source);
        if (timestamp < audio_start_ns_) {
          const auto skip =
              av_rescale_q_rnd(audio_start_ns_ - timestamp, kNanoseconds,
                               {1, audio_rate_}, AV_ROUND_DOWN);
          offset = static_cast<int>(
              std::min<std::int64_t>(skip, source->nb_samples));
        }
        if (offset > 0) {
          std::lock_guard lock(mutex_);
          performance_.AudioDiscarded(offset);
        }
        if (offset == source->nb_samples) continue;
        aligned = true;
      }
      const int samples = source->nb_samples - offset;
      const int planes = av_sample_fmt_is_planar(audio_format_)
                             ? audio_layout_.nb_channels
                             : 1;
      const int stride = av_get_bytes_per_sample(audio_format_) *
                         (planes == 1 ? audio_layout_.nb_channels : 1);
      std::vector<void*> data(static_cast<std::size_t>(planes));
      for (int plane = 0; plane < planes; ++plane) {
        data[static_cast<std::size_t>(plane)] =
            source->extended_data[plane] + offset * stride;
      }
      ffmpeg::FfmpegException::throwIfError(
          av_audio_fifo_realloc(fifo.get(),
                                av_audio_fifo_size(fifo.get()) + samples),
          "扩展音频编码 FIFO");
      if (av_audio_fifo_write(fifo.get(), data.data(), samples) != samples) {
        throw std::runtime_error("写入音频编码 FIFO 失败");
      }
      const int frame_size = audio_encoder_->frame_size();
      if (frame_size == 0) {
        encode_samples(av_audio_fifo_size(fifo.get()));
      } else {
        while (!Cancelled() && av_audio_fifo_size(fifo.get()) >= frame_size) {
          encode_samples(frame_size);
        }
      }
    }
    if (!Cancelled()) {
      const int remaining = av_audio_fifo_size(fifo.get());
      if (remaining > 0) {
        if (!(audio_capabilities_ & (AV_CODEC_CAP_SMALL_LAST_FRAME |
                                     AV_CODEC_CAP_VARIABLE_FRAME_SIZE))) {
          throw std::runtime_error("音频编码器不支持不足一帧的尾部样本");
        }
        encode_samples(remaining);
      }
      DrainCodec(*audio_encoder_, false);
    }
  } catch (const ffmpeg::FfmpegException& error) {
    Fail(error.error_code(), error.what(), false);
  } catch (const std::exception& error) {
    Fail(AVERROR_EXTERNAL, error.what(), false);
  } catch (...) {
    Fail(AVERROR_EXTERNAL, "未知音频编码错误", false);
  }
  FinishTrack(false);
}

void Encoder::FinishTrack(bool video) noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    (video ? video_done_ : audio_done_) = true;
    if (video_done_ && audio_done_) performance_.Finish();
    if (draining_ && !stopping_ && !failed_ && video_done_ && audio_done_ &&
        !ended_notified_) {
      ended_notified_ = true;
      notify = true;
    }
  }
  wake_.notify_all();
  if (notify && on_ended_) on_ended_();
}

void Encoder::Fail(int error, std::string_view message, bool video) noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    if (!stopping_ && !failed_) {
      failed_ = true;
      performance_.Error(video);
      notify = true;
    }
  }
  wake_.notify_all();
  if (notify) {
    MW_LOG_ERROR("streamer", "Encoder failed: code={} error={}", error,
                 message);
    if (on_error_) on_error_(error, message);
  }
}

}  // namespace mw::streamer
