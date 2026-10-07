#ifndef MW_STREAMER_FFMPEG_DECODER_H_
#define MW_STREAMER_FFMPEG_DECODER_H_

#include <chrono>
#include <cstdint>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/performance/performance.h"

namespace mw::streamer::ffmpeg {

enum class DecodeResult { kFrame, kNeedInput, kEnd };

// Synchronous decoder for one track. Serialize all calls on the owning worker.
// Input owns packet queues, timestamp synthesis and playback scheduling.
class MW_STREAMER_API Decoder {
 public:
  virtual ~Decoder();

  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  Decoder(Decoder&&) = delete;
  Decoder& operator=(Decoder&&) = delete;

  // true: accepted, the caller can release the packet. false: receive output,
  // then retry the same packet. Does not keep an application packet queue.
  bool SendPacket(const Packet& packet);
  // Replaces frame, including unreferencing it when no output is available.
  // Output timestamps use the input stream time_base.
  DecodeResult ReceiveFrame(Frame& frame);
  // Signals EOF. On false, receive output and retry. Then receive until kEnd.
  bool Drain();
  // Discards buffered data and permits new input after EOF or a seek. Frames
  // retained by the caller remain valid. Keeps the shared device reference.
  void Flush() noexcept;

 protected:
  Decoder(const StreamInfo& stream, AVMediaType type,
          std::string_view decoder_name);
  void Open();
  AVCodecContext* context() noexcept;

 private:
  using PerformanceClock = std::chrono::steady_clock;
  PerformanceClock::time_point BeginPerformanceWork() const noexcept;
  void FinishPerformanceWork(PerformanceClock::time_point started,
                             bool error = false, bool final = false) noexcept;
  void CountPerformanceFrame(const AVFrame& frame) noexcept;
  void ReportPerformance(bool final) noexcept;

  CodecContext context_;
  AVRational time_base_;
  int stream_index_;
  const char* performance_module_;
  internal::PerformanceWindow performance_;
  bool performance_enabled_ = false;
  bool performance_trace_enabled_ = false;
  bool performance_final_ = false;
  std::int64_t performance_previous_pts_ = AV_NOPTS_VALUE;
};

class MW_STREAMER_API VideoDecoder final : public Decoder {
 public:
  // Software decoding; no device is required. Empty name selects by codec_id.
  explicit VideoDecoder(const StreamInfo& stream,
                        std::string_view decoder_name = {});
  // Shares the external CPU/CUDA device. CUDA frames stay on the GPU. Device
  // creation and automatic fallback to another decoder are not performed.
  VideoDecoder(const StreamInfo& stream, const HwDeviceContext& device,
               std::string_view decoder_name = {});

 private:
  static AVPixelFormat GetHardwareFormat(AVCodecContext* context,
                                         const AVPixelFormat* formats) noexcept;
};

class MW_STREAMER_API AudioDecoder final : public Decoder {
 public:
  // Audio decoding produces CPU sample buffers and needs no hardware device.
  explicit AudioDecoder(const StreamInfo& stream,
                        std::string_view decoder_name = {});
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_DECODER_H_
