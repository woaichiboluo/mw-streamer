#ifndef MW_STREAMER_ENCODER_ENCODER_H_
#define MW_STREAMER_ENCODER_ENCODER_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/encoder.h"
#include "mw/streamer/media_frame.h"
#include "mw/streamer/performance/performance.h"

namespace mw::streamer {

enum class RateControl { kCbr, kVbr };

struct MW_STREAMER_API EncoderConfig {
  AVRational fps{30, 1};
  // Both zero preserve the input dimensions. No implicit resizing.
  int width = 0;
  int height = 0;
  // NONE inherits the incoming stream's format. Otherwise this is the required
  // pixel layout after processing (for example YUV420P or NV12), including the
  // software layout of CUDA surfaces. No implicit pixel format conversion.
  AVPixelFormat video_pixel_format = AV_PIX_FMT_NONE;
  std::string video_encoder_name = "libx264";
  std::string audio_encoder_name = "aac";
  std::int64_t video_bit_rate = 5000000;
  std::int64_t audio_bit_rate = 128000;
  int gop_size = 60;
  int max_b_frames = 0;
  RateControl rate_control = RateControl::kVbr;
  std::int64_t max_bit_rate = 0;
  std::map<std::string, std::string> video_options;
  std::map<std::string, std::string> audio_options;
};

// A real-time consumer of complete channel frames. Two fixed workers finish
// encoding each frame's tracks before taking the next frame. Callbacks must
// consume their own exceptions, and must not call Start, Drain or Stop.
// Serialize Start/Stop externally; stop upstream delivery before Stop.
class MW_STREAMER_API Encoder final {
 public:
  using OnReady = std::function<void(const std::vector<ffmpeg::StreamInfo>&)>;
  // Packets retain encoder PTS/DTS and time bases, including codec delay.
  // Copy with Packet::Ref() to retain.
  // Audio and video callbacks may execute concurrently.
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  using OnEnded = std::function<void()>;
  using OnError = std::function<void(int, std::string_view)>;

  Encoder() = default;
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  Encoder(Encoder&&) = delete;
  Encoder& operator=(Encoder&&) = delete;

  void SetOnReady(OnReady callback) noexcept;
  void SetOnPacket(OnPacket callback) noexcept;
  void SetOnEnded(OnEnded callback) noexcept;
  void SetOnError(OnError callback) noexcept;

  // streams describe the actual incoming frame layout after processing.
  // Opens codecs synchronously, invokes OnReady, then launches workers.
  // Failure throws; another Start requires Stop. No device is needed for CPU.
  void Start(const EncoderConfig& config,
             const std::vector<ffmpeg::StreamInfo>& streams,
             const ffmpeg::HwDeviceContext& video_device);
  // Retains one complete channel frame. The real-time queue holds one frame,
  // excluding the frame being encoded. A full queue drops the incoming frame
  // but advances both track counters and returns true.
  // False only rejects a closed/failed session. Input PTS is not a second
  // clock: video frames/audio samples are counted at admission, before any
  // drop.
  bool SubmitFrame(const MediaFrame& frame);
  // Rejects new frames and drains queued samples/pictures and codec delay.
  // OnEnded fires once when both workers finish, including an empty session.
  void Drain() noexcept;
  // Rejects new frames, drains accepted work and codec delay, then joins
  // workers and releases codecs. Fatal failures cancel pending work instead.
  // Normal Stop/destruction may call OnPacket and OnEnded; captured resources
  // must remain alive until completion. No callbacks run after returning.
  // Callbacks are retained for a subsequent Start.
  void Stop() noexcept;

 private:
  friend class LivePipeline;
  // Internal pipeline/startup failure cleanup, without a public cancel API.
  void RequestCancel() noexcept;
  void RunVideo() noexcept;
  void RunAudio() noexcept;
  void EncodeFrame(ffmpeg::Encoder& encoder, const ffmpeg::Frame& frame,
                   bool video);
  ffmpeg::EncodeResult ReceivePackets(ffmpeg::Encoder& encoder, bool video);
  void DrainCodec(ffmpeg::Encoder& encoder, bool video);
  void FinishTrack(bool video) noexcept;
  void Fail(int error, std::string_view message, bool video) noexcept;
  bool Cancelled() noexcept;
  void Clear() noexcept;

  EncoderConfig config_;
  OnReady on_ready_;
  OnPacket on_packet_;
  OnEnded on_ended_;
  OnError on_error_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool started_ = false;
  bool stopping_ = false;
  bool draining_ = false;
  bool failed_ = false;
  bool video_done_ = true;
  bool audio_done_ = true;
  bool ended_notified_ = false;
  int callback_calls_ = 0;
  std::thread video_thread_;
  std::thread audio_thread_;
  std::unique_ptr<ffmpeg::VideoEncoder> video_encoder_;
  std::unique_ptr<ffmpeg::AudioEncoder> audio_encoder_;
  std::deque<MediaFrame> frames_;
  std::optional<MediaFrame> active_frame_;
  bool audio_pending_ = false;
  bool audio_completed_ = true;
  bool audio_finish_requested_ = false;
  std::int64_t video_pts_ = 0;
  std::int64_t audio_pts_ = 0;
  int video_stream_index_ = -1;
  int audio_stream_index_ = -1;
  AVSampleFormat audio_format_ = AV_SAMPLE_FMT_NONE;
  AVSampleFormat audio_input_format_ = AV_SAMPLE_FMT_NONE;
  int audio_rate_ = 0;
  AVChannelLayout audio_layout_{};
  int audio_capabilities_ = 0;
  internal::EncoderPerformance performance_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_ENCODER_H_
