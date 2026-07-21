#include "mw/streamer/internal/fragmented_mp4_muxer.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
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

constexpr int kIoBufferSize = 32 * 1024;

std::FILE* OpenExclusive(const std::filesystem::path& path) {
  errno = 0;
#ifdef _WIN32
  std::FILE* file = nullptr;
  const errno_t result = _wfopen_s(&file, path.c_str(), L"wbx");
  if (result != 0) {
    errno = result;
  }
#else
  std::FILE* file = std::fopen(path.c_str(), "wbx");
#endif
  if (file == nullptr) {
    const std::error_code error(errno, std::generic_category());
    ThrowError(
        errno == EEXIST ? StatusCode::kAlreadyExists : StatusCode::kUnavailable,
        "failed to exclusively create fragmented MP4 output", error);
  }
  return file;
}

int WriteFile(void* opaque, const std::uint8_t* data, int size) {
  auto* file = static_cast<std::FILE*>(opaque);
  const std::size_t written =
      std::fwrite(data, 1, static_cast<std::size_t>(size), file);
  if (written == static_cast<std::size_t>(size)) {
    return size;
  }
  return AVERROR(errno == 0 ? EIO : errno);
}

std::int64_t TellFile(std::FILE* file) {
#ifdef _WIN32
  return _ftelli64(file);
#else
  return ftello(file);
#endif
}

int SeekFile(std::FILE* file, std::int64_t offset, int whence) {
#ifdef _WIN32
  return _fseeki64(file, offset, whence);
#else
  return fseeko(file, offset, whence);
#endif
}

std::int64_t Seek(void* opaque, std::int64_t offset, int whence) {
  auto* file = static_cast<std::FILE*>(opaque);
  if (whence == AVSEEK_SIZE) {
    const std::int64_t current = TellFile(file);
    if (current < 0 || SeekFile(file, 0, SEEK_END) != 0) {
      return AVERROR(errno == 0 ? EIO : errno);
    }
    const std::int64_t size = TellFile(file);
    if (size < 0 || SeekFile(file, current, SEEK_SET) != 0) {
      return AVERROR(errno == 0 ? EIO : errno);
    }
    return size;
  }

  whence &= ~AVSEEK_FORCE;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    return AVERROR(EINVAL);
  }
  if (SeekFile(file, offset, whence) != 0) {
    return AVERROR(errno == 0 ? EIO : errno);
  }
  const std::int64_t position = TellFile(file);
  return position < 0 ? AVERROR(errno == 0 ? EIO : errno) : position;
}

AVIOContext* CreateIoContext(std::FILE* file) {
  auto* buffer = static_cast<std::uint8_t*>(av_malloc(kIoBufferSize));
  if (buffer == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to allocate fragmented MP4 IO buffer");
  }
  AVIOContext* context = avio_alloc_context(buffer, kIoBufferSize, 1, file,
                                            nullptr, WriteFile, Seek);
  if (context == nullptr) {
    av_free(buffer);
    ThrowError(StatusCode::kFfmpegError,
               "failed to allocate fragmented MP4 IO context");
  }
  context->seekable = AVIO_SEEKABLE_NORMAL;
  return context;
}

AVStream* AddStream(AVFormatContext* context,
                    const ffmpeg::CodecParameters& parameters,
                    AVRational time_base) {
  if (!IsValidTimeBase(time_base)) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 stream time base is invalid");
  }
  AVStream* stream = avformat_new_stream(context, nullptr);
  if (stream == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to create a fragmented MP4 stream");
  }
  CheckFfmpeg(avcodec_parameters_copy(stream->codecpar, parameters.get()),
              "failed to copy fragmented MP4 stream parameters");
  stream->time_base = time_base;
  return stream;
}

}  // namespace

FragmentedMp4Muxer::~FragmentedMp4Muxer() noexcept { CloseIo(); }

void FragmentedMp4Muxer::Open(const std::filesystem::path& path,
                              const ffmpeg::CodecParameters& video_parameters,
                              AVRational video_time_base,
                              const ffmpeg::CodecParameters* audio_parameters,
                              AVRational audio_time_base) {
  if (format_context_.has_value()) {
    ThrowError(StatusCode::kInvalidArgument,
               "FragmentedMp4Muxer can only be opened once");
  }
  if (path.empty()) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 output path is empty");
  }
  const AVCodecParameters* raw_video_parameters = video_parameters.get();
  if (raw_video_parameters == nullptr ||
      (raw_video_parameters->codec_id != AV_CODEC_ID_H264 &&
       raw_video_parameters->codec_id != AV_CODEC_ID_HEVC)) {
    ThrowError(StatusCode::kUnsupported,
               "fragmented MP4 requires H.264 or H.265 video");
  }
  if (audio_parameters != nullptr &&
      audio_parameters->get()->codec_id != AV_CODEC_ID_AAC) {
    ThrowError(StatusCode::kUnsupported,
               "fragmented MP4 only supports AAC audio");
  }

  ffmpeg::OutputFormatContext candidate("mp4");
  AVFormatContext* context = candidate.get();
  AVStream* video_stream =
      AddStream(context, video_parameters, video_time_base);
  AVStream* audio_stream = nullptr;
  if (audio_parameters != nullptr) {
    audio_stream = AddStream(context, *audio_parameters, audio_time_base);
  }

  std::FILE* file = OpenExclusive(path);
  AVIOContext* io_context = nullptr;
  try {
    io_context = CreateIoContext(file);
    context->pb = io_context;
    context->flags |= AVFMT_FLAG_CUSTOM_IO;

    ffmpeg::Dictionary options;
    options.Set("movflags", "frag_keyframe+empty_moov+default_base_moof");
    CheckFfmpeg(avformat_write_header(context, options.inout()),
                "failed to write the fragmented MP4 header");
    for (const std::string& option : options.keys()) {
      spdlog::warn("fragmented MP4 muxer did not consume option '{}'", option);
    }
  } catch (...) {
    context->pb = nullptr;
    if (io_context != nullptr) {
      avio_context_free(&io_context);
    }
    std::fclose(file);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    throw;
  }

  format_context_.emplace(std::move(candidate));
  file_ = file;
  io_context_ = io_context;
  video_stream_ = video_stream;
  audio_stream_ = audio_stream;
  header_written_ = true;
}

void FragmentedMp4Muxer::WriteVideo(ffmpeg::Packet packet) {
  Write(std::move(packet), video_stream_, &previous_video_dts_);
}

void FragmentedMp4Muxer::WriteAudio(ffmpeg::Packet packet) {
  if (audio_stream_ == nullptr) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 output has no audio stream");
  }
  Write(std::move(packet), audio_stream_, &previous_audio_dts_);
}

void FragmentedMp4Muxer::Finish() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 muxer is not open");
  }
  CheckFfmpeg(av_write_trailer(format_context_->get()),
              "failed to finish fragmented MP4 output");

  avio_flush(io_context_);
  const int io_error = io_context_->error;
  std::error_code file_error;
  if (std::fflush(file_) != 0) {
    file_error =
        std::error_code(errno == 0 ? EIO : errno, std::generic_category());
  }

  format_context_->get()->pb = nullptr;
  avio_context_free(&io_context_);
  if (std::fclose(file_) != 0 && !file_error) {
    file_error =
        std::error_code(errno == 0 ? EIO : errno, std::generic_category());
  }
  file_ = nullptr;
  finished_ = true;
  if (io_error < 0) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to flush fragmented MP4 output", io_error);
  }
  if (file_error) {
    ThrowError(StatusCode::kUnavailable,
               "failed to flush fragmented MP4 output file", file_error);
  }
}

bool FragmentedMp4Muxer::is_open() const noexcept {
  return format_context_.has_value() && header_written_ && !finished_;
}

void FragmentedMp4Muxer::Write(ffmpeg::Packet packet, AVStream* stream,
                               std::optional<std::int64_t>* previous_dts) {
  if (!is_open() || stream == nullptr) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 muxer is not ready for packets");
  }
  AVPacket* raw_packet = packet.get();
  if (raw_packet == nullptr || raw_packet->size <= 0 ||
      !IsValidTimeBase(raw_packet->time_base) ||
      raw_packet->dts == AV_NOPTS_VALUE) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 packet is missing data, DTS, or time base");
  }

  av_packet_rescale_ts(raw_packet, raw_packet->time_base, stream->time_base);
  if (previous_dts->has_value() && raw_packet->dts <= **previous_dts) {
    ThrowError(StatusCode::kInvalidArgument,
               "fragmented MP4 packet DTS is not strictly increasing");
  }
  *previous_dts = raw_packet->dts;
  raw_packet->stream_index = stream->index;
  raw_packet->pos = -1;
  CheckFfmpeg(av_interleaved_write_frame(format_context_->get(), raw_packet),
              "failed to write a fragmented MP4 packet");
}

void FragmentedMp4Muxer::CloseIo() noexcept {
  if (format_context_.has_value()) {
    format_context_->get()->pb = nullptr;
  }
  if (io_context_ != nullptr) {
    avio_flush(io_context_);
    avio_context_free(&io_context_);
  }
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace mw::streamer::internal
