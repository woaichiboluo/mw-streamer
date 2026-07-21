#ifndef MW_STREAMER_INTERNAL_GPU_FRAME_UTILS_H_
#define MW_STREAMER_INTERNAL_GPU_FRAME_UTILS_H_

extern "C" {
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"
#include "mw/streamer/processor.h"

namespace mw::streamer::internal {

[[nodiscard]] ffmpeg::Frame CreateBlackGpuNv12Frame(
    GpuNv12FramePool* frame_pool, AVColorRange color_range,
    AVColorSpace color_space);

[[nodiscard]] MwGpuNv12FrameView MakeInputGpuNv12FrameView(
    const ffmpeg::Frame& frame, int device_id) noexcept;

[[nodiscard]] MwGpuNv12FrameView MakeOutputGpuNv12FrameView(
    ffmpeg::Frame* frame, int device_id, const MwVideoOutputInfo& output_info);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_GPU_FRAME_UTILS_H_
