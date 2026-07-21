#ifndef MW_STREAMER_FFMPEG_PACKET_H_
#define MW_STREAMER_FFMPEG_PACKET_H_

extern "C" {
#include <libavcodec/packet.h>
}

namespace mw::streamer::ffmpeg {

class Packet final {
 public:
  Packet();
  ~Packet() noexcept;

  Packet(const Packet&) = delete;
  Packet& operator=(const Packet&) = delete;
  Packet(Packet&& other) noexcept;
  Packet& operator=(Packet&& other) noexcept;

  [[nodiscard]] AVPacket* get() noexcept;
  [[nodiscard]] const AVPacket* get() const noexcept;

  void Unref() noexcept;
  [[nodiscard]] Packet Ref() const;

 private:
  AVPacket* packet_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_PACKET_H_
