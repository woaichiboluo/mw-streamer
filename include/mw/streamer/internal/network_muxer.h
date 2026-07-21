#ifndef MW_STREAMER_INTERNAL_NETWORK_MUXER_H_
#define MW_STREAMER_INTERNAL_NETWORK_MUXER_H_

#include <chrono>
#include <cstdint>
#include <optional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/output_format_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/common/ffmpeg_interrupt.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {

class NetworkMuxer final {
 public:
  NetworkMuxer() = default;
  ~NetworkMuxer() noexcept;

  NetworkMuxer(const NetworkMuxer&) = delete;
  NetworkMuxer& operator=(const NetworkMuxer&) = delete;
  NetworkMuxer(NetworkMuxer&&) = delete;
  NetworkMuxer& operator=(NetworkMuxer&&) = delete;

  void Open(const OutputConfig& config,
            const ffmpeg::CodecParameters& video_parameters,
            AVRational video_time_base,
            const ffmpeg::CodecParameters* audio_parameters = nullptr,
            AVRational audio_time_base = {0, 1},
            FfmpegInterrupt* interrupt = nullptr);
  void WriteVideo(ffmpeg::Packet packet);
  void WriteAudio(ffmpeg::Packet packet);
  void Finish();
  void Abort() noexcept;

  [[nodiscard]] bool is_open() const noexcept;

 private:
  void Write(ffmpeg::Packet packet, AVStream* stream,
             std::optional<std::int64_t>* previous_dts);
  [[nodiscard]] int CloseIo() noexcept;

  std::optional<ffmpeg::OutputFormatContext> format_context_;
  AVStream* video_stream_ = nullptr;
  AVStream* audio_stream_ = nullptr;
  std::optional<std::int64_t> previous_video_dts_;
  std::optional<std::int64_t> previous_audio_dts_;
  FfmpegInterrupt* interrupt_ = nullptr;
  std::chrono::milliseconds write_timeout_{0};
  bool open_attempted_ = false;
  bool header_written_ = false;
  bool owns_io_ = false;
  bool finished_ = false;
  bool aborted_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_NETWORK_MUXER_H_
