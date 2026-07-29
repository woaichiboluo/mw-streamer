#include "mw/output/recording_target.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "Common/config.h"
#include "Record/MP4Demuxer.h"
#include "Util/onceToken.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using mw::streamer::output::Fmp4FileTarget;
using mw::streamer::output::HlsFmp4FileTarget;

std::filesystem::path SamplePath() {
  return std::filesystem::path(MW_RECORDING_TARGET_TEST_DATA_DIR) /
         "packet_queue_8s.mp4";
}

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw_streamer_recording_" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("ZLM录像目标生成带相同开始时间的fMP4和HLS-fMP4") {
  auto& zlm_config = toolkit::mINI::Instance();
  const auto old_enable_fmp4 = zlm_config[mediakit::Record::kEnableFmp4];
  const auto old_hls_segment_count = zlm_config[mediakit::Hls::kSegmentNum];
  toolkit::onceToken restore_zlm_config(
      nullptr, [&zlm_config, old_enable_fmp4, old_hls_segment_count]() {
        zlm_config[mediakit::Record::kEnableFmp4] = old_enable_fmp4;
        zlm_config[mediakit::Hls::kSegmentNum] = old_hls_segment_count;
      });
  zlm_config[mediakit::Record::kEnableFmp4] = false;
  zlm_config[mediakit::Hls::kSegmentNum] = 3;

  TestDirectory directory;
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(SamplePath().string());
  const auto tracks = demuxer.getTracks(true);
  REQUIRE(tracks.size() == 2);

  const auto start_time = std::chrono::system_clock::now();
  Fmp4FileTarget fmp4(directory.path() / "camera.mp4", tracks, start_time);
  HlsFmp4FileTarget hls(directory.path() / "camera.m3u8", tracks, start_time);

  bool eof = false;
  std::size_t frame_count = 0;
  while (!eof) {
    bool key_frame = false;
    int error = 0;
    auto frame = demuxer.readFrame(key_frame, eof, &error);
    REQUIRE(error == 0);
    if (!frame) {
      continue;
    }
    if (key_frame && !frame->keyFrame()) {
      frame = std::make_shared<mediakit::FrameCacheAble>(frame, true);
    }
    fmp4.Write(frame);
    hls.Write(frame);
    ++frame_count;
  }
  REQUIRE(frame_count > 0);

  const auto fmp4_path = fmp4.path();
  const auto hls_path = hls.path();
  fmp4.Close();
  hls.Close();

  CHECK(zlm_config[mediakit::Record::kEnableFmp4] == false);
  CHECK(zlm_config[mediakit::Hls::kSegmentNum] == 3);
  REQUIRE(fmp4_path.parent_path() == directory.path());
  REQUIRE(fmp4_path.extension() == ".mp4");
  REQUIRE(fmp4_path.stem().string().rfind("camera_", 0) == 0);
  REQUIRE(hls_path.filename() == "index.m3u8");
  REQUIRE(hls_path.parent_path().parent_path() == directory.path());
  REQUIRE(hls_path.parent_path().filename().string().rfind("camera_", 0) == 0);
  CHECK(fmp4_path.stem().string().substr(7) ==
        hls_path.parent_path().filename().string().substr(7));

  const auto fmp4_data = ReadFile(fmp4_path);
  CHECK(fmp4_data.find("moof") != std::string::npos);

  mediakit::MP4Demuxer recorded_demuxer;
  recorded_demuxer.openMP4(fmp4_path.string());
  CHECK(recorded_demuxer.getTracks(true).size() == 2);
  CHECK(recorded_demuxer.getDurationMS() >= 7900);

  const auto playlist = ReadFile(hls_path);
  CHECK(playlist.find("#EXT-X-MAP:URI=\"init.mp4\"") != std::string::npos);
  CHECK(playlist.find("#EXT-X-ENDLIST") != std::string::npos);

  std::size_t media_segment_count = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(hls_path.parent_path())) {
    if (entry.is_regular_file() && entry.path().extension() == ".mp4" &&
        entry.path().filename() != "init.mp4") {
      ++media_segment_count;
    }
  }
  CHECK(media_segment_count >= 3);
}
