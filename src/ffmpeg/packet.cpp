#include "mw/streamer/ffmpeg/packet.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

Packet::Packet() : packet_(av_packet_alloc()) {
  if (packet_ == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate an FFmpeg packet");
  }
}

Packet::~Packet() noexcept { av_packet_free(&packet_); }

Packet::Packet(Packet&& other) noexcept
    : packet_(std::exchange(other.packet_, nullptr)) {}

Packet& Packet::operator=(Packet&& other) noexcept {
  if (this != &other) {
    av_packet_free(&packet_);
    packet_ = std::exchange(other.packet_, nullptr);
  }
  return *this;
}

AVPacket* Packet::get() noexcept { return packet_; }

const AVPacket* Packet::get() const noexcept { return packet_; }

void Packet::Unref() noexcept {
  if (packet_ != nullptr) {
    av_packet_unref(packet_);
  }
}

Packet Packet::Ref() const {
  if (packet_ == nullptr) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot reference an empty FFmpeg packet");
  }

  Packet result;
  internal::CheckFfmpeg(av_packet_ref(result.get(), packet_),
                        "failed to reference an FFmpeg packet");
  return result;
}

}  // namespace mw::streamer::ffmpeg
