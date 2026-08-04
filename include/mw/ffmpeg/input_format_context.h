#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_INPUT_FORMAT_CONTEXT_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_INPUT_FORMAT_CONTEXT_H_

#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace mw::streamer::ffmpeg {

class Packet;

// Owns one FFmpeg input format context. The interrupt callback and its opaque
// pointee are borrowed until destruction. This type is not synchronized and
// must be opened, read, and destroyed by one owner thread.
class InputFormatContext final {
 public:
  explicit InputFormatContext(const std::string& input,
                              AVIOInterruptCB interrupt_callback = {});
  ~InputFormatContext();

  InputFormatContext(const InputFormatContext&) = delete;
  InputFormatContext& operator=(const InputFormatContext&) = delete;
  InputFormatContext(InputFormatContext&& other) noexcept;
  InputFormatContext& operator=(InputFormatContext&& other) noexcept;

  const AVFormatContext* get() const noexcept;
  AVFormatContext* get() noexcept;
  const AVFormatContext* operator->() const noexcept;
  AVFormatContext* operator->() noexcept;

  void FindStreamInfo();
  // The caller must Unref a reused packet before reading. Returns false at
  // natural EOF and throws for every other read error.
  bool ReadPacket(Packet& packet);

 private:
  AVFormatContext* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_INPUT_FORMAT_CONTEXT_H_
