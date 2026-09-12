#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_RECORDING_TARGET_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_RECORDING_TARGET_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "mw/zlm/config.h"

namespace mw::streamer {

// Internal recording targets. Construction, Write(), and Close() must be
// serialized on the same thread. preserve_packets disables first-keyframe
// filtering and timestamp correction, and reports rejected frames as errors.
class Fmp4FileTarget final {
 public:
  Fmp4FileTarget(const std::filesystem::path& requested_path,
                 const std::vector<mediakit::Track::Ptr>& tracks,
                 RecordingConfig config = {},
                 std::chrono::system_clock::time_point start_time =
                     std::chrono::system_clock::now(),
                 bool preserve_packets = false);
  ~Fmp4FileTarget();

  Fmp4FileTarget(const Fmp4FileTarget&) = delete;
  Fmp4FileTarget& operator=(const Fmp4FileTarget&) = delete;

  void Write(const mediakit::Frame::Ptr& frame);
  void Close();

  const std::filesystem::path& path() const noexcept;

 private:
  class Muxer;

  const bool preserve_packets_;
  std::filesystem::path path_;
  std::shared_ptr<Muxer> muxer_;
};

class HlsFmp4FileTarget final {
 public:
  HlsFmp4FileTarget(const std::filesystem::path& requested_path,
                    const std::vector<mediakit::Track::Ptr>& tracks,
                    RecordingConfig config = {},
                    std::chrono::system_clock::time_point start_time =
                        std::chrono::system_clock::now(),
                    bool preserve_packets = false);
  ~HlsFmp4FileTarget();

  HlsFmp4FileTarget(const HlsFmp4FileTarget&) = delete;
  HlsFmp4FileTarget& operator=(const HlsFmp4FileTarget&) = delete;

  void Write(const mediakit::Frame::Ptr& frame);
  void Close();

  const std::filesystem::path& path() const noexcept;

 private:
  class Recorder;

  const bool preserve_packets_;
  std::filesystem::path path_;
  std::shared_ptr<Recorder> recorder_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_RECORDING_TARGET_H_
