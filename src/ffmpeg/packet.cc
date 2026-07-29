#include "mw/ffmpeg/packet.h"

#include <new>
#include <stdexcept>
#include <utility>

#include "mw/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {

Packet::Packet() : packet_(av_packet_alloc()) {
  if (!packet_) {
    throw std::bad_alloc();
  }
}

Packet::Packet(AVPacket* packet) : packet_(packet) {
  if (!packet_) {
    throw std::invalid_argument("不能接管空AVPacket");
  }
}

Packet::~Packet() { av_packet_free(&packet_); }

Packet::Packet(const Packet& other) : Packet() {
  if (!other.packet_) {
    throw std::logic_error("不能引用已移动的Packet");
  }
  ThrowIfError(av_packet_ref(packet_, other.packet_), "引用AVPacket");
}

Packet& Packet::operator=(const Packet& other) {
  if (this != &other) {
    Packet copy(other);
    std::swap(packet_, copy.packet_);
  }
  return *this;
}

Packet::Packet(Packet&& other) noexcept
    : packet_(std::exchange(other.packet_, nullptr)) {}

Packet& Packet::operator=(Packet&& other) noexcept {
  if (this != &other) {
    av_packet_free(&packet_);
    packet_ = std::exchange(other.packet_, nullptr);
  }
  return *this;
}

Packet Packet::Clone(const AVPacket& source) {
  auto* packet = av_packet_clone(&source);
  if (!packet) {
    throw std::bad_alloc();
  }
  return Packet(packet);
}

Packet Packet::Clone() const {
  if (!packet_) {
    throw std::logic_error("不能Clone已移动的Packet");
  }
  return Clone(*packet_);
}

Packet Packet::Ref() const { return Packet(*this); }

const AVPacket* Packet::get() const noexcept { return packet_; }

AVPacket* Packet::get() noexcept { return packet_; }

const AVPacket* Packet::operator->() const noexcept { return packet_; }

AVPacket* Packet::operator->() noexcept { return packet_; }

void Packet::Unref() noexcept {
  if (packet_) {
    av_packet_unref(packet_);
  }
}

}  // namespace mw::streamer::ffmpeg
