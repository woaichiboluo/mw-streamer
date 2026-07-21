#include "mw/streamer/internal/network_muxer.h"

#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "spdlog/spdlog.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/dictionary.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"

namespace mw::streamer::internal {
namespace {

struct FormatOptions {
  const char* format_name;
  const char* header_option_key;
  const char* header_option_value;
};

class InterruptDeadline final {
 public:
  InterruptDeadline(FfmpegInterrupt* interrupt,
                    std::chrono::milliseconds timeout) noexcept
      : interrupt_(interrupt) {
    if (interrupt_ != nullptr) {
      interrupt_->Arm(timeout);
    }
  }

  ~InterruptDeadline() noexcept {
    if (interrupt_ != nullptr) {
      interrupt_->Disarm();
    }
  }

  InterruptDeadline(const InterruptDeadline&) = delete;
  InterruptDeadline& operator=(const InterruptDeadline&) = delete;

  [[nodiscard]] bool was_interrupted() const noexcept {
    return interrupt_ != nullptr &&
           (interrupt_->is_cancelled() || interrupt_->timed_out());
  }

 private:
  FfmpegInterrupt* interrupt_;
};

void InitializeNetwork() {
  static const int result = avformat_network_init();
  CheckFfmpeg(result, "failed to initialize FFmpeg networking");
}

FormatOptions GetFormatOptions(const OutputConfig& config) {
  switch (config.protocol) {
    case OutputProtocol::kRtmp:
      return {"flv", "flvflags", "no_duration_filesize"};
    case OutputProtocol::kRtsp:
      return {"rtsp", "rtsp_transport",
              config.rtsp_transport == RtspTransport::kTcp ? "tcp" : "udp"};
    case OutputProtocol::kSrt:
      return {"mpegts", "mpegts_flags", "resend_headers"};
    case OutputProtocol::kFile:
      ThrowError(StatusCode::kInvalidArgument,
                 "NetworkMuxer does not support file output");
  }
  ThrowError(StatusCode::kInvalidArgument,
             "network output protocol is invalid");
}

void SetUserOptions(const std::map<std::string, std::string>& source,
                    ffmpeg::Dictionary* destination) {
  for (const auto& [key, value] : source) {
    destination->Set(key, value);
  }
}

void SetIoConstraints(const OutputConfig& config, AVCodecID video_codec_id,
                      ffmpeg::Dictionary* options) {
  if (config.protocol == OutputProtocol::kRtmp &&
      video_codec_id == AV_CODEC_ID_HEVC) {
    options->Set("rtmp_enhanced_codecs", "hvc1");
  } else if (config.protocol == OutputProtocol::kSrt) {
    options->Set("mode", "caller");
    options->Set("transtype", "live");
    options->Set("pkt_size", "1316");
  }
}

AVStream* AddStream(AVFormatContext* context,
                    const ffmpeg::CodecParameters& parameters,
                    AVRational time_base) {
  if (!IsValidTimeBase(time_base)) {
    ThrowError(StatusCode::kInvalidArgument,
               "network output stream time base is invalid");
  }
  AVStream* stream = avformat_new_stream(context, nullptr);
  if (stream == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to create a network output stream");
  }
  CheckFfmpeg(avcodec_parameters_copy(stream->codecpar, parameters.get()),
              "failed to copy network output stream parameters");
  stream->codecpar->codec_tag = 0;
  stream->time_base = time_base;
  return stream;
}

void CheckNetworkOperation(int result, FfmpegInterrupt* interrupt,
                           std::string_view operation) {
  if (interrupt != nullptr &&
      (interrupt->is_cancelled() || interrupt->timed_out())) {
    if (result < 0) {
      ThrowError(StatusCode::kUnavailable, operation, result);
    }
    ThrowError(StatusCode::kUnavailable, operation);
  }
  if (result >= 0) {
    return;
  }
  ThrowError(StatusCode::kFfmpegError, operation, result);
}

}  // namespace

NetworkMuxer::~NetworkMuxer() noexcept { Abort(); }

void NetworkMuxer::Open(const OutputConfig& config,
                        const ffmpeg::CodecParameters& video_parameters,
                        AVRational video_time_base,
                        const ffmpeg::CodecParameters* audio_parameters,
                        AVRational audio_time_base,
                        FfmpegInterrupt* interrupt) {
  if (open_attempted_) {
    ThrowError(StatusCode::kInvalidArgument,
               "NetworkMuxer can only be opened once");
  }
  open_attempted_ = true;
  if (config.url.empty()) {
    ThrowError(StatusCode::kInvalidArgument, "network output URL is empty");
  }

  const AVCodecParameters* raw_video_parameters = video_parameters.get();
  if (raw_video_parameters == nullptr ||
      (raw_video_parameters->codec_id != AV_CODEC_ID_H264 &&
       raw_video_parameters->codec_id != AV_CODEC_ID_HEVC)) {
    ThrowError(StatusCode::kUnsupported,
               "network output requires H.264 or H.265 video");
  }
  if (audio_parameters != nullptr &&
      (audio_parameters->get() == nullptr ||
       audio_parameters->get()->codec_id != AV_CODEC_ID_AAC)) {
    ThrowError(StatusCode::kUnsupported,
               "network output only supports AAC audio");
  }

  InitializeNetwork();
  const FormatOptions format_options = GetFormatOptions(config);
  InterruptDeadline deadline(interrupt, config.open_timeout);
  ffmpeg::OutputFormatContext candidate(format_options.format_name);
  AVFormatContext* context = candidate.get();
  if (interrupt != nullptr) {
    context->interrupt_callback = interrupt->callback();
  }
  context->url = av_strdup(config.url.c_str());
  if (context->url == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to copy the network output URL");
  }

  AVStream* video_stream =
      AddStream(context, video_parameters, video_time_base);
  AVStream* audio_stream = nullptr;
  if (audio_parameters != nullptr) {
    audio_stream = AddStream(context, *audio_parameters, audio_time_base);
  }

  ffmpeg::Dictionary options;
  SetUserOptions(config.options, &options);
  SetIoConstraints(config, raw_video_parameters->codec_id, &options);
  const bool owns_io = (context->oformat->flags & AVFMT_NOFILE) == 0;
  try {
    if (owns_io) {
      CheckNetworkOperation(
          avio_open2(
              &context->pb, context->url, AVIO_FLAG_WRITE,
              interrupt == nullptr ? nullptr : &context->interrupt_callback,
              options.inout()),
          interrupt, "failed to connect network output");
    }
    options.Set(format_options.header_option_key,
                format_options.header_option_value);
    CheckNetworkOperation(avformat_write_header(context, options.inout()),
                          interrupt, "failed to write network output header");
    for (const std::string& option : options.keys()) {
      spdlog::warn("network muxer did not consume option '{}'", option);
    }
  } catch (...) {
    if (owns_io && context->pb != nullptr) {
      avio_closep(&context->pb);
    }
    throw;
  }

  format_context_.emplace(std::move(candidate));
  video_stream_ = video_stream;
  audio_stream_ = audio_stream;
  interrupt_ = interrupt;
  write_timeout_ = config.write_timeout;
  owns_io_ = owns_io;
  header_written_ = true;
}

void NetworkMuxer::WriteVideo(ffmpeg::Packet packet) {
  Write(std::move(packet), video_stream_, &previous_video_dts_);
}

void NetworkMuxer::WriteAudio(ffmpeg::Packet packet) {
  if (audio_stream_ == nullptr) {
    ThrowError(StatusCode::kInvalidArgument,
               "network output has no audio stream");
  }
  Write(std::move(packet), audio_stream_, &previous_audio_dts_);
}

void NetworkMuxer::Finish() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "network muxer is not open");
  }

  InterruptDeadline deadline(interrupt_, write_timeout_);
  const int trailer_result = av_write_trailer(format_context_->get());
  const int close_result = CloseIo();
  if (trailer_result < 0 || close_result < 0 || deadline.was_interrupted()) {
    format_context_.reset();
  }
  finished_ = true;
  CheckNetworkOperation(trailer_result, interrupt_,
                        "failed to finish network output");
  CheckNetworkOperation(close_result, interrupt_,
                        "failed to close network output");
}

void NetworkMuxer::Abort() noexcept {
  if (aborted_ || finished_) {
    return;
  }
  FfmpegInterrupt* const interrupt = interrupt_;
  InterruptDeadline deadline(interrupt, write_timeout_);
  static_cast<void>(CloseIo());
  format_context_.reset();
  video_stream_ = nullptr;
  audio_stream_ = nullptr;
  interrupt_ = nullptr;
  aborted_ = true;
}

bool NetworkMuxer::is_open() const noexcept {
  return format_context_.has_value() && header_written_ && !finished_ &&
         !aborted_;
}

void NetworkMuxer::Write(ffmpeg::Packet packet, AVStream* stream,
                         std::optional<std::int64_t>* previous_dts) {
  if (!is_open() || stream == nullptr) {
    ThrowError(StatusCode::kInvalidArgument,
               "network muxer is not ready for packets");
  }
  AVPacket* raw_packet = packet.get();
  if (raw_packet == nullptr || raw_packet->size <= 0 ||
      !IsValidTimeBase(raw_packet->time_base) ||
      raw_packet->dts == AV_NOPTS_VALUE) {
    ThrowError(StatusCode::kInvalidArgument,
               "network output packet is missing data, DTS, or time base");
  }

  av_packet_rescale_ts(raw_packet, raw_packet->time_base, stream->time_base);
  if (previous_dts->has_value() && raw_packet->dts <= **previous_dts) {
    ThrowError(StatusCode::kInvalidArgument,
               "network output packet DTS is not strictly increasing");
  }
  *previous_dts = raw_packet->dts;
  raw_packet->stream_index = stream->index;
  raw_packet->pos = -1;
  InterruptDeadline deadline(interrupt_, write_timeout_);
  const int result =
      av_interleaved_write_frame(format_context_->get(), raw_packet);
  CheckNetworkOperation(result, interrupt_,
                        "failed to write a network output packet");
}

int NetworkMuxer::CloseIo() noexcept {
  if (!owns_io_ || !format_context_.has_value() ||
      format_context_->get()->pb == nullptr) {
    return 0;
  }
  return avio_closep(&format_context_->get()->pb);
}

}  // namespace mw::streamer::internal
