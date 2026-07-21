#include "mw/streamer/internal/gpu_frame_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"
#include "mw/streamer/internal/video_color.h"

namespace mw::streamer::internal {

ffmpeg::Frame CreateBlackGpuNv12Frame(GpuNv12FramePool* frame_pool,
                                      AVColorRange color_range,
                                      AVColorSpace color_space) {
  if (frame_pool == nullptr || !frame_pool->is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "black GPU frame requires a valid NV12 frame pool");
  }
  if (color_range != AVCOL_RANGE_MPEG && color_range != AVCOL_RANGE_JPEG) {
    ThrowError(StatusCode::kInvalidArgument,
               "black GPU frame requires a limited or full color range");
  }

  ffmpeg::Frame software_frame;
  AVFrame* raw_software_frame = software_frame.get();
  raw_software_frame->format = AV_PIX_FMT_NV12;
  raw_software_frame->width = frame_pool->width();
  raw_software_frame->height = frame_pool->height();
  raw_software_frame->color_range = color_range;
  raw_software_frame->colorspace = color_space;
  CheckFfmpeg(av_frame_get_buffer(raw_software_frame, 32),
              "failed to allocate a software black NV12 frame");
  software_frame.MakeWritable();

  const std::uint8_t y_value = color_range == AVCOL_RANGE_JPEG
                                   ? static_cast<std::uint8_t>(0)
                                   : static_cast<std::uint8_t>(16);
  for (int row = 0; row < raw_software_frame->height; ++row) {
    std::fill_n(
        raw_software_frame->data[0] + row * raw_software_frame->linesize[0],
        static_cast<std::size_t>(raw_software_frame->width), y_value);
  }
  for (int row = 0; row < raw_software_frame->height / 2; ++row) {
    std::fill_n(
        raw_software_frame->data[1] + row * raw_software_frame->linesize[1],
        static_cast<std::size_t>(raw_software_frame->width),
        static_cast<std::uint8_t>(128));
  }

  ffmpeg::Frame gpu_frame = frame_pool->Acquire();
  CheckFfmpeg(av_hwframe_transfer_data(gpu_frame.get(), raw_software_frame, 0),
              "failed to upload the black NV12 frame to the NVIDIA device");
  gpu_frame.get()->color_range = color_range;
  gpu_frame.get()->colorspace = color_space;
  return gpu_frame;
}

MwGpuNv12FrameView MakeInputGpuNv12FrameView(const ffmpeg::Frame& frame,
                                             int device_id) noexcept {
  const AVFrame* raw_frame = frame.get();
  if (raw_frame == nullptr) {
    return {};
  }

  MwGpuNv12FrameView result{};
  result.device_id = device_id;
  result.width = raw_frame->width;
  result.height = raw_frame->height;
  result.pts = raw_frame->pts;
  result.time_base = {raw_frame->time_base.num, raw_frame->time_base.den};
  result.pts_us = ToMicroseconds(raw_frame->pts, raw_frame->time_base);
  result.y = raw_frame->data[0];
  result.uv = raw_frame->data[1];
  result.y_pitch_bytes = static_cast<std::size_t>(raw_frame->linesize[0]);
  result.uv_pitch_bytes = static_cast<std::size_t>(raw_frame->linesize[1]);
  result.color_range = ToMwColorRange(raw_frame->color_range);
  result.color_space = ToMwColorSpace(raw_frame->colorspace);
  return result;
}

MwGpuNv12FrameView MakeOutputGpuNv12FrameView(
    ffmpeg::Frame* frame, int device_id, const MwVideoOutputInfo& output_info) {
  if (frame == nullptr || frame->get() == nullptr || device_id < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "output GPU frame view requires a valid frame and device");
  }
  AVFrame* raw_frame = frame->get();
  if (raw_frame->format != AV_PIX_FMT_CUDA ||
      raw_frame->width != output_info.width ||
      raw_frame->height != output_info.height ||
      raw_frame->hw_frames_ctx == nullptr || raw_frame->data[0] == nullptr ||
      raw_frame->data[1] == nullptr || raw_frame->linesize[0] <= 0 ||
      raw_frame->linesize[1] <= 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "output GPU frame does not match the NV12 output contract");
  }
  const auto* frames_context = reinterpret_cast<const AVHWFramesContext*>(
      raw_frame->hw_frames_ctx->data);
  if (frames_context == nullptr || frames_context->format != AV_PIX_FMT_CUDA ||
      frames_context->sw_format != AV_PIX_FMT_NV12) {
    ThrowError(StatusCode::kInvalidArgument,
               "output GPU frame is not CUDA-backed NV12");
  }

  raw_frame->color_range = ToAvColorRange(output_info.color_range);
  raw_frame->colorspace = ToAvColorSpace(output_info.color_space);

  MwGpuNv12FrameView result{};
  result.device_id = device_id;
  result.width = raw_frame->width;
  result.height = raw_frame->height;
  result.pts = 0;
  result.time_base = {0, 0};
  result.pts_us = 0;
  result.y = raw_frame->data[0];
  result.uv = raw_frame->data[1];
  result.y_pitch_bytes = static_cast<std::size_t>(raw_frame->linesize[0]);
  result.uv_pitch_bytes = static_cast<std::size_t>(raw_frame->linesize[1]);
  result.color_range = output_info.color_range;
  result.color_space = output_info.color_space;
  return result;
}

}  // namespace mw::streamer::internal
