#ifndef MW_STREAMER_SRC_OUTPUT_RECORDING_TARGET_H_
#define MW_STREAMER_SRC_OUTPUT_RECORDING_TARGET_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mw::streamer::output {

// Internal recording targets. Construction, Write(), and Close() must be
// serialized on the same thread.
class Fmp4FileTarget final {
 public:
  Fmp4FileTarget(const std::filesystem::path& requested_path,
                 const std::vector<mediakit::Track::Ptr>& tracks,
                 std::chrono::system_clock::time_point start_time =
                     std::chrono::system_clock::now());
  ~Fmp4FileTarget();

  Fmp4FileTarget(const Fmp4FileTarget&) = delete;
  Fmp4FileTarget& operator=(const Fmp4FileTarget&) = delete;

  void Write(const mediakit::Frame::Ptr& frame);
  void Close();

  const std::filesystem::path& path() const noexcept;

 private:
  class Muxer;

  std::filesystem::path path_;
  std::shared_ptr<Muxer> muxer_;
};

class HlsFmp4FileTarget final {
 public:
  HlsFmp4FileTarget(const std::filesystem::path& requested_path,
                    const std::vector<mediakit::Track::Ptr>& tracks,
                    std::chrono::system_clock::time_point start_time =
                        std::chrono::system_clock::now());
  ~HlsFmp4FileTarget();

  HlsFmp4FileTarget(const HlsFmp4FileTarget&) = delete;
  HlsFmp4FileTarget& operator=(const HlsFmp4FileTarget&) = delete;

  void Write(const mediakit::Frame::Ptr& frame);
  void Close();

  const std::filesystem::path& path() const noexcept;

 private:
  class Recorder;

  std::filesystem::path path_;
  std::shared_ptr<Recorder> recorder_;
};

}  // namespace mw::streamer::output

#endif  // MW_STREAMER_SRC_OUTPUT_RECORDING_TARGET_H_
