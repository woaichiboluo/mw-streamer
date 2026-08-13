#ifndef MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_HOST_FRAME_H_
#define MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_HOST_FRAME_H_

#include <memory>

#include "mw/processor/processor.h"

namespace mw::streamer::opencv_adapter {

class HostMatAdapter;

// Owns a tight Host copy of a linear video frame. The copied frame preserves
// the source pixel format and metadata and no longer depends on the source
// callback or its storage lifetime.
class HostFrame final {
 public:
  static HostFrame CopyFrom(const MwStreamerVideoFrameView& source);

  ~HostFrame();

  HostFrame(const HostFrame&) = delete;
  HostFrame& operator=(const HostFrame&) = delete;
  HostFrame(HostFrame&& other) noexcept;
  HostFrame& operator=(HostFrame&& other) noexcept;

  // Synchronously copies this frame into a matching writable output buffer.
  // Host and CUDA linear destinations are supported.
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

}  // namespace mw::streamer::opencv_adapter

#endif  // MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_HOST_FRAME_H_
