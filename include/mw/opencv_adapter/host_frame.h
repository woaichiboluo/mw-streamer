#ifndef MW_OPENCV_ADAPTER_HOST_FRAME_H_
#define MW_OPENCV_ADAPTER_HOST_FRAME_H_

#include <cuda.h>

#include <memory>

#include "mw/export.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

class HostMatAdapter;

// Owns a tight Host copy of a linear Host video frame. The copied frame
// preserves the source pixel format and metadata and no longer depends on the
// source callback or its storage lifetime.
class MW_OPENCV_ADAPTER_API HostFrame final {
 public:
  // Copies tight positive-stride Host planes into a matching Host output.
  // Contiguous planes are copied in one memcpy; no row-by-row fallback exists.
  static void Copy(const MwStreamerVideoFrameView& source,
                   const MwStreamerVideoBufferView& destination);

  // Allocates ordinary CPU storage and copies a tight Host frame into it.
  static HostFrame CopyFrom(const MwStreamerVideoFrameView& source);

  // Allocates reusable page-locked storage for asynchronous CUDA transfers.
  // This is a setup operation; context must remain alive until destruction.
  // No source pixels are copied.
  static HostFrame AllocatePinned(const MwStreamerVideoFrameView& prototype,
                                  CUcontext context);

  ~HostFrame();

  HostFrame(const HostFrame&) = delete;
  HostFrame& operator=(const HostFrame&) = delete;
  HostFrame(HostFrame&& other) noexcept;
  HostFrame& operator=(HostFrame&& other) noexcept;

  // Copies this frame into a matching tight Host output buffer.
  void CopyTo(const MwStreamerVideoBufferView& destination) const;

  // The returned view remains valid until this HostFrame is moved from or
  // destroyed. A moved-from HostFrame may only be assigned to or destroyed.
  const MwStreamerVideoFrameView& view() const noexcept;

 private:
  friend class HostMatAdapter;

  class Impl;

  static HostFrame AllocateLike(const MwStreamerVideoFrameView& prototype);

  explicit HostFrame(std::unique_ptr<Impl> impl) noexcept;

  MwStreamerVideoFrameView& mutable_view() noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_HOST_FRAME_H_
