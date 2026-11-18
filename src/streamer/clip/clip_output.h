#ifndef MW_SRC_STREAMER_CLIP_CLIP_OUTPUT_H_
#define MW_SRC_STREAMER_CLIP_CLIP_OUTPUT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "mw/streamer/clip/clip.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

// Optional live encoded-packet downstream. Control Start/Stop externally;
// all other entry points are concurrent and contain/log their own failures.
// No disk IO or Recorder joins execute on mapping/packet/submission callers.
class ClipOutput final {
 public:
  explicit ClipOutput(ClipConfig config);
  ~ClipOutput();
  ClipOutput(const ClipOutput&) = delete;
  ClipOutput& operator=(const ClipOutput&) = delete;

  void Start(const std::vector<ffmpeg::StreamInfo>& streams);
  void Stop() noexcept;
  void SetGeneration(std::uint64_t generation) noexcept;
  std::uint64_t generation() const noexcept;
  // Called immediately before video encoder admission. Repeated admission
  // attempts are harmless; a mapping alone does not imply an encoded frame.
  void MapVideo(std::uint64_t generation, const ffmpeg::Frame& source,
                const ffmpeg::Frame& output) noexcept;
  // Marks the first audio timeline position in this source generation,
  // excluding delayed encoder packets from the previous generation.
  void MapAudio(std::uint64_t generation, const ffmpeg::Frame& output) noexcept;
  void SubmitPacket(const ffmpeg::Packet& packet) noexcept;
  void SubmitClip(const ClipRequest& request) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_CLIP_CLIP_OUTPUT_H_
