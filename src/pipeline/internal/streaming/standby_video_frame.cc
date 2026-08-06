#include "mw/pipeline/internal/streaming/standby_video_frame.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/pixel_format.h"

namespace mw::streamer::pipeline::internal::streaming {
namespace {

using ScaleContext = std::unique_ptr<SwsContext, void (*)(SwsContext*)>;

ffmpeg::Frame DecodeImage(const std::string& path) {
  ffmpeg::InputFormatContext input(path);
  input.FindStreamInfo();
  const int stream_index =
      av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  ffmpeg::ThrowIfError(stream_index, "查找备播图片视频流");

  const auto* parameters = input->streams[stream_index]->codecpar;
  const auto* codec = avcodec_find_decoder(parameters->codec_id);
  if (!codec) {
    throw std::invalid_argument("找不到备播图片解码器");
  }
  ffmpeg::CodecContext decoder(codec);
  ffmpeg::ThrowIfError(avcodec_parameters_to_context(decoder.get(), parameters),
                       "复制备播图片解码参数");
  ffmpeg::ThrowIfError(avcodec_open2(decoder.get(), codec, nullptr),
                       "打开备播图片解码器");

  ffmpeg::Packet packet;
  for (;;) {
    packet.Unref();
    const bool has_packet = input.ReadPacket(packet);
    if (!has_packet) {
      ffmpeg::ThrowIfError(avcodec_send_packet(decoder.get(), nullptr),
                           "提交备播图片结束标记");
    } else if (packet->stream_index != stream_index) {
      continue;
    } else {
      ffmpeg::ThrowIfError(avcodec_send_packet(decoder.get(), packet.get()),
                           "提交备播图片数据");
    }

    ffmpeg::Frame decoded;
    const int receive_result =
        avcodec_receive_frame(decoder.get(), decoded.get());
    if (receive_result == 0) {
      if (decoded->width <= 0 || decoded->height <= 0 ||
          decoded->format == AV_PIX_FMT_NONE ||
          ffmpeg::IsHardwarePixelFormat(
              static_cast<AVPixelFormat>(decoded->format))) {
        throw std::invalid_argument("备播图片解码结果无效");
      }
      return decoded;
    }
    if (receive_result != AVERROR(EAGAIN) && receive_result != AVERROR_EOF) {
      ffmpeg::ThrowIfError(receive_result, "接收备播图片解码帧");
    }
    if (!has_packet) {
      break;
    }
  }
  throw std::invalid_argument("备播图片没有可解码的视频帧");
}

ScaleContext MakeScaleContext(int source_width, int source_height,
                              AVPixelFormat source_format, int target_width,
                              int target_height, AVPixelFormat target_format) {
  auto* context = sws_getContext(source_width, source_height, source_format,
                                 target_width, target_height, target_format,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!context) {
    throw std::runtime_error("创建备播图片像素格式转换器失败");
  }
  return {context, sws_freeContext};
}

ffmpeg::Frame AllocateRgbaCanvas(int width, int height) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_RGBA;
  frame->width = width;
  frame->height = height;
  ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 32),
                       "分配备播RGBA画布");
  for (int row = 0; row < height; ++row) {
    auto* line = frame->data[0] + row * frame->linesize[0];
    for (int column = 0; column < width; ++column) {
      line[column * 4] = 20;
      line[column * 4 + 1] = 25;
      line[column * 4 + 2] = 34;
      line[column * 4 + 3] = 255;
    }
  }
  return frame;
}

const std::array<std::uint8_t, 7>& Glyph(char value) {
  static constexpr std::array<std::uint8_t, 7> kA{0x0e, 0x11, 0x11, 0x1f,
                                                  0x11, 0x11, 0x11};
  static constexpr std::array<std::uint8_t, 7> kD{0x1e, 0x11, 0x11, 0x11,
                                                  0x11, 0x11, 0x1e};
  static constexpr std::array<std::uint8_t, 7> kG{0x0e, 0x11, 0x10, 0x17,
                                                  0x11, 0x11, 0x0f};
  static constexpr std::array<std::uint8_t, 7> kI{0x1f, 0x04, 0x04, 0x04,
                                                  0x04, 0x04, 0x1f};
  static constexpr std::array<std::uint8_t, 7> kL{0x10, 0x10, 0x10, 0x10,
                                                  0x10, 0x10, 0x1f};
  static constexpr std::array<std::uint8_t, 7> kN{0x11, 0x19, 0x19, 0x15,
                                                  0x13, 0x13, 0x11};
  static constexpr std::array<std::uint8_t, 7> kO{0x0e, 0x11, 0x11, 0x11,
                                                  0x11, 0x11, 0x0e};
  static constexpr std::array<std::uint8_t, 7> kDot{0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x06, 0x06};
  static constexpr std::array<std::uint8_t, 7> kSpace{};

  switch (value) {
    case 'A':
      return kA;
    case 'D':
      return kD;
    case 'G':
      return kG;
    case 'I':
      return kI;
    case 'L':
      return kL;
    case 'N':
      return kN;
    case 'O':
      return kO;
    case '.':
      return kDot;
    default:
      return kSpace;
  }
}

void PutPixel(AVFrame* frame, int x, int y) {
  if (x < 0 || y < 0 || x >= frame->width || y >= frame->height) {
    return;
  }
  auto* pixel = frame->data[0] + y * frame->linesize[0] + x * 4;
  pixel[0] = 66;
  pixel[1] = 184;
  pixel[2] = 255;
  pixel[3] = 255;
}

void DrawBuiltInImage(AVFrame* canvas) {
  constexpr std::string_view kText = "LOADING...";
  const int scale =
      std::max(1, std::min(canvas->width / static_cast<int>(kText.size() * 6),
                           canvas->height / 12));
  const int text_width = static_cast<int>(kText.size()) * 6 * scale - scale;
  const int origin_x = (canvas->width - text_width) / 2;
  const int origin_y = std::max(0, (canvas->height - 7 * scale) / 2 - scale);

  for (std::size_t character = 0; character < kText.size(); ++character) {
    const auto& glyph = Glyph(kText[character]);
    const int character_x = origin_x + static_cast<int>(character) * 6 * scale;
    for (int row = 0; row < 7; ++row) {
      for (int column = 0; column < 5; ++column) {
        if ((glyph[row] & (1U << (4 - column))) == 0) {
          continue;
        }
        for (int y = 0; y < scale; ++y) {
          for (int x = 0; x < scale; ++x) {
            PutPixel(canvas, character_x + column * scale + x,
                     origin_y + row * scale + y);
          }
        }
      }
    }
  }

  const int bar_width = std::max(1, std::min(canvas->width / 3, text_width));
  const int bar_height = std::max(2, scale);
  const int bar_x = (canvas->width - bar_width) / 2;
  const int bar_y = std::min(canvas->height - bar_height, origin_y + 9 * scale);
  for (int y = 0; y < bar_height; ++y) {
    for (int x = 0; x < bar_width; ++x) {
      PutPixel(canvas, bar_x + x, bar_y + y);
    }
  }
}

void DrawDecodedImage(AVFrame* canvas, const AVFrame& decoded) {
  int scaled_width = canvas->width;
  int scaled_height =
      static_cast<int>(static_cast<std::int64_t>(decoded.height) *
                       canvas->width / decoded.width);
  if (scaled_height > canvas->height) {
    scaled_height = canvas->height;
    scaled_width = static_cast<int>(static_cast<std::int64_t>(decoded.width) *
                                    canvas->height / decoded.height);
  }
  scaled_width = std::max(scaled_width, 1);
  scaled_height = std::max(scaled_height, 1);

  auto scaler = MakeScaleContext(decoded.width, decoded.height,
                                 static_cast<AVPixelFormat>(decoded.format),
                                 scaled_width, scaled_height, AV_PIX_FMT_RGBA);
  const int offset_x = (canvas->width - scaled_width) / 2;
  const int offset_y = (canvas->height - scaled_height) / 2;
  std::array<std::uint8_t*, 4> destination{};
  std::array<int, 4> linesizes{};
  destination[0] =
      canvas->data[0] + offset_y * canvas->linesize[0] + offset_x * 4;
  linesizes[0] = canvas->linesize[0];
  const int rows =
      sws_scale(scaler.get(), decoded.data, decoded.linesize, 0, decoded.height,
                destination.data(), linesizes.data());
  if (rows != scaled_height) {
    throw std::runtime_error("缩放备播图片未生成完整画面");
  }
}

ffmpeg::Frame ConvertCanvas(const ffmpeg::Frame& canvas,
                            AVPixelFormat target_format) {
  ffmpeg::Frame output;
  output->format = target_format;
  output->width = canvas->width;
  output->height = canvas->height;
  ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 32),
                       "分配备播软件视频帧");
  auto scaler = MakeScaleContext(canvas->width, canvas->height, AV_PIX_FMT_RGBA,
                                 output->width, output->height, target_format);
  const int rows = sws_scale(scaler.get(), canvas->data, canvas->linesize, 0,
                             canvas->height, output->data, output->linesize);
  if (rows != output->height) {
    throw std::runtime_error("转换备播视频帧未生成完整画面");
  }
  return output;
}

}  // namespace

StandbyVideoFrame::StandbyVideoFrame(std::string image_path)
    : image_path_(std::move(image_path)) {}

void StandbyVideoFrame::Prepare(
    const ffmpeg::Frame& prototype,
    const ffmpeg::HardwareContext* hardware_context) {
  if (prepared_) {
    return;
  }
  if (!prototype.get() || prototype->width <= 0 || prototype->height <= 0 ||
      prototype->format == AV_PIX_FMT_NONE) {
    throw std::invalid_argument("不能根据无效原型创建备播视频帧");
  }

  auto canvas = AllocateRgbaCanvas(prototype->width, prototype->height);
  if (image_path_.empty()) {
    DrawBuiltInImage(canvas.get());
  } else {
    const auto image = DecodeImage(image_path_);
    DrawDecodedImage(canvas.get(), *image.get());
  }

  const auto format = static_cast<AVPixelFormat>(prototype->format);
  if (!ffmpeg::IsHardwarePixelFormat(format)) {
    frame_ = ConvertCanvas(canvas, format);
  } else {
    const auto* frames_context =
        ffmpeg::HardwareContext::GetFramesContext(*prototype.get());
    if (format != AV_PIX_FMT_CUDA || !frames_context || !hardware_context ||
        !hardware_context->IsCompatible(*prototype.get())) {
      throw std::invalid_argument("备播暂不支持该硬件视频帧");
    }
    auto software = ConvertCanvas(canvas, frames_context->sw_format);
    ffmpeg::Frame hardware;
    ffmpeg::ThrowIfError(
        av_hwframe_get_buffer(prototype->hw_frames_ctx, hardware.get(), 0),
        "分配CUDA备播视频帧");
    ffmpeg::ThrowIfError(
        av_hwframe_transfer_data(hardware.get(), software.get(), 0),
        "上传CUDA备播视频帧");
    frame_ = std::move(hardware);
  }

  frame_.CopyPropertiesFrom(prototype);
  frame_.ClearCrop();
  frame_->pts = AV_NOPTS_VALUE;
  frame_->duration = 0;
  prepared_ = true;
}

bool StandbyVideoFrame::prepared() const noexcept { return prepared_; }

ffmpeg::Frame StandbyVideoFrame::Ref() const {
  if (!prepared_) {
    throw std::logic_error("备播视频帧尚未准备");
  }
  return frame_.Ref();
}

}  // namespace mw::streamer::pipeline::internal::streaming
