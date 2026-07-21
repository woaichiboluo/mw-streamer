#ifndef MW_STREAMER_INTERNAL_FRAGMENTED_MP4_MUXER_H_
#define MW_STREAMER_INTERNAL_FRAGMENTED_MP4_MUXER_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/output_format_context.h"
#include "mw/streamer/ffmpeg/packet.h"

namespace mw::streamer::internal {

class FragmentedMp4Muxer final {
 public:
  FragmentedMp4Muxer() = default;
  ~FragmentedMp4Muxer() noexcept;

  FragmentedMp4Muxer(const FragmentedMp4Muxer&) = delete;
  FragmentedMp4Muxer& operator=(const FragmentedMp4Muxer&) = delete;
  FragmentedMp4Muxer(FragmentedMp4Muxer&&) = delete;
  FragmentedMp4Muxer& operator=(FragmentedMp4Muxer&&) = delete;

  void Open(const std::filesystem::path& path,
            const ffmpeg::CodecParameters& video_parameters,
            AVRational video_time_base,
            const ffmpeg::CodecParameters* audio_parameters = nullptr,
            AVRational audio_time_base = {0, 1});
  void WriteVideo(ffmpeg::Packet packet);
  void WriteAudio(ffmpeg::Packet packet);
  void Finish();

  [[nodiscard]] bool is_open() const noexcept;

 private:
  void Write(ffmpeg::Packet packet, AVStream* stream,
             std::optional<std::int64_t>* previous_dts);
  void CloseIo() noexcept;

  std::optional<ffmpeg::OutputFormatContext> format_context_;
  std::FILE* file_ = nullptr;
  AVIOContext* io_context_ = nullptr;
  AVStream* video_stream_ = nullptr;
  AVStream* audio_stream_ = nullptr;
  std::optional<std::int64_t> previous_video_dts_;
  std::optional<std::int64_t> previous_audio_dts_;
  bool header_written_ = false;
  bool finished_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_FRAGMENTED_MP4_MUXER_H_
