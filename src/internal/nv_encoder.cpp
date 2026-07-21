#include "mw/streamer/internal/nv_encoder.h"

#include <string>
#include <utility>

#include "spdlog/spdlog.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/dictionary.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"

namespace mw::streamer::internal {
namespace {

const char* EncoderName(AVCodecID codec_id) {
  if (codec_id == AV_CODEC_ID_H264) {
    return "h264_nvenc";
  }
  if (codec_id == AV_CODEC_ID_HEVC) {
    return "hevc_nvenc";
  }
  ThrowError(StatusCode::kUnsupported,
             "NVENC only supports H.264 and H.265 output");
}

bool SupportsCudaFrames(const AVCodec& codec) {
  for (int index = 0;; ++index) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(&codec, index);
    if (config == nullptr) {
      return false;
    }
    if (config->device_type == AV_HWDEVICE_TYPE_CUDA &&
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) != 0 &&
        config->pix_fmt == AV_PIX_FMT_CUDA) {
      return true;
    }
  }
}

void ValidateConfig(const NvEncoderConfig& config) {
  if (config.width <= 0 || config.height <= 0 ||
      !IsValidTimeBase(config.frame_rate) || config.gop_size <= 0 ||
      config.bit_rate <= 0 || config.max_bit_rate < 0 ||
      config.vbv_buffer_size < 0 || config.max_b_frames < 0 ||
      config.preset.empty()) {
    ThrowError(StatusCode::kInvalidArgument,
               "NVENC configuration contains an invalid value");
  }
}

void ValidateFramePool(const GpuNv12FramePool& frame_pool,
                       const CudaDeviceContext& device_context, int width,
                       int height) {
  if (!frame_pool.is_valid() ||
      frame_pool.device_id() != device_context.device_id() ||
      frame_pool.width() != width || frame_pool.height() != height) {
    ThrowError(StatusCode::kInvalidArgument,
               "NVENC frame pool does not match the encoder configuration");
  }

  const auto* frames_context = reinterpret_cast<const AVHWFramesContext*>(
      frame_pool.buffer().get()->data);
  if (frames_context == nullptr || frames_context->format != AV_PIX_FMT_CUDA ||
      frames_context->sw_format != AV_PIX_FMT_NV12 ||
      frames_context->device_ref == nullptr ||
      frames_context->device_ref->data != device_context.buffer().get()->data) {
    ThrowError(StatusCode::kInvalidArgument,
               "NVENC requires a CUDA NV12 frame pool on the pipeline device");
  }
}

ffmpeg::Dictionary BuildOptions(const NvEncoderConfig& config) {
  ffmpeg::Dictionary options;
  for (const auto& [key, value] : config.options) {
    options.Set(key, value);
  }
  options.Set("preset", config.preset);
  options.Set("profile", "main");
  options.Set(
      "rc", config.rate_control == NvEncoderRateControl::kCbr ? "cbr" : "vbr");
  return options;
}

}  // namespace

void NvEncoder::Open(const NvEncoderConfig& config,
                     const CudaDeviceContext& device_context,
                     const GpuNv12FramePool& frame_pool) {
  if (is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "NvEncoder can only be opened once");
  }
  ValidateConfig(config);
  if (!device_context.is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "NvEncoder requires a valid CUDA device context");
  }
  ValidateFramePool(frame_pool, device_context, config.width, config.height);

  const char* encoder_name = EncoderName(config.codec_id);
  const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
  if (codec == nullptr || av_codec_is_encoder(codec) == 0 ||
      !SupportsCudaFrames(*codec)) {
    ThrowError(
        StatusCode::kUnsupported,
        std::string("required NVIDIA encoder is unavailable: ") + encoder_name);
  }

  ffmpeg::CodecContext candidate(codec);
  AVCodecContext* context = candidate.get();
  context->codec_type = AVMEDIA_TYPE_VIDEO;
  context->codec_id = config.codec_id;
  context->width = config.width;
  context->height = config.height;
  context->framerate = config.frame_rate;
  context->time_base = av_inv_q(config.frame_rate);
  context->pix_fmt = AV_PIX_FMT_CUDA;
  context->gop_size = config.gop_size;
  context->max_b_frames = config.max_b_frames;
  context->bit_rate = config.bit_rate;
  context->color_range = config.color_range;
  context->colorspace = config.color_space;
  context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  context->rc_max_rate =
      config.max_bit_rate > 0 ? config.max_bit_rate : config.bit_rate;
  context->rc_buffer_size = config.vbv_buffer_size > 0 ? config.vbv_buffer_size
                                                       : context->rc_max_rate;
  context->hw_frames_ctx = av_buffer_ref(frame_pool.buffer().get());
  if (context->hw_frames_ctx == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to reference the NVENC frame pool");
  }

  ffmpeg::Dictionary options = BuildOptions(config);
  CheckFfmpeg(avcodec_open2(context, codec, options.inout()),
              "failed to open the NVIDIA video encoder");
  for (const std::string& option : options.keys()) {
    spdlog::warn("NVENC did not consume option '{}'", option);
  }

  CheckFfmpeg(avcodec_parameters_from_context(codec_parameters_.get(), context),
              "failed to copy NVENC codec parameters");
  frame_context_ = context->hw_frames_ctx;
  time_base_ = context->time_base;
  width_ = context->width;
  height_ = context->height;
  encoder_.emplace(std::move(candidate));
}

bool NvEncoder::Send(const ffmpeg::Frame& frame) {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "NvEncoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "cannot send a video frame after end of stream");
  }

  const AVFrame* raw_frame = frame.get();
  if (raw_frame == nullptr || raw_frame->format != AV_PIX_FMT_CUDA ||
      raw_frame->width != width_ || raw_frame->height != height_ ||
      raw_frame->hw_frames_ctx == nullptr ||
      raw_frame->hw_frames_ctx->data != frame_context_->data ||
      raw_frame->pts == AV_NOPTS_VALUE ||
      !IsValidTimeBase(raw_frame->time_base) ||
      av_cmp_q(raw_frame->time_base, time_base_) != 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "video frame does not match the NVENC input contract");
  }

  const int result = avcodec_send_frame(encoder_->get(), raw_frame);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send a CUDA frame to NVENC");
  return true;
}

bool NvEncoder::SendEndOfStream() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "NvEncoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "NVENC end of stream was already sent");
  }

  const int result = avcodec_send_frame(encoder_->get(), nullptr);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send the NVENC drain frame");
  drain_sent_ = true;
  return true;
}

std::optional<ffmpeg::Packet> NvEncoder::Receive() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "NvEncoder has not been opened");
  }

  ffmpeg::Packet packet;
  const int result = avcodec_receive_packet(encoder_->get(), packet.get());
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return std::nullopt;
  }
  CheckFfmpeg(result, "failed to receive an NVENC packet");
  packet.get()->time_base = time_base_;
  return packet;
}

bool NvEncoder::is_open() const noexcept {
  return encoder_.has_value() && encoder_->get() != nullptr;
}

AVRational NvEncoder::time_base() const noexcept { return time_base_; }

const ffmpeg::CodecParameters& NvEncoder::codec_parameters() const noexcept {
  return codec_parameters_;
}

}  // namespace mw::streamer::internal
