#include "mw/output/output_session.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Record/MP4Demuxer.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::output::OutputConfig;
using mw::streamer::output::OutputSession;

std::filesystem::path SamplePath() {
  return std::filesystem::path(MW_OUTPUT_SESSION_TEST_DATA_DIR) /
         "packet_queue_8s.mp4";
}

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw_streamer_output_" + std::to_string(suffix));
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

struct Binding {
  mediakit::Track::Ptr track;
  ZlmPacketConverter::Ptr converter;
};

void AddStreams(const std::vector<mediakit::Track::Ptr>& tracks,
                OutputConfig& config) {
  int stream_index = 0;
  for (const auto& track : tracks) {
    ZlmCodecParametersConverter parameters(track);
    config.streams.push_back(
        {stream_index, parameters.codec_parameters(), parameters.time_base()});
    ++stream_index;
  }
}

std::vector<Binding> MakeBindings(
    const std::vector<mediakit::Track::Ptr>& tracks, OutputSession& output) {
  std::vector<Binding> bindings;
  int stream_index = 0;
  for (const auto& track : tracks) {
    auto converter = std::make_shared<ZlmPacketConverter>(track, stream_index);
    converter->SetOnPacket([&output](const Packet& packet) {
      output.Write(packet);
      return true;
    });
    bindings.push_back({track, std::move(converter)});
    ++stream_index;
  }
  return bindings;
}

std::filesystem::path FindGeneratedFmp4(
    const std::filesystem::path& directory) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
      return entry.path();
    }
  }
  return {};
}

std::filesystem::path FindGeneratedHls(const std::filesystem::path& directory) {
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().filename() == "index.m3u8") {
      return entry.path();
    }
  }
  return {};
}

}  // namespace

TEST_CASE("OutputSession隔离网络失败并同时生成fMP4和HLS-fMP4") {
  TestDirectory directory;
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(SamplePath().string());
  const auto tracks = demuxer.getTracks(true);
  REQUIRE(tracks.size() == 2);

  OutputConfig config;
  config.targets = {
      "rtmp://127.0.0.1:1/live/unreachable",
      "rtsp://127.0.0.1:1/live/unreachable",
      "srt://127.0.0.1:1?mode=caller",
      (directory.path() / "camera.mp4").string(),
      (directory.path() / "camera.m3u8").string(),
  };

  AddStreams(tracks, config);
  OutputSession session(std::move(config));
  auto bindings = MakeBindings(tracks, session);
  REQUIRE_NOTHROW(session.Open());

  std::unordered_map<int, ZlmPacketConverter::Ptr> converters;
  for (const auto& binding : bindings) {
    converters.emplace(binding.track->getIndex(), binding.converter);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

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
    const auto converter = converters.find(frame->getIndex());
    REQUIRE(converter != converters.end());
    CHECK(converter->second->InputFrame(frame));
    ++frame_count;
  }
  REQUIRE(frame_count > 0);
  for (const auto& binding : bindings) {
    CHECK(binding.converter->Flush());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto network_traffic = session.GetNetworkTraffic();
  REQUIRE(network_traffic.size() == 3);
  CHECK(network_traffic[0].target == "rtmp://127.0.0.1:1/live/unreachable");
  CHECK(network_traffic[1].target == "rtsp://127.0.0.1:1/live/unreachable");
  CHECK(network_traffic[2].target == "srt://127.0.0.1:1?mode=caller");
  session.Close();

  const auto fmp4_path = FindGeneratedFmp4(directory.path());
  REQUIRE_FALSE(fmp4_path.empty());
  mediakit::MP4Demuxer recorded;
  recorded.openMP4(fmp4_path.string());
  CHECK(recorded.getTracks(true).size() == 2);
  CHECK(recorded.getDurationMS() >= 7700);

  const auto hls_path = FindGeneratedHls(directory.path());
  REQUIRE_FALSE(hls_path.empty());
  CHECK(hls_path.filename() == "index.m3u8");
}

TEST_CASE("OutputSession同步拒绝无目标和未知网络协议") {
  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters.get()->codec_id = AV_CODEC_ID_H264;

  OutputConfig no_target;
  no_target.streams.push_back({0, parameters, AVRational{1, 1000}});
  OutputSession no_target_session(std::move(no_target));
  CHECK_THROWS_AS(no_target_session.Open(), std::invalid_argument);

  OutputConfig unknown_protocol;
  unknown_protocol.streams.push_back({0, parameters, AVRational{1, 1000}});
  unknown_protocol.targets.push_back("udp://127.0.0.1:9000/live");
  OutputSession unknown_protocol_session(std::move(unknown_protocol));
  CHECK_THROWS_AS(unknown_protocol_session.Open(), std::invalid_argument);

  OutputConfig invalid_zlm;
  invalid_zlm.streams.push_back({0, parameters, AVRational{1, 1000}});
  invalid_zlm.targets.push_back("rtmp://127.0.0.1/live/test");
  invalid_zlm.zlm.recording.hls_segment_duration =
      std::chrono::milliseconds::zero();
  OutputSession invalid_zlm_session(std::move(invalid_zlm));
  CHECK_THROWS_AS(invalid_zlm_session.Open(), std::invalid_argument);
}

TEST_CASE("OutputSession在所有文件目标永久失效后通知一次") {
  TestDirectory directory;
  const auto blocked_path = directory.path() / "blocked";
  std::ofstream(blocked_path).put('x');

  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(SamplePath().string());
  const auto tracks = demuxer.getTracks(true);
  REQUIRE_FALSE(tracks.empty());

  OutputConfig config;
  config.targets = {(blocked_path / "input.mp4").string()};
  AddStreams(tracks, config);
  OutputSession session(std::move(config));
  std::mutex mutex;
  std::condition_variable condition;
  std::size_t unavailable_calls = 0;
  session.SetOnAllTargetsUnavailable([&]() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++unavailable_calls;
    }
    condition.notify_all();
  });
  auto bindings = MakeBindings(tracks, session);
  session.Open();

  std::unordered_map<int, ZlmPacketConverter::Ptr> converters;
  for (const auto& binding : bindings) {
    converters.emplace(binding.track->getIndex(), binding.converter);
  }
  bool eof = false;
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
    REQUIRE(converters.at(frame->getIndex())->InputFrame(frame));
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(condition.wait_for(lock, std::chrono::seconds(2),
                               [&]() { return unavailable_calls == 1; }));
  }
  session.Close();
  CHECK(unavailable_calls == 1);
  CHECK_FALSE(std::filesystem::is_directory(blocked_path));
}
