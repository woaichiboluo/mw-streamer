#ifndef MW_STREAMER_ENCODER_AV_ENCODER_CONFIG_H_
#define MW_STREAMER_ENCODER_AV_ENCODER_CONFIG_H_

#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace mw::streamer {

// Shared target format for conversion, composition and subsequent encoding.
// Color properties are inherited from input frames; hardware devices are
// supplied separately. Encoder-specific options belong to the future encoder.
struct AvEncoderConfig {
  std::string video_encoder_name = "libx264";
  std::string audio_encoder_name = "aac";
  // Both zero preserve source size. Conversion here does not resize video.
  int width = 0;
  int height = 0;
  AVRational fps{30, 1};
  // The final image's pixel layout, including CUDA surfaces' software format.
  // NONE preserves the source layout. Frame-rate conversion does not change it.
  AVPixelFormat video_pixel_format = AV_PIX_FMT_NONE;
  int audio_sample_rate = 48000;
  // Native FFmpeg AAC accepts planar float samples, not packed S32.
  AVSampleFormat audio_sample_format = AV_SAMPLE_FMT_FLTP;
  // Borrowed when passed to a module's constructor; that module must deep-copy
  // the layout. Native layouts such as stereo need no external allocation.
  AVChannelLayout audio_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_AV_ENCODER_CONFIG_H_
