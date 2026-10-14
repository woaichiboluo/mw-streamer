#ifndef MW_STREAMER_ENCODER_AV_ENCODER_CONFIG_H_
#define MW_STREAMER_ENCODER_AV_ENCODER_CONFIG_H_

#include <cstdint>
#include <map>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace mw::streamer {

enum class RateControl { kCbr, kVbr };

// Shared target format for conversion, composition and subsequent encoding.
// Color properties are inherited from input frames; hardware devices are
// supplied separately. Encoder options are consumed only by AvEncoder.
struct AvEncoderConfig {
  std::string video_encoder_name = "libx264";
  std::string audio_encoder_name = "aac";
  // Both zero preserve source size. Conversion here does not resize video.
  int width = 0;
  int height = 0;
  AVRational fps{30, 1};
  // The final image's pixel layout, including CUDA surfaces' software format.
  // NONE disables channel video output. Input FPS conversion preserves its
  // source layout; the compositor produces this target layout.
  AVPixelFormat video_pixel_format = AV_PIX_FMT_NV12;
  int audio_sample_rate = 48000;
  // Native FFmpeg AAC accepts planar float samples, not packed S32.
  // NONE disables channel audio output.
  AVSampleFormat audio_sample_format = AV_SAMPLE_FMT_FLTP;
  // Borrowed when passed to a module's constructor; that module must deep-copy
  // the layout. Native layouts such as stereo need no external allocation.
  AVChannelLayout audio_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
  std::int64_t video_bit_rate = 5000000;
  std::int64_t audio_bit_rate = 128000;
  int gop_size = 60;
  int max_b_frames = 0;
  RateControl rate_control = RateControl::kVbr;
  // Zero leaves the VBR peak unconstrained. CBR uses the target video bitrate.
  std::int64_t max_bit_rate = 0;
  // Codec-specific options must not override the explicit format/rate fields.
  std::map<std::string, std::string> video_options;
  std::map<std::string, std::string> audio_options;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_AV_ENCODER_CONFIG_H_
