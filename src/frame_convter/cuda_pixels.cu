#include <cuda_runtime.h>

#include "cuda_pixels.h"

namespace mw::frame_convter::internal {
namespace {

constexpr int kBlockSize = 16;

template <typename T>
__device__ T* Row(T* data, std::size_t step, int y) {
  return reinterpret_cast<T*>(reinterpret_cast<char*>(data) + step * y);
}

template <typename T>
__device__ const T* Row(const T* data, std::size_t step, int y) {
  return reinterpret_cast<const T*>(reinterpret_cast<const char*>(data) +
                                    step * y);
}

__device__ int ClampIndex(int value, int size) {
  return min(max(value, 0), size - 1);
}

__device__ int Quantize(float value, int maximum) {
  return static_cast<int>(floorf(fminf(fmaxf(value, 0.0f), maximum) + 0.5f));
}

__device__ float ReadCode(const std::uint8_t* row, int index, bool p010) {
  return p010 ? reinterpret_cast<const std::uint16_t*>(row)[index] >> 6
              : row[index];
}

__device__ void WriteCode(std::uint8_t* row, int index, float value,
                          bool p010) {
  if (p010) {
    reinterpret_cast<std::uint16_t*>(row)[index] =
        static_cast<std::uint16_t>(Quantize(value, 1023) << 6);
  } else {
    row[index] = static_cast<std::uint8_t>(Quantize(value, 255));
  }
}

__device__ float SampleChroma(const std::uint8_t* uv, std::size_t step,
                              bool p010, int width, int height, float x,
                              float y, int channel) {
  const int x0 = static_cast<int>(floorf(x));
  const int y0 = static_cast<int>(floorf(y));
  const float fx = x - x0;
  const float fy = y - y0;
  const auto* top = Row(uv, step, ClampIndex(y0, height));
  const auto* bottom = Row(uv, step, ClampIndex(y0 + 1, height));
  const int left = 2 * ClampIndex(x0, width) + channel;
  const int right = 2 * ClampIndex(x0 + 1, width) + channel;
  const float a =
      ReadCode(top, left, p010) * (1.0f - fx) + ReadCode(top, right, p010) * fx;
  const float b = ReadCode(bottom, left, p010) * (1.0f - fx) +
                  ReadCode(bottom, right, p010) * fx;
  return a * (1.0f - fy) + b * fy;
}

__device__ float Transform(const ColorMatrix& matrix, int channel, float a,
                           float b, float c) {
  const float* coefficients = matrix.values[channel];
  return coefficients[0] * a + coefficients[1] * b + coefficients[2] * c +
         coefficients[3];
}

__global__ void YuvToRgbKernel(const std::uint8_t* y, std::size_t y_step,
                               const std::uint8_t* uv, std::size_t uv_step,
                               bool p010, int width, int height, float chroma_x,
                               float chroma_y, ColorMatrix matrix,
                               std::uint8_t* dst, std::size_t dst_step) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int row = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || row >= height) return;
  const float luma = ReadCode(Row(y, y_step, row), x, p010);
  const float u =
      SampleChroma(uv, uv_step, p010, width / 2, height / 2,
                   (x - chroma_x) * 0.5f, (row - chroma_y) * 0.5f, 0);
  const float v =
      SampleChroma(uv, uv_step, p010, width / 2, height / 2,
                   (x - chroma_x) * 0.5f, (row - chroma_y) * 0.5f, 1);
  auto* pixel = Row(dst, dst_step, row) + 3 * x;
  for (int channel = 0; channel < 3; ++channel) {
    pixel[channel] = static_cast<std::uint8_t>(
        Quantize(Transform(matrix, channel, luma, u, v), 255));
  }
}

// Integrate a width-two box over unit-width luma pixel cells. Fractional
// overlap preserves the requested chroma center, including cosited samples.
__device__ float BoxWeight(int index, float center) {
  const float overlap =
      fminf(index + 0.5f, center + 1.0f) - fmaxf(index - 0.5f, center - 1.0f);
  return fmaxf(overlap, 0.0f) * 0.5f;
}

__global__ void RgbToYuvKernel(const std::uint8_t* src, std::size_t src_step,
                               std::uint8_t* y, std::size_t y_step,
                               std::uint8_t* uv, std::size_t uv_step, bool p010,
                               int width, int height, float chroma_x,
                               float chroma_y, ColorMatrix matrix) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int row = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || row >= height) return;
  const auto* pixel = Row(src, src_step, row) + 3 * x;
  WriteCode(Row(y, y_step, row), x,
            Transform(matrix, 0, pixel[0], pixel[1], pixel[2]), p010);
  if (x >= width / 2 || row >= height / 2) return;
  const float center_x = 2 * x + chroma_x;
  const float center_y = 2 * row + chroma_y;
  const int first_x = static_cast<int>(floorf(center_x - 0.5f));
  const int first_y = static_cast<int>(floorf(center_y - 0.5f));
  float average[3] = {};
  for (int dy = 0; dy < 3; ++dy) {
    const int sy = first_y + dy;
    const float wy = BoxWeight(sy, center_y);
    const auto* src_row = Row(src, src_step, ClampIndex(sy, height));
    for (int dx = 0; dx < 3; ++dx) {
      const int sx = first_x + dx;
      const float weight = wy * BoxWeight(sx, center_x);
      const auto* sample = src_row + 3 * ClampIndex(sx, width);
      for (int channel = 0; channel < 3; ++channel) {
        average[channel] += sample[channel] * weight;
      }
    }
  }
  auto* uv_row = Row(uv, uv_step, row);
  for (int channel = 0; channel < 2; ++channel) {
    WriteCode(
        uv_row, 2 * x + channel,
        Transform(matrix, channel + 1, average[0], average[1], average[2]),
        p010);
  }
}

dim3 Grid(int width, int height) {
  return dim3((width + kBlockSize - 1) / kBlockSize,
              (height + kBlockSize - 1) / kBlockSize);
}

}  // namespace

cudaError_t YuvToRgb(const std::uint8_t* y, std::size_t y_step,
                     const std::uint8_t* uv, std::size_t uv_step, bool p010,
                     int width, int height, float chroma_x, float chroma_y,
                     const ColorMatrix& matrix, std::uint8_t* dst,
                     std::size_t dst_step, cudaStream_t stream) {
  YuvToRgbKernel<<<Grid(width, height), dim3(kBlockSize, kBlockSize), 0,
                   stream>>>(y, y_step, uv, uv_step, p010, width, height,
                             chroma_x, chroma_y, matrix, dst, dst_step);
  return cudaGetLastError();
}

cudaError_t RgbToYuv(const std::uint8_t* src, std::size_t src_step,
                     std::uint8_t* y, std::size_t y_step, std::uint8_t* uv,
                     std::size_t uv_step, bool p010, int width, int height,
                     float chroma_x, float chroma_y, const ColorMatrix& matrix,
                     cudaStream_t stream) {
  RgbToYuvKernel<<<Grid(width, height), dim3(kBlockSize, kBlockSize), 0,
                   stream>>>(src, src_step, y, y_step, uv, uv_step, p010, width,
                             height, chroma_x, chroma_y, matrix);
  return cudaGetLastError();
}

}  // namespace mw::frame_convter::internal
