#ifndef MW_STREAMER_FFMPEG_ENCODER_H_
#define MW_STREAMER_FFMPEG_ENCODER_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::ffmpeg {

enum class EncodeResult { kPacket, kNeedInput, kEnd };

struct MW_STREAMER_API VideoEncoderConfig {
  std::string encoder_name = "libx264";
  int width = 0;
  int height = 0;
  // Actual pixel layout. CPU/CUDA storage is selected by the device.
  AVPixelFormat pixel_format = AV_PIX_FMT_YUV420P;
  AVRational frame_rate{0, 1};
  AVRational time_base{1, 90000};
  std::int64_t bit_rate = 0;
  int gop_size = 60;
  int max_b_frames = 0;
  bool global_header = true;
  AVRational sample_aspect_ratio{1, 1};
  AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
  AVColorSpace color_space = AVCOL_SPC_UNSPECIFIED;
  AVColorPrimaries color_primaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic color_trc = AVCOL_TRC_UNSPECIFIED;
};

struct MW_STREAMER_API AudioEncoderConfig {
  std::string encoder_name = "aac";
  AVSampleFormat sample_format = AV_SAMPLE_FMT_FLTP;
  int sample_rate = 48000;
  // Borrowed during construction; the encoder owns a deep copy of the layout.
  AVChannelLayout channel_layout = AV_CHANNEL_LAYOUT_STEREO;
  std::int64_t bit_rate = 128000;
  bool global_header = true;
};

// Synchronous encoder for one track. Serialize calls on the owning worker.
// The caller aligns the session timeline before submitting frames.
class MW_STREAMER_API Encoder {
 public:
  virtual ~Encoder();

  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  Encoder(Encoder&&) = delete;
  Encoder& operator=(Encoder&&) = delete;

  // Rescales PTS and duration without modifying the original frame or buffers.
  // false: receive output, then retry the same frame. No application queue.
  bool SendFrame(const Frame& frame);
  // Replaces packet. PTS/DTS/duration and time_base describe encoder output;
  // stream_index is assigned by the caller when routing to a muxer.
  EncodeResult ReceivePacket(Packet& packet);
  // Signals EOF. On false, receive output and retry, then receive until kEnd.
  // Recreate the encoder to start another session after draining.
  bool Drain();
  StreamInfo stream_info(int stream_index) const;

 protected:
  Encoder(std::string_view encoder_name, AVMediaType type);
  // Options are copied; the caller retains ownership of the dictionary.
  void Open(AVDictionary* options);
  AVCodecContext* context() noexcept;
  const AVCodecContext* context() const noexcept;

 private:
  void ValidateFrame(const AVFrame& frame) const;

  CodecContext context_;
};

class MW_STREAMER_API VideoEncoder final : public Encoder {
 public:
  explicit VideoEncoder(const VideoEncoderConfig& config,
                        AVDictionary* options = nullptr);
  // CUDA uses pixel_format as its surface layout (for example NV12).
  // GPU frames stay on that device; no conversion or fallback is performed.
  VideoEncoder(const VideoEncoderConfig& config, const HwDeviceContext& device,
               AVDictionary* options = nullptr);
  // Retains compatible frames or mechanically transfers CPU/CUDA storage.
  // Pixel layout, dimensions and source properties remain unchanged.
  Frame PrepareFrame(const Frame& frame) const;

 private:
  void Configure(const VideoEncoderConfig& config);
  void ConfigureDevice(const VideoEncoderConfig& config,
                       const HwDeviceContext& device);
};

class MW_STREAMER_API AudioEncoder final : public Encoder {
 public:
  explicit AudioEncoder(const AudioEncoderConfig& config,
                        AVDictionary* options = nullptr);
  // 0 denotes a variable-size encoder. Otherwise submit frame_size samples;
  // a supported short final frame must be followed by Drain().
  int frame_size() const noexcept;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_ENCODER_H_
