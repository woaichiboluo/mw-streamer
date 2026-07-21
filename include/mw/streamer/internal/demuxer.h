#ifndef MW_STREAMER_INTERNAL_DEMUXER_H_
#define MW_STREAMER_INTERNAL_DEMUXER_H_

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "mw/streamer/ffmpeg/input_format_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/common/ffmpeg_interrupt.h"
#include "mw/streamer/internal/stream_info.h"

namespace mw::streamer::internal {

struct DemuxerOpenOptions {
  std::map<std::string, std::string> options;
  FfmpegInterrupt* interrupt = nullptr;
  std::chrono::milliseconds open_timeout{0};
  std::chrono::milliseconds read_timeout{0};
};

class Demuxer final {
 public:
  void Open(std::string_view url);
  void Open(std::string_view url, const DemuxerOpenOptions& options);
  [[nodiscard]] std::optional<ffmpeg::Packet> Read();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const StreamInfo& video_stream() const noexcept;
  [[nodiscard]] bool has_audio() const noexcept;
  [[nodiscard]] const StreamInfo& audio_stream() const noexcept;

 private:
  std::optional<ffmpeg::InputFormatContext> format_context_;
  StreamInfo video_stream_;
  StreamInfo audio_stream_;
  FfmpegInterrupt* interrupt_ = nullptr;
  std::chrono::milliseconds read_timeout_{0};
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_DEMUXER_H_
