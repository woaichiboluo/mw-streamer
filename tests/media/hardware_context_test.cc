#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"

namespace {

using mw::streamer::ffmpeg::HardwareContext;

TEST_CASE("HardwareContext通过FFmpeg在指定设备创建CUDA上下文") {
  const auto context = HardwareContext::CreateCuda(0);
  const auto other_context = HardwareContext::CreateCuda(0);

  CHECK(context.type() == AV_HWDEVICE_TYPE_CUDA);
  CHECK(context.device_index() == 0);
  REQUIRE(context.get() != nullptr);
  REQUIRE(context.get()->data != nullptr);
  REQUIRE(other_context.get() != nullptr);
  CHECK(other_context.get()->data != context.get()->data);
}

TEST_CASE("HardwareContext拷贝共享FFmpeg硬件设备上下文") {
  const auto original = HardwareContext::CreateCuda(0);
  const auto copy = original;

  CHECK(copy.get() != original.get());
  CHECK(copy.get()->data == original.get()->data);
  CHECK(copy.device_index() == original.device_index());
}

TEST_CASE("FFmpeg引用可独立维持HardwareContext资源生命周期") {
  AVBufferRef* retained_context = nullptr;
  {
    const auto context = HardwareContext::CreateCuda(0);
    retained_context = av_buffer_ref(context.get());
    REQUIRE(retained_context != nullptr);
  }

  const auto* device_context =
      reinterpret_cast<const AVHWDeviceContext*>(retained_context->data);
  REQUIRE(device_context != nullptr);
  CHECK(device_context->type == AV_HWDEVICE_TYPE_CUDA);

  av_buffer_unref(&retained_context);
}

TEST_CASE("HardwareContext可供FFmpeg分配并传输CUDA帧") {
  const auto context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(context.get()));
  REQUIRE(frames_ref != nullptr);

  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = 64;
  frames_context->height = 64;
  frames_context->initial_pool_size = 1;

  const int init_result = av_hwframe_ctx_init(frames_ref);
  if (init_result < 0) {
    av_buffer_unref(&frames_ref);
  }
  REQUIRE(init_result >= 0);

  mw::streamer::ffmpeg::Frame frame;
  const int allocate_result = av_hwframe_get_buffer(frames_ref, frame.get(), 0);
  av_buffer_unref(&frames_ref);
  REQUIRE(allocate_result >= 0);

  mw::streamer::ffmpeg::Frame source;
  source->format = AV_PIX_FMT_NV12;
  source->width = 64;
  source->height = 64;
  REQUIRE(av_frame_get_buffer(source.get(), 32) >= 0);
  REQUIRE(av_frame_make_writable(source.get()) >= 0);
  source->data[0][0] = 0x5a;
  source->data[1][0] = 0xa5;
  REQUIRE(av_hwframe_transfer_data(frame.get(), source.get(), 0) >= 0);

  mw::streamer::ffmpeg::Frame restored;
  restored->format = AV_PIX_FMT_NV12;
  restored->width = 64;
  restored->height = 64;
  REQUIRE(av_frame_get_buffer(restored.get(), 32) >= 0);
  REQUIRE(av_hwframe_transfer_data(restored.get(), frame.get(), 0) >= 0);

  CHECK(restored->data[0][0] == 0x5a);
  CHECK(restored->data[1][0] == 0xa5);
  CHECK(HardwareContext::GetFramesContext(*frame.get()) != nullptr);
  CHECK(context.IsCompatible(*frame.get()));

  const auto other_context = HardwareContext::CreateCuda(0);
  CHECK_FALSE(other_context.IsCompatible(*frame.get()));
  CHECK(HardwareContext::GetFramesContext(*source.get()) == nullptr);
}

TEST_CASE("HardwareContext拒绝无效CUDA设备索引") {
  CHECK_THROWS_AS(HardwareContext::CreateCuda(-1), std::invalid_argument);
  CHECK_THROWS(HardwareContext::CreateCuda(99999));
}

}  // namespace
