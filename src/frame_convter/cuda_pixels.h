#ifndef MW_FRAME_CONVTER_CUDA_PIXELS_H_
#define MW_FRAME_CONVTER_CUDA_PIXELS_H_

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>

namespace mw::frame_convter::internal {

struct ColorMatrix {
  float values[3][4];
};

// The caller supplies valid device buffers, positive even dimensions, and byte
// strides. Chroma positions are measured in luma pixels. Matrices apply an
// affine transform to actual code values: YUV [0,255]/[0,1023], RGB [0,255].
// Calls enqueue work on stream; the caller owns synchronization and lifetimes.
cudaError_t YuvToRgb(const std::uint8_t* y, std::size_t y_step,
                     const std::uint8_t* uv, std::size_t uv_step, bool p010,
                     int width, int height, float chroma_x, float chroma_y,
                     const ColorMatrix& matrix, std::uint8_t* dst,
                     std::size_t dst_step, cudaStream_t stream);

cudaError_t RgbToYuv(const std::uint8_t* src, std::size_t src_step,
                     std::uint8_t* y, std::size_t y_step, std::uint8_t* uv,
                     std::size_t uv_step, bool p010, int width, int height,
                     float chroma_x, float chroma_y, const ColorMatrix& matrix,
                     cudaStream_t stream);

}  // namespace mw::frame_convter::internal

#endif  // MW_FRAME_CONVTER_CUDA_PIXELS_H_
