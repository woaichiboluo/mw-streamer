#ifndef MW_STREAMER_OUTPUT_ENCODE_PUSH_SINK_H_
#define MW_STREAMER_OUTPUT_ENCODE_PUSH_SINK_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/output/frame_sink.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace mw::streamer {

// Owns an Encoder and Remuxer without another queue or worker. Retains the
// caller's CPU/CUDA device reference. Requires Init before initialization;
// destroy the sink before Shutdown. Serialize Initialize, output additions
// and destruction externally. Serialize Send calls with each other; after
// initialization they may overlap output additions. Output owns shared
// references to this sink.
class MW_STREAMER_API EncodePushSink final : public FrameSink {
 public:
  // Once on the first terminal Encoder/Remuxer failure, outside locks on that
  // component's worker. Later Send calls return false. Network retries do not
  // invoke this callback. Querying output_info is allowed; Initialize, Send,
  // output additions and destruction from the callback are forbidden.
  // Observer exceptions are logged without replacing the original failure.
  using OnError = std::function<void(std::exception_ptr)>;

  // Constructs both components, without opening codecs or starting outputs.
  explicit EncodePushSink(const ffmpeg::HwDeviceContext& device);
  // Drains the Encoder first, then finalizes and closes the Remuxer. Callback
  // resources must remain alive until destruction finishes.
  ~EncodePushSink() override;
  EncodePushSink(const EncodePushSink&) = delete;
  EncodePushSink& operator=(const EncodePushSink&) = delete;
  EncodePushSink(EncodePushSink&&) = delete;
  EncodePushSink& operator=(EncodePushSink&&) = delete;

  // Thread-safe replacement. Register before Initialize; past errors are not
  // replayed when changing the callback.
  void SetOnError(OnError callback);
  // Starts the Encoder, then the Remuxer with its encoded tracks. Both must
  // succeed. Failure logs, rolls back started work and returns false without
  // OnError; configured outputs remain available for retry. A successful sink
  // is initialized once; repeated calls return false without changing it.
  [[nodiscard]] bool Initialize(
      const AvEncoderConfig& format) noexcept override;
  // The promise completes after Encoder admission, including intentional drops.
  // It does not wait for encoding, muxing or network delivery. Before
  // successful initialization/after terminal failure returns a ready false
  // future. Input validation exceptions are stored in the future; no deferred
  // work is added.
  std::future<bool> Send(OutputFrame frame) override;

  // Same registration, dynamic addition and error contracts as Remuxer. One
  // RTSP and one HLS publication per sink; push/recording targets may be
  // multiple.
  std::string AddPushUrl(const std::string& url);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls);
  Remuxer::OutputInfo output_info() const;
  // Same synchronous snapshot and raw SDK status contract as Remuxer.
  std::vector<Remuxer::PushStatus> push_statuses() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_OUTPUT_ENCODE_PUSH_SINK_H_
