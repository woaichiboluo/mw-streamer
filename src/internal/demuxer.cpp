#include "mw/streamer/internal/demuxer.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "fmt/format.h"
#include "spdlog/spdlog.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include "mw/streamer/ffmpeg/dictionary.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/ffmpeg_log.h"

namespace mw::streamer::internal {
namespace {

void InitializeNetwork() {
  static const int result = avformat_network_init();
  CheckFfmpeg(result, "failed to initialize FFmpeg networking");
}

void CheckOpenOperation(int result, FfmpegInterrupt* interrupt,
                        std::string_view operation,
                        std::string_view unavailable_message) {
  if (result >= 0) {
    return;
  }
  if (interrupt != nullptr &&
      (interrupt->is_cancelled() || interrupt->timed_out())) {
    ThrowError(StatusCode::kUnavailable, unavailable_message, result);
  }
  ThrowError(StatusCode::kFfmpegError, operation, result);
}

StreamInfo SnapshotStream(const AVStream& stream) {
  if (stream.codecpar == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "input stream is missing codec parameters");
  }

  StreamInfo result;
  result.index = stream.index;
  result.time_base = stream.time_base;
  result.frame_rate = stream.r_frame_rate;
  CheckFfmpeg(
      avcodec_parameters_copy(result.codec_parameters.get(), stream.codecpar),
      "failed to copy input stream parameters");
  return result;
}

}  // namespace

void Demuxer::Open(std::string_view url) { Open(url, {}); }

void Demuxer::Open(std::string_view url,
                   const DemuxerOpenOptions& open_options) {
  if (is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "Demuxer can only be opened once");
  }
  if (url.empty()) {
    ThrowError(StatusCode::kInvalidArgument, "input URL is empty");
  }

  InstallFfmpegLogBridge();
  InitializeNetwork();
  ffmpeg::InputFormatContext candidate;
  if (open_options.interrupt != nullptr) {
    candidate.get()->interrupt_callback = open_options.interrupt->callback();
    open_options.interrupt->Arm(open_options.open_timeout);
  }
  ffmpeg::Dictionary options;
  for (const auto& [key, value] : open_options.options) {
    options.Set(key, value);
  }
  const std::string null_terminated_url(url);
  try {
    CheckOpenOperation(
        avformat_open_input(candidate.inout(), null_terminated_url.c_str(),
                            nullptr, options.inout()),
        open_options.interrupt, "failed to open input",
        "input open was cancelled or timed out");
    CheckOpenOperation(avformat_find_stream_info(candidate.get(), nullptr),
                       open_options.interrupt,
                       "failed to read input stream information",
                       "input stream discovery was cancelled or timed out");
  } catch (...) {
    if (open_options.interrupt != nullptr) {
      open_options.interrupt->Disarm();
    }
    throw;
  }
  if (open_options.interrupt != nullptr) {
    open_options.interrupt->Disarm();
  }
  for (const std::string& option : options.keys()) {
    spdlog::warn("input demuxer did not consume option '{}'", option);
  }

  StreamInfo video_stream;
  StreamInfo audio_stream;
  const AVFormatContext* context = candidate.get();
  for (unsigned int index = 0; index < context->nb_streams; ++index) {
    const AVStream* stream = context->streams[index];
    if (stream == nullptr || stream->codecpar == nullptr) {
      ThrowError(StatusCode::kFfmpegError, "input contains an invalid stream");
    }

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (video_stream.index >= 0) {
        ThrowError(StatusCode::kUnsupported,
                   "input contains more than one video stream");
      }
      video_stream = SnapshotStream(*stream);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      if (audio_stream.index >= 0) {
        ThrowError(StatusCode::kUnsupported,
                   "input contains more than one audio stream");
      }
      audio_stream = SnapshotStream(*stream);
    }
  }

  if (video_stream.index < 0) {
    ThrowError(StatusCode::kNotFound, "input does not contain a video stream");
  }
  const AVCodecID video_codec_id =
      video_stream.codec_parameters.get()->codec_id;
  if (video_codec_id != AV_CODEC_ID_H264 &&
      video_codec_id != AV_CODEC_ID_HEVC) {
    ThrowError(StatusCode::kUnsupported,
               fmt::format("unsupported input video codec id {}",
                           static_cast<int>(video_codec_id)));
  }

  format_context_.emplace(std::move(candidate));
  video_stream_ = std::move(video_stream);
  audio_stream_ = std::move(audio_stream);
  interrupt_ = open_options.interrupt;
  read_timeout_ = open_options.read_timeout;
}

std::optional<ffmpeg::Packet> Demuxer::Read() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "Demuxer has not been opened");
  }

  ffmpeg::Packet candidate;
  while (true) {
    if (interrupt_ != nullptr) {
      interrupt_->Arm(read_timeout_);
    }
    const int read_result =
        av_read_frame(format_context_->get(), candidate.get());
    if (interrupt_ != nullptr) {
      interrupt_->Disarm();
    }
    if (read_result == AVERROR(EAGAIN)) {
      candidate.Unref();
      continue;
    }
    if (read_result == AVERROR_EOF) {
      return std::nullopt;
    }
    if (read_result < 0) {
      if (interrupt_ != nullptr &&
          (interrupt_->is_cancelled() || interrupt_->timed_out())) {
        ThrowError(StatusCode::kUnavailable,
                   "input read was cancelled or timed out", read_result);
      }
      ThrowError(StatusCode::kFfmpegError, "failed to read an input packet",
                 read_result);
    }

    const int stream_index = candidate.get()->stream_index;
    if (stream_index != video_stream_.index &&
        (!has_audio() || stream_index != audio_stream_.index)) {
      candidate.Unref();
      continue;
    }

    candidate.get()->time_base = stream_index == video_stream_.index
                                     ? video_stream_.time_base
                                     : audio_stream_.time_base;
    return std::optional<ffmpeg::Packet>{std::move(candidate)};
  }
}

bool Demuxer::is_open() const noexcept { return format_context_.has_value(); }

const StreamInfo& Demuxer::video_stream() const noexcept {
  return video_stream_;
}

bool Demuxer::has_audio() const noexcept { return audio_stream_.index >= 0; }

const StreamInfo& Demuxer::audio_stream() const noexcept {
  return audio_stream_;
}

}  // namespace mw::streamer::internal
