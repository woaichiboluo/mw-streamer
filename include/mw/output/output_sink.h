#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SINK_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SINK_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

#include "mw/ffmpeg/frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::output {

namespace internal {
class OutputSinkAccess;
}

// Consumer of one independently scheduled raw output flow. The Pipeline calls
// Start and Stop once, and serializes audio and video delivery for each Sink on
// a dedicated thread. Each Write transfers one independently referenced frame
// that the Sink may retain after the call returns. Sinks must treat the frame
// and its buffers as read-only because tee branches may share buffer storage.
class OutputSink {
 public:
  virtual ~OutputSink();

  virtual void Start() = 0;
  virtual void WriteAudio(ffmpeg::Frame frame) = 0;
  virtual void WriteVideo(ffmpeg::Frame frame) = 0;
  // Stop must synchronously release Sink resources, end event production, and
  // must not throw.
  virtual void Stop() noexcept = 0;

  OutputSink(const OutputSink&) = delete;
  OutputSink& operator=(const OutputSink&) = delete;
  OutputSink(OutputSink&&) = delete;
  OutputSink& operator=(OutputSink&&) = delete;

 protected:
  OutputSink() = default;

  // Submits without waiting for Processor handling. type and payload are
  // borrowed only until this call returns. Returns false when the event queue
  // is full or the event channel is not currently bound and running.
  bool NotifyEvent(
      std::string_view type, const void* payload = nullptr,
      std::size_t payload_size = 0,
      std::optional<MwStreamerMediaTimestamp> timestamp = std::nullopt) const;

 private:
  using EventSubmitter = std::function<bool(
      std::string_view type, const void* payload, std::size_t payload_size,
      std::optional<MwStreamerMediaTimestamp> timestamp)>;

  void BindEventSubmitter(EventSubmitter submitter);

  friend class internal::OutputSinkAccess;

  EventSubmitter event_submitter_;
};

namespace internal {

// Keeps framework-only event binding out of the public OutputSink surface.
class OutputSinkAccess final {
 public:
  static void BindEventSubmitter(OutputSink& sink,
                                 OutputSink::EventSubmitter submitter);
};

}  // namespace internal

}  // namespace mw::streamer::output

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SINK_H_
