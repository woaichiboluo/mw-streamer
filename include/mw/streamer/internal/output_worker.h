#ifndef MW_STREAMER_INTERNAL_OUTPUT_WORKER_H_
#define MW_STREAMER_INTERNAL_OUTPUT_WORKER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

extern "C" {
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/common/blocking_queue.h"
#include "mw/streamer/internal/common/ffmpeg_interrupt.h"
#include "mw/streamer/internal/common/reconnect_backoff.h"
#include "mw/streamer/pipeline_config.h"
#include "mw/streamer/status.h"

namespace mw::streamer::internal {

class FragmentedMp4Muxer;
class NetworkMuxer;

enum class EncodedMediaType {
  kVideo,
  kAudio,
};

struct EncodedPacket {
  EncodedPacket(EncodedMediaType media_type, ffmpeg::Packet packet);

  EncodedPacket(const EncodedPacket&) = delete;
  EncodedPacket& operator=(const EncodedPacket&) = delete;
  EncodedPacket(EncodedPacket&&) noexcept = default;
  EncodedPacket& operator=(EncodedPacket&&) noexcept = default;

  [[nodiscard]] EncodedPacket Ref() const;

  EncodedMediaType media_type;
  ffmpeg::Packet packet;
};

class OutputPacketGate final {
 public:
  void Reset() noexcept;
  [[nodiscard]] bool ShouldWrite(const EncodedPacket& packet);

 private:
  bool waiting_for_keyframe_ = true;
  std::int64_t cutover_dts_us_ = 0;
};

using OutputEventReporter =
    std::function<void(EndpointEventType type, const Status& status)>;

class OutputWorker final {
 public:
  OutputWorker(OutputConfig config,
               const ffmpeg::CodecParameters& video_parameters,
               AVRational video_time_base,
               const ffmpeg::CodecParameters* audio_parameters,
               AVRational audio_time_base, OutputEventReporter reporter);
  ~OutputWorker() noexcept;

  OutputWorker(const OutputWorker&) = delete;
  OutputWorker& operator=(const OutputWorker&) = delete;
  OutputWorker(OutputWorker&&) = delete;
  OutputWorker& operator=(OutputWorker&&) = delete;

  void Start();
  [[nodiscard]] bool Enqueue(const EncodedPacket& packet);
  void Close() noexcept;
  void RequestStop() noexcept;
  void Wait() noexcept;
  void Stop() noexcept;

  [[nodiscard]] bool is_accepting() const noexcept;

 private:
  void Run() noexcept;
  void RunFile();
  void RunNetwork();
  void WriteFile(EncodedPacket packet);
  void WriteNetwork(EncodedPacket packet);
  [[nodiscard]] Status OpenNetworkSession();
  void AbortNetworkSession() noexcept;
  void MarkActiveNetworkSessionOverflow() noexcept;
  void CancelActiveNetworkOperation() noexcept;
  void Report(EndpointEventType type, const Status& status) noexcept;
  void Fail(const Status& status) noexcept;
  [[nodiscard]] Status CurrentExceptionStatus(const char* stage) const;
  [[nodiscard]] double NextJitterSample();

  OutputConfig config_;
  ffmpeg::CodecParameters video_parameters_;
  AVRational video_time_base_;
  std::unique_ptr<ffmpeg::CodecParameters> audio_parameters_;
  AVRational audio_time_base_;
  OutputEventReporter reporter_;
  BlockingQueue<EncodedPacket> queue_;
  ReconnectBackoff reconnect_backoff_;
  OutputPacketGate packet_gate_;
  std::mt19937 random_engine_;
  std::uniform_real_distribution<double> jitter_distribution_{-1.0, 1.0};

  std::unique_ptr<FragmentedMp4Muxer> file_muxer_;
  std::unique_ptr<FfmpegInterrupt> network_interrupt_;
  std::unique_ptr<NetworkMuxer> network_muxer_;
  Status network_error_;

  mutable std::mutex interrupt_mutex_;
  FfmpegInterrupt* active_interrupt_ = nullptr;
  std::mutex wait_mutex_;
  std::condition_variable wait_condition_;
  std::thread thread_;

  std::atomic<bool> accepting_{false};
  std::atomic<bool> close_requested_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> overflow_pending_{false};
  bool started_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_OUTPUT_WORKER_H_
