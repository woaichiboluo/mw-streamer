#include "mw/streamer/internal/nv_decoder.h"

#include <cstddef>
#include <optional>
#include <utility>

#include "fmt/format.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"
#include "mw/streamer/internal/gpu_frame_utils.h"

namespace mw::streamer::internal {
namespace {

AVPixelFormat SelectCudaPixelFormat(AVCodecContext*,
                                    const AVPixelFormat* formats) {
  for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE;
       ++format) {
    if (*format == AV_PIX_FMT_CUDA) {
      return *format;
    }
  }
  return AV_PIX_FMT_NONE;
}

void ValidateHardwareFrame(const AVFrame& frame,
                           const AVBufferRef* device_context) {
  if (frame.format != AV_PIX_FMT_CUDA) {
    ThrowError(StatusCode::kUnsupported,
               fmt::format("NVDEC returned pixel format {}, expected CUDA",
                           frame.format));
  }
  if (frame.hw_frames_ctx == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "NVDEC returned a frame without a hardware context");
  }

  const auto* frames_context =
      reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
  if (frames_context == nullptr || frames_context->format != AV_PIX_FMT_CUDA ||
      frames_context->sw_format != AV_PIX_FMT_NV12) {
    ThrowError(StatusCode::kUnsupported,
               "NVDEC output is not a CUDA-backed NV12 frame");
  }
  if (frames_context->device_ref == nullptr || device_context == nullptr ||
      frames_context->device_ref->data != device_context->data) {
    ThrowError(StatusCode::kFfmpegError,
               "NVDEC frame does not use the pipeline CUDA context");
  }
  if (frame.data[0] == nullptr || frame.data[1] == nullptr ||
      frame.linesize[0] <= 0 || frame.linesize[1] <= 0) {
    ThrowError(StatusCode::kFfmpegError,
               "NVDEC returned an incomplete CUDA NV12 frame");
  }
}

}  // namespace

void NvDecoder::Open(const StreamInfo& stream,
                     const CudaDeviceContext& device_context) {
  if (is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "NvDecoder can only be opened once");
  }
  const AVCodecParameters* codec_parameters = stream.codec_parameters.get();
  if (stream.index < 0 || codec_parameters == nullptr ||
      codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
    ThrowError(StatusCode::kInvalidArgument,
               "NvDecoder requires valid video stream information");
  }
  if (!IsValidTimeBase(stream.time_base)) {
    ThrowError(StatusCode::kInvalidArgument,
               "NvDecoder requires a valid packet time base");
  }
  if (!device_context.is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "NvDecoder requires a valid CUDA device context");
  }

  const char* decoder_name = nullptr;
  if (codec_parameters->codec_id == AV_CODEC_ID_H264) {
    decoder_name = "h264_cuvid";
  } else if (codec_parameters->codec_id == AV_CODEC_ID_HEVC) {
    decoder_name = "hevc_cuvid";
  } else {
    ThrowError(StatusCode::kUnsupported,
               fmt::format("unsupported input video codec id {}",
                           static_cast<int>(codec_parameters->codec_id)));
  }

  const AVCodec* codec = avcodec_find_decoder_by_name(decoder_name);
  if (codec == nullptr || av_codec_is_decoder(codec) == 0) {
    ThrowError(
        StatusCode::kUnsupported,
        fmt::format("required NVIDIA decoder {} is unavailable", decoder_name));
  }

  ffmpeg::CodecContext candidate(codec);
  CheckFfmpeg(avcodec_parameters_to_context(candidate.get(), codec_parameters),
              "failed to copy video stream parameters");

  AVBufferRef* decoder_device = av_buffer_ref(device_context.buffer().get());
  if (decoder_device == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to reference the CUDA device context");
  }

  AVCodecContext* raw_decoder = candidate.get();
  raw_decoder->hw_device_ctx = decoder_device;
  raw_decoder->get_format = SelectCudaPixelFormat;
  raw_decoder->pkt_timebase = stream.time_base;
  CheckFfmpeg(avcodec_open2(raw_decoder, codec, nullptr),
              "failed to open the NVIDIA video decoder");

  packet_time_base_ = stream.time_base;
  stream_index_ = stream.index;
  device_id_ = device_context.device_id();
  input_width_ = raw_decoder->width;
  input_height_ = raw_decoder->height;
  decoder_.emplace(std::move(candidate));
}

bool NvDecoder::Send(const ffmpeg::Packet& packet) {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "NvDecoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "cannot send a packet after end of stream");
  }

  const AVPacket* raw_packet = packet.get();
  if (raw_packet == nullptr || raw_packet->data == nullptr ||
      raw_packet->size <= 0 || raw_packet->stream_index != stream_index_) {
    ThrowError(StatusCode::kInvalidArgument,
               "packet does not belong to this video stream");
  }

  const int result = avcodec_send_packet(decoder_->get(), raw_packet);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send a packet to NVDEC");
  return true;
}

bool NvDecoder::SendEndOfStream() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "NvDecoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "NVDEC end of stream was already sent");
  }

  const int result = avcodec_send_packet(decoder_->get(), nullptr);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send the NVDEC drain packet");
  drain_sent_ = true;
  return true;
}

std::optional<ffmpeg::Frame> NvDecoder::Receive() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "NvDecoder has not been opened");
  }

  ffmpeg::Frame frame;
  const int result = avcodec_receive_frame(decoder_->get(), frame.get());
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return std::nullopt;
  }
  CheckFfmpeg(result, "failed to receive an NVDEC frame");

  AVFrame* raw_frame = frame.get();
  ValidateHardwareFrame(*raw_frame, decoder_->get()->hw_device_ctx);
  if (raw_frame->width != input_width_ || raw_frame->height != input_height_) {
    ThrowError(StatusCode::kUnsupported,
               "input video resolution changed while decoding");
  }
  raw_frame->time_base = packet_time_base_;
  return frame;
}

MwGpuNv12FrameView NvDecoder::View(const ffmpeg::Frame& frame) const noexcept {
  return MakeInputGpuNv12FrameView(frame, device_id_);
}

bool NvDecoder::is_open() const noexcept {
  return decoder_.has_value() && decoder_->get() != nullptr;
}

int NvDecoder::stream_index() const noexcept { return stream_index_; }

}  // namespace mw::streamer::internal
