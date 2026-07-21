#include "mw/streamer/ffmpeg/channel_layout.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

ChannelLayout::ChannelLayout(int channel_count) {
  if (channel_count <= 0) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "channel count must be positive");
  }

  av_channel_layout_default(&layout_, channel_count);
  if (av_channel_layout_check(&layout_) == 0) {
    av_channel_layout_uninit(&layout_);
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "failed to create a default FFmpeg channel layout");
  }
}

ChannelLayout::ChannelLayout(AVChannelLayout layout) noexcept
    : layout_(layout) {}

ChannelLayout::~ChannelLayout() noexcept { av_channel_layout_uninit(&layout_); }

ChannelLayout::ChannelLayout(ChannelLayout&& other) noexcept
    : layout_(std::exchange(other.layout_, AVChannelLayout{})) {}

ChannelLayout& ChannelLayout::operator=(ChannelLayout&& other) noexcept {
  if (this != &other) {
    av_channel_layout_uninit(&layout_);
    layout_ = std::exchange(other.layout_, AVChannelLayout{});
  }
  return *this;
}

ChannelLayout ChannelLayout::CopyOf(const AVChannelLayout& layout) {
  if (av_channel_layout_check(&layout) == 0) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot copy an invalid FFmpeg channel layout");
  }

  AVChannelLayout copy{};
  internal::CheckFfmpeg(av_channel_layout_copy(&copy, &layout),
                        "failed to copy an FFmpeg channel layout");
  return ChannelLayout(copy);
}

AVChannelLayout* ChannelLayout::get() noexcept { return &layout_; }

const AVChannelLayout* ChannelLayout::get() const noexcept { return &layout_; }

ChannelLayout ChannelLayout::Clone() const { return CopyOf(layout_); }

}  // namespace mw::streamer::ffmpeg
