#ifndef MW_STREAMER_INPUT_FILE_INPUT_H_
#define MW_STREAMER_INPUT_FILE_INPUT_H_

#include <memory>
#include <string>

#include "mw/streamer/input/config.h"
#include "mw/streamer/input/input.h"

namespace mw::streamer {

// Reads one local file as fast as the synchronous observer accepts packets.
// Selects at most one audio and one video track, excluding cover artwork.
// Original signed timestamps, stream time bases and packet side data survive
// delivery. Offline consumers must apply bounded backpressure without dropping
// media. Source EOF precedes downstream draining.
class FileInput final : public Input {
 public:
  explicit FileInput(FileInputConfig config);
  ~FileInput() override;

  FileInput(const FileInput&) = delete;
  FileInput& operator=(const FileInput&) = delete;

  // Opens asynchronously on a dedicated thread. Empty paths throw before
  // borrowing observer; open/read failures arrive as state events. Each input
  // starts once. All callbacks execute on the file thread.
  void Start(Observer& observer) override;
  // Interrupts FFmpeg I/O and joins the file thread. The owner must release
  // any downstream backpressure before Stop; an observer call cannot be
  // forcibly interrupted. Calling Stop from a callback terminates.
  void Stop() noexcept override;
  InputState state() const noexcept override;
  NodeSnapshot GetPerformance() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_FILE_INPUT_H_
