#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_PACKET_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_PACKET_H_

extern "C" {
#include <libavcodec/packet.h>
}

namespace mw::streamer::ffmpeg {

class Packet final {
 public:
  Packet();
  // Adopts a packet allocated by av_packet_alloc() or av_packet_clone().
  explicit Packet(AVPacket* packet);
  ~Packet();

  Packet(const Packet& other);
  Packet& operator=(const Packet& other);
  Packet(Packet&& other) noexcept;
  Packet& operator=(Packet&& other) noexcept;

  static Packet Clone(const AVPacket& source);
  Packet Clone() const;
  Packet Ref() const;

  const AVPacket* get() const noexcept;
  AVPacket* get() noexcept;
  const AVPacket* operator->() const noexcept;
  AVPacket* operator->() noexcept;
  void Unref() noexcept;

 private:
  AVPacket* packet_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_PACKET_H_
