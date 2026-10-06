#include "mw/streamer/ffmpeg/hw_device_context.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;

// Test the wrapper's ownership without requiring a GPU or initializing a
// hardware backend. Real CUDA creation is verified separately below.
AVBufferRef* MakeTrackedReference(int& releases) {
  auto* context =
      static_cast<AVHWDeviceContext*>(av_mallocz(sizeof(AVHWDeviceContext)));
  if (!context) {
    throw std::bad_alloc();
  }
  auto* reference = av_buffer_create(
      reinterpret_cast<uint8_t*>(context), sizeof(*context),
      [](void* opaque, uint8_t* data) {
        ++*static_cast<int*>(opaque);
        av_free(data);
      },
      &releases, 0);
  if (!reference) {
    av_free(context);
    throw std::bad_alloc();
  }
  return reference;
}

TEST_CASE("硬件设备在最后一个引用释放后销毁", "[ffmpeg][hw_device]") {
  int releases = 0;
  {
    auto retained = [&] {
      ffmpeg::HwDeviceContext owner(MakeTrackedReference(releases));
      auto shared = owner.Ref();
      CHECK(shared.get() != owner.get());
      CHECK(shared.get()->data == owner.get()->data);
      CHECK(av_buffer_get_ref_count(owner.get()) == 2);
      return shared;
    }();
    CHECK(releases == 0);
    CHECK(av_buffer_get_ref_count(retained.get()) == 1);
  }
  CHECK(releases == 1);
}

TEST_CASE("硬件设备赋值释放旧设备并共享新设备", "[ffmpeg][hw_device]") {
  int old_releases = 0;
  int new_releases = 0;
  {
    ffmpeg::HwDeviceContext destination(MakeTrackedReference(old_releases));
    ffmpeg::HwDeviceContext source(MakeTrackedReference(new_releases));
    destination = source;
    CHECK(old_releases == 1);
    CHECK(new_releases == 0);
    CHECK(destination.get()->data == source.get()->data);
    CHECK(av_buffer_get_ref_count(source.get()) == 2);
    destination = destination;
    CHECK(av_buffer_get_ref_count(source.get()) == 2);
  }
  CHECK(old_releases == 1);
  CHECK(new_releases == 1);
}

TEST_CASE("移动硬件设备转移引用而不重建或增加引用", "[ffmpeg][hw_device]") {
  int old_releases = 0;
  int new_releases = 0;
  {
    ffmpeg::HwDeviceContext source(MakeTrackedReference(new_releases));
    auto* reference = source.get();
    ffmpeg::HwDeviceContext moved(std::move(source));
    CHECK(source.get() == nullptr);
    CHECK(moved.get() == reference);
    CHECK(av_buffer_get_ref_count(moved.get()) == 1);
    ffmpeg::HwDeviceContext destination(MakeTrackedReference(old_releases));
    destination = std::move(moved);
    CHECK(old_releases == 1);
    CHECK(moved.get() == nullptr);
    CHECK(destination.get() == reference);
    CHECK(av_buffer_get_ref_count(destination.get()) == 1);
  }
  CHECK(new_releases == 1);
}

TEST_CASE("硬件设备创建失败通过异常报告", "[ffmpeg][hw_device]") {
  CHECK_THROWS_AS(ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCuda, "99999"),
                  ffmpeg::FfmpegException);
  CHECK_THROWS_AS(
      ffmpeg::HwDeviceContext(static_cast<ffmpeg::HwDeviceType>(-1)),
      std::invalid_argument);
  CHECK_THROWS_AS(ffmpeg::HwDeviceContext(static_cast<AVBufferRef*>(nullptr)),
                  std::invalid_argument);
}

TEST_CASE("CPU上下文无需硬件设备即可复制引用和移动", "[ffmpeg][hw_device]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  CHECK(cpu.get() == nullptr);
  auto copied = cpu;
  CHECK(copied.get() == nullptr);
  auto referenced = cpu.Ref();
  CHECK(referenced.get() == nullptr);
  auto moved = std::move(copied);
  CHECK(moved.get() == nullptr);
  CHECK(copied.get() == nullptr);
  int releases = 0;
  ffmpeg::HwDeviceContext destination(MakeTrackedReference(releases));
  destination = cpu;
  CHECK(releases == 1);
  CHECK(destination.get() == nullptr);
  destination = std::move(referenced);
  CHECK(destination.get() == nullptr);
}

// Requires an NVIDIA GPU and driver. Run explicitly with the [cuda] filter.
TEST_CASE("CUDA设备引用共享同一个原生上下文", "[.][ffmpeg][cuda]") {
  auto retained = [] {
    // Use a non-null-terminated view to verify the wrapper copies the device
    // string before passing it to FFmpeg.
    const std::string_view device("09", 1);
    ffmpeg::HwDeviceContext owner(ffmpeg::HwDeviceType::kCuda, device);
    const auto* native =
        reinterpret_cast<const AVHWDeviceContext*>(owner.get()->data);
    REQUIRE(native->type == AV_HWDEVICE_TYPE_CUDA);
    REQUIRE(native->hwctx != nullptr);
    ffmpeg::HwDeviceContext decoder = owner;
    auto encoder = owner.Ref();
    CHECK(decoder.get()->data == owner.get()->data);
    CHECK(encoder.get()->data == owner.get()->data);
    const auto* shared_native =
        reinterpret_cast<const AVHWDeviceContext*>(encoder.get()->data);
    CHECK(shared_native->hwctx == native->hwctx);
    CHECK(av_buffer_get_ref_count(owner.get()) == 3);
    return encoder;
  }();
  CHECK(av_buffer_get_ref_count(retained.get()) == 1);
  const auto* native =
      reinterpret_cast<const AVHWDeviceContext*>(retained.get()->data);
  CHECK(native->type == AV_HWDEVICE_TYPE_CUDA);
  CHECK(native->hwctx != nullptr);

  // The original owner and both temporary consumers have gone away. Allocate
  // a real GPU frame through the remaining reference to exercise CUcontext.
  const std::unique_ptr<AVBufferRef, void (*)(AVBufferRef*)> pool(
      av_hwframe_ctx_alloc(retained.get()),
      [](AVBufferRef* reference) { av_buffer_unref(&reference); });
  REQUIRE(pool != nullptr);
  auto* frames = reinterpret_cast<AVHWFramesContext*>(pool->data);
  frames->format = AV_PIX_FMT_CUDA;
  frames->sw_format = AV_PIX_FMT_NV12;
  frames->width = 64;
  frames->height = 64;
  ffmpeg::FfmpegException::throwIfError(av_hwframe_ctx_init(pool.get()),
                                        "初始化CUDA帧池");
  ffmpeg::Frame frame;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_get_buffer(pool.get(), frame.get(), 0), "分配CUDA帧");
  CHECK(frame->format == AV_PIX_FMT_CUDA);
  CHECK(frame->data[0] != nullptr);
  const auto* frame_context =
      reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  CHECK(frame_context->device_ctx == native);
}

}  // namespace
