#ifndef MW_STREAMER_FFMPEG_CHANNEL_LAYOUT_H_
#define MW_STREAMER_FFMPEG_CHANNEL_LAYOUT_H_

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace mw::streamer::ffmpeg {

class ChannelLayout final {
 public:
  explicit ChannelLayout(int channel_count);
  ~ChannelLayout() noexcept;

  ChannelLayout(const ChannelLayout&) = delete;
  ChannelLayout& operator=(const ChannelLayout&) = delete;
  ChannelLayout(ChannelLayout&& other) noexcept;
  ChannelLayout& operator=(ChannelLayout&& other) noexcept;

  [[nodiscard]] static ChannelLayout CopyOf(const AVChannelLayout& layout);

  [[nodiscard]] AVChannelLayout* get() noexcept;
  [[nodiscard]] const AVChannelLayout* get() const noexcept;
  [[nodiscard]] ChannelLayout Clone() const;

 private:
  explicit ChannelLayout(AVChannelLayout layout) noexcept;

  AVChannelLayout layout_{};
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_CHANNEL_LAYOUT_H_
