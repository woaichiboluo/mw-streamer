#include "mw/streamer/internal/standby_image.h"

#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/input_format_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/ffmpeg_log.h"

namespace mw::streamer::internal {
namespace {

using SwsContextOwner = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;

void ValidateOutputDimensions(int width, int height) {
  if (width <= 0 || height <= 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "standby image output dimensions must be positive");
  }
}

int FindVideoStream(const AVFormatContext& format_context) {
  for (unsigned int index = 0; index < format_context.nb_streams; ++index) {
    const AVStream* stream = format_context.streams[index];
    if (stream != nullptr && stream->codecpar != nullptr &&
        stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      return static_cast<int>(index);
    }
  }
  ThrowError(StatusCode::kNotFound,
             "standby image does not contain a video stream");
}

int ToSwsColorSpace(AVColorSpace color_space) noexcept {
  switch (color_space) {
    case AVCOL_SPC_BT709:
      return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      return SWS_CS_ITU601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return SWS_CS_BT2020;
    default:
      return SWS_CS_DEFAULT;
  }
}

void ConfigureScalerColors(SwsContext* scaler, const AVFrame& source,
                           AVColorRange output_color_range,
                           AVColorSpace output_color_space) {
  const AVPixFmtDescriptor* source_format =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source.format));
  const bool source_is_rgb = source_format != nullptr &&
                             (source_format->flags & AV_PIX_FMT_FLAG_RGB) != 0;
  const int source_range =
      source.color_range == AVCOL_RANGE_JPEG || source_is_rgb ? 1 : 0;
  const int output_range = output_color_range == AVCOL_RANGE_JPEG ? 1 : 0;
  const AVColorSpace source_color_space =
      source.colorspace == AVCOL_SPC_UNSPECIFIED ? output_color_space
                                                 : source.colorspace;
  CheckFfmpeg(
      sws_setColorspaceDetails(
          scaler, sws_getCoefficients(ToSwsColorSpace(source_color_space)),
          source_range,
          sws_getCoefficients(ToSwsColorSpace(output_color_space)),
          output_range, 0, 1 << 16, 1 << 16),
      "failed to configure standby image color conversion");
}

std::optional<ffmpeg::Frame> ReceiveFrame(AVCodecContext* decoder) {
  ffmpeg::Frame frame;
  const int result = avcodec_receive_frame(decoder, frame.get());
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return std::nullopt;
  }
  CheckFfmpeg(result, "failed to decode the standby image");
  return std::optional<ffmpeg::Frame>{std::move(frame)};
}

std::optional<ffmpeg::Frame> SendPacketAndReceiveFrame(AVCodecContext* decoder,
                                                       const AVPacket* packet) {
  while (true) {
    const int result = avcodec_send_packet(decoder, packet);
    if (result == AVERROR(EAGAIN)) {
      if (auto frame = ReceiveFrame(decoder); frame.has_value()) {
        return frame;
      }
      continue;
    }
    CheckFfmpeg(result, "failed to send standby image data to the decoder");
    return ReceiveFrame(decoder);
  }
}

ffmpeg::Frame DecodeFirstFrame(AVFormatContext* format_context,
                               int stream_index, AVCodecContext* decoder) {
  ffmpeg::Packet packet;
  while (true) {
    packet.Unref();
    const int result = av_read_frame(format_context, packet.get());
    if (result == AVERROR(EAGAIN)) {
      continue;
    }
    if (result == AVERROR_EOF) {
      break;
    }
    CheckFfmpeg(result, "failed to read standby image data");
    if (packet.get()->stream_index != stream_index) {
      continue;
    }
    if (auto frame = SendPacketAndReceiveFrame(decoder, packet.get());
        frame.has_value()) {
      return std::move(*frame);
    }
  }

  if (auto frame = SendPacketAndReceiveFrame(decoder, nullptr);
      frame.has_value()) {
    return std::move(*frame);
  }
  ThrowError(StatusCode::kFfmpegError,
             "standby image decoder produced no frame");
}

ffmpeg::Frame ScaleToNv12(const ffmpeg::Frame& source, int output_width,
                          int output_height, AVColorRange output_color_range,
                          AVColorSpace output_color_space) {
  const AVFrame* raw_source = source.get();
  if (raw_source == nullptr || raw_source->width <= 0 ||
      raw_source->height <= 0 || raw_source->format == AV_PIX_FMT_NONE) {
    ThrowError(StatusCode::kFfmpegError,
               "standby image decoder returned an invalid frame");
  }

  SwsContextOwner scaler(
      sws_getContext(raw_source->width, raw_source->height,
                     static_cast<AVPixelFormat>(raw_source->format),
                     output_width, output_height, AV_PIX_FMT_NV12, SWS_BILINEAR,
                     nullptr, nullptr, nullptr),
      &sws_freeContext);
  if (scaler == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to create the standby image scaler");
  }
  ConfigureScalerColors(scaler.get(), *raw_source, output_color_range,
                        output_color_space);

  ffmpeg::Frame result;
  AVFrame* raw_result = result.get();
  raw_result->format = AV_PIX_FMT_NV12;
  raw_result->width = output_width;
  raw_result->height = output_height;
  raw_result->color_range = output_color_range;
  raw_result->colorspace = output_color_space;
  raw_result->color_primaries = raw_source->color_primaries;
  raw_result->color_trc = raw_source->color_trc;
  raw_result->chroma_location = raw_source->chroma_location;
  CheckFfmpeg(av_frame_get_buffer(raw_result, 32),
              "failed to allocate the standby NV12 frame");
  result.MakeWritable();

  const int scaled_height =
      sws_scale(scaler.get(), raw_source->data, raw_source->linesize, 0,
                raw_source->height, raw_result->data, raw_result->linesize);
  CheckFfmpeg(scaled_height, "failed to scale the standby image");
  if (scaled_height != output_height) {
    ThrowError(StatusCode::kFfmpegError,
               "standby image scaler produced an incomplete frame");
  }
  return result;
}

}  // namespace

ffmpeg::Frame DecodeStandbyImageToNv12(std::string_view path, int output_width,
                                       int output_height,
                                       AVColorRange color_range,
                                       AVColorSpace color_space) {
  if (path.empty()) {
    ThrowError(StatusCode::kInvalidArgument,
               "standby image path must not be empty");
  }
  InstallFfmpegLogBridge();
  ValidateOutputDimensions(output_width, output_height);

  ffmpeg::InputFormatContext format_context;
  const std::string null_terminated_path(path);
  CheckFfmpeg(
      avformat_open_input(format_context.inout(), null_terminated_path.c_str(),
                          nullptr, nullptr),
      "failed to open the standby image");
  CheckFfmpeg(avformat_find_stream_info(format_context.get(), nullptr),
              "failed to read standby image stream information");

  const int stream_index = FindVideoStream(*format_context.get());
  const AVCodecParameters* parameters =
      format_context.get()->streams[stream_index]->codecpar;
  if (parameters->codec_id != AV_CODEC_ID_MJPEG &&
      parameters->codec_id != AV_CODEC_ID_PNG) {
    ThrowError(StatusCode::kUnsupported,
               "standby image must be encoded as JPG or PNG");
  }
  const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
  if (codec == nullptr) {
    ThrowError(StatusCode::kUnsupported,
               "standby image decoder is unavailable");
  }

  ffmpeg::CodecContext decoder(codec);
  CheckFfmpeg(avcodec_parameters_to_context(decoder.get(), parameters),
              "failed to copy standby image codec parameters");
  CheckFfmpeg(avcodec_open2(decoder.get(), codec, nullptr),
              "failed to open the standby image decoder");

  return ScaleToNv12(
      DecodeFirstFrame(format_context.get(), stream_index, decoder.get()),
      output_width, output_height, color_range, color_space);
}

void StandbyImage::Load(std::string_view path, GpuNv12FramePool* frame_pool,
                        AVColorRange color_range, AVColorSpace color_space) {
  if (is_loaded()) {
    ThrowError(StatusCode::kInvalidArgument,
               "standby image can only be loaded once");
  }
  if (frame_pool == nullptr || !frame_pool->is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "standby image requires a valid GPU NV12 frame pool");
  }

  ffmpeg::Frame software_frame =
      DecodeStandbyImageToNv12(path, frame_pool->width(), frame_pool->height(),
                               color_range, color_space);
  ffmpeg::Frame gpu_frame = frame_pool->Acquire();
  CheckFfmpeg(
      av_hwframe_transfer_data(gpu_frame.get(), software_frame.get(), 0),
      "failed to upload the standby image to the NVIDIA device");
  gpu_frame.get()->color_range = software_frame.get()->color_range;
  gpu_frame.get()->colorspace = software_frame.get()->colorspace;
  gpu_frame.get()->color_primaries = software_frame.get()->color_primaries;
  gpu_frame.get()->color_trc = software_frame.get()->color_trc;
  gpu_frame.get()->chroma_location = software_frame.get()->chroma_location;
  frame_.emplace(std::move(gpu_frame));
}

ffmpeg::Frame StandbyImage::Ref() const {
  if (!is_loaded()) {
    ThrowError(StatusCode::kInvalidArgument,
               "standby image has not been loaded");
  }
  return frame_->Ref();
}

bool StandbyImage::is_loaded() const noexcept { return frame_.has_value(); }

}  // namespace mw::streamer::internal
