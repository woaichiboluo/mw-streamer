#include "output/recording_target.h"

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "Record/HlsMakerImp.h"
#include "Record/MP4Muxer.h"
#include "mw/log/logging.h"

namespace mw::streamer::output {
namespace {

constexpr std::string_view kMp4Extension = ".mp4";
constexpr std::string_view kHlsExtension = ".m3u8";
constexpr std::uint32_t kHlsFileBufferSize = 64 * 1024;
constexpr float kHlsSegmentDurationSeconds = 2.0F;
constexpr std::uint32_t kHlsRecordingSegmentCount = 0;

std::string FormatStartTime(std::chrono::system_clock::time_point start_time) {
  const std::time_t time = std::chrono::system_clock::to_time_t(start_time);
  std::tm local_time{};
#if defined(_WIN32)
  if (localtime_s(&local_time, &time) != 0) {
#else
  if (!localtime_r(&time, &local_time)) {
#endif
    throw std::runtime_error("无法格式化录像开始时间");
  }

  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          start_time.time_since_epoch())
          .count() %
      1000;
  std::ostringstream stream;
  stream << std::put_time(&local_time, "%Y%m%d_%H%M%S_") << std::setfill('0')
         << std::setw(3) << milliseconds;
  return stream.str();
}

void ValidateRequestedPath(const std::filesystem::path& requested_path,
                           std::string_view extension) {
  if (requested_path.empty() || requested_path.filename().empty() ||
      requested_path.extension() != extension) {
    throw std::invalid_argument("录像目标路径扩展名无效");
  }
}

std::filesystem::path MakeTimestampedFilePath(
    const std::filesystem::path& requested_path,
    std::chrono::system_clock::time_point start_time) {
  ValidateRequestedPath(requested_path, kMp4Extension);
  return requested_path.parent_path() /
         (requested_path.stem().string() + "_" + FormatStartTime(start_time) +
          requested_path.extension().string());
}

std::filesystem::path MakeTimestampedHlsPath(
    const std::filesystem::path& requested_path,
    std::chrono::system_clock::time_point start_time) {
  ValidateRequestedPath(requested_path, kHlsExtension);
  return requested_path.parent_path() /
         (requested_path.stem().string() + "_" + FormatStartTime(start_time)) /
         "index.m3u8";
}

template <typename Recorder>
void AddTracks(Recorder& recorder,
               const std::vector<mediakit::Track::Ptr>& tracks) {
  if (tracks.empty()) {
    throw std::invalid_argument("录像至少需要一个Track");
  }
  for (const auto& track : tracks) {
    if (!track || !track->ready()) {
      throw std::invalid_argument("录像Track未就绪");
    }
    if (!recorder.addTrack(track)) {
      throw std::invalid_argument("ZLM录像器不支持该Track");
    }
  }
  recorder.addTrackCompleted();
}

}  // namespace

class Fmp4FileTarget::Muxer final : public mediakit::MP4MuxerInterface {
 public:
  explicit Muxer(const std::filesystem::path& path)
      : file_(std::make_shared<mediakit::MP4FileDisk>()) {
    file_->openFile(path.string().c_str(), "wb+");
  }

  void Close() {
    resetTracks();
    file_.reset();
  }

 protected:
  mediakit::MP4FileIO::Writer createWriter() override {
    return file_->createWriter(0, true);
  }

 private:
  mediakit::MP4FileDisk::Ptr file_;
};

class HlsFmp4FileTarget::Recorder final : public mediakit::MP4MuxerMemory {
 public:
  explicit Recorder(const std::filesystem::path& path)
      : hls_(std::make_shared<mediakit::HlsMakerImp>(
            true, path.string(), std::string(), kHlsFileBufferSize,
            kHlsSegmentDurationSeconds, kHlsRecordingSegmentCount, false,
            ".mp4")) {}

  ~Recorder() override {
    try {
      flush();
    } catch (const std::exception& error) {
      log::Module<log::LogModule::kStreamer>::Error("刷新HLS-fMP4录像失败：{}",
                                                    error.what());
    }
  }

  void addTrackCompleted() override {
    mediakit::MP4MuxerMemory::addTrackCompleted();
    const auto& data = getInitSegment();
    hls_->inputInitSegment(data.data(), data.size());
  }

 private:
  void onSegmentData(std::string buffer, std::uint64_t timestamp,
                     bool key_position) override {
    if (buffer.empty()) {
      hls_->inputData(nullptr, 0, timestamp, key_position);
      return;
    }
    hls_->inputData(buffer.data(), buffer.size(), timestamp, key_position);
  }

  std::shared_ptr<mediakit::HlsMakerImp> hls_;
};

Fmp4FileTarget::Fmp4FileTarget(const std::filesystem::path& requested_path,
                               const std::vector<mediakit::Track::Ptr>& tracks,
                               std::chrono::system_clock::time_point start_time)
    : path_(MakeTimestampedFilePath(requested_path, start_time)),
      muxer_(std::make_shared<Muxer>(path_)) {
  try {
    AddTracks(*muxer_, tracks);
  } catch (...) {
    muxer_->Close();
    muxer_.reset();
    throw;
  }
}

Fmp4FileTarget::~Fmp4FileTarget() {
  try {
    Close();
  } catch (const std::exception& error) {
    log::Module<log::LogModule::kStreamer>::Error("关闭fMP4录像失败：{}",
                                                  error.what());
  }
}

void Fmp4FileTarget::Write(const mediakit::Frame::Ptr& frame) {
  if (muxer_ && frame) {
    muxer_->inputFrame(frame);
  }
}

void Fmp4FileTarget::Close() {
  if (!muxer_) {
    return;
  }
  muxer_->flush();
  muxer_->Close();
  muxer_.reset();
}

const std::filesystem::path& Fmp4FileTarget::path() const noexcept {
  return path_;
}

HlsFmp4FileTarget::HlsFmp4FileTarget(
    const std::filesystem::path& requested_path,
    const std::vector<mediakit::Track::Ptr>& tracks,
    std::chrono::system_clock::time_point start_time)
    : path_(MakeTimestampedHlsPath(requested_path, start_time)),
      recorder_(std::make_shared<Recorder>(path_)) {
  try {
    AddTracks(*recorder_, tracks);
  } catch (...) {
    recorder_.reset();
    throw;
  }
}

HlsFmp4FileTarget::~HlsFmp4FileTarget() {
  try {
    Close();
  } catch (const std::exception& error) {
    log::Module<log::LogModule::kStreamer>::Error("关闭HLS-fMP4录像失败：{}",
                                                  error.what());
  }
}

void HlsFmp4FileTarget::Write(const mediakit::Frame::Ptr& frame) {
  if (recorder_ && frame) {
    recorder_->inputFrame(frame);
  }
}

void HlsFmp4FileTarget::Close() {
  if (!recorder_) {
    return;
  }
  recorder_->flush();
  recorder_.reset();
}

const std::filesystem::path& HlsFmp4FileTarget::path() const noexcept {
  return path_;
}

}  // namespace mw::streamer::output
