#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;

class RemuxerLog final {
 public:
  explicit RemuxerLog(mw::log::LogLevel level)
      : directory_(
            fs::temp_directory_path() /
            ("mw-remuxer-latency-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        runtime_(nullptr, &mw::streamer::Shutdown) {
    fs::create_directories(directory_);
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    config.log.async.enabled = false;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = (directory_ / "performance.log").string();
    config.log.modules = {{"perf.remuxer.convert", level},
                          {"perf.remuxer.output", level},
                          {"streamer", mw::log::LogLevel::kOff},
                          {"zlm", mw::log::LogLevel::kOff}};
    runtime_.reset(mw::streamer::Init(config));
  }

  ~RemuxerLog() {
    runtime_.reset();
    std::error_code error;
    fs::remove_all(directory_, error);
  }

  std::string OutputPath() const {
    return (directory_ / "output.mp4").generic_u8string();
  }

  bool OutputExists(const std::string& planned) const {
    return fs::exists(fs::u8path(planned));
  }

  std::string Read() {
    runtime_.reset();
    std::ifstream file(directory_ / "performance.log");
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file), {}};
  }

 private:
  fs::path directory_;
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime_;
};

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct RemuxMedia {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Packet> packets;

  RemuxMedia() {
    const auto path =
        (fs::u8path(MW_STREAMER_LATENCY_TEST_DATA_DIR) / "h264_video.mp4")
            .generic_u8string();
    AVFormatContext* raw = nullptr;
    const auto opened =
        avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
    REQUIRE(opened >= 0);
    REQUIRE(avformat_find_stream_info(raw, nullptr) >= 0);
    const int index =
        av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(index >= 0);
    const auto* native = raw->streams[index];
    stream = {index, ffmpeg::CodecParameters(*native->codecpar),
              native->time_base};
    for (;;) {
      ffmpeg::Packet packet;
      const int code = av_read_frame(raw, packet.get());
      if (code == AVERROR_EOF) break;
      REQUIRE(code >= 0);
      if (packet->stream_index == index) {
        packet->time_base = stream.time_base;
        packets.push_back(std::move(packet));
      }
    }
    REQUIRE_FALSE(packets.empty());
  }
};

void CheckRecording(const std::string& path, std::size_t packet_count) {
  AVFormatContext* raw = nullptr;
  const int opened = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  REQUIRE(opened >= 0);
  REQUIRE(avformat_find_stream_info(raw, nullptr) >= 0);
  REQUIRE(raw->nb_streams == 1);
  CHECK(raw->streams[0]->codecpar->codec_id == AV_CODEC_ID_H264);
  std::size_t count = 0;
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(raw, packet.get());
    if (result == AVERROR_EOF) break;
    REQUIRE(result >= 0);
    ++count;
  }
  CHECK(count == packet_count);
}

std::vector<std::string> RemuxSummaries(const std::string& log,
                                        const std::string& stage) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin < log.size()) {
    const auto end = log.find('\n', begin);
    auto line = log.substr(begin, end - begin);
    if (line.find("stage=" + stage + " ") != std::string::npos &&
        line.find("report=summary") != std::string::npos) {
      result.push_back(std::move(line));
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

std::size_t RemuxCounter(const std::string& line, const char* field) {
  std::smatch match;
  REQUIRE(std::regex_search(line, match,
                            std::regex(std::string(field) + "=([0-9]+)")));
  return static_cast<std::size_t>(std::stoull(match[1].str()));
}

TEST_CASE("Remuxer 只统计实际执行的协议转换和同步输出并独立结束会话",
          "[performance][latency][remuxer]") {
  const RemuxMedia media;
  bool recording_only = false;
  SECTION("无外部输出仍执行协议转换") {}
  SECTION("仅 MP4 录像跳过协议转换且完整排空") { recording_only = true; }
  RemuxerLog logging(mw::log::LogLevel::kInfo);
  {
    std::atomic<bool> failed{false};
    mw::streamer::Remuxer remuxer;
    remuxer.SetOnError([&](std::exception_ptr) { failed = true; });
    for (int session = 0; session < 2; ++session) {
      std::string planned;
      if (recording_only) planned = remuxer.AddPushUrl(logging.OutputPath());
      REQUIRE(remuxer.Start({media.stream}));
      for (const auto& packet : media.packets) {
        REQUIRE(remuxer.SubmitPacket(packet));
      }
      remuxer.Stop();
      remuxer.Stop();
      REQUIRE_FALSE(failed.load());
      if (recording_only) {
        REQUIRE(logging.OutputExists(planned));
        CheckRecording(planned, media.packets.size());
        const auto recordings = remuxer.recording_statuses();
        REQUIRE(recordings.size() == 1);
        CHECK(recordings.front().status.written_packets ==
              media.packets.size());
        CHECK(recordings.front().status.dropped_packets == 0);
        CHECK(recordings.front().status.error.empty());
      }
      REQUIRE(remuxer.push_statuses().empty());
    }
  }
  const auto log = logging.Read();
  for (const auto* stage : {"remux.convert", "remux.output"}) {
    const auto summaries = RemuxSummaries(log, stage);
    REQUIRE(summaries.size() == 2);
    for (const auto& summary : summaries) {
      CHECK(RemuxCounter(summary, "samples") ==
            (recording_only ? 0 : media.packets.size()));
      CHECK(RemuxCounter(summary, "unavailable") == 0);
    }
  }
}

TEST_CASE("关闭 Remuxer latency 不产生阶段统计且仍能正常排空",
          "[performance][latency][remuxer][disabled]") {
  const RemuxMedia media;
  RemuxerLog logging(mw::log::LogLevel::kOff);
  {
    std::atomic<bool> failed{false};
    mw::streamer::Remuxer remuxer;
    remuxer.SetOnError([&](std::exception_ptr) { failed = true; });
    const auto planned = remuxer.AddPushUrl(logging.OutputPath());
    REQUIRE(remuxer.Start({media.stream}));
    for (const auto& packet : media.packets) {
      REQUIRE(remuxer.SubmitPacket(packet));
    }
    remuxer.Stop();
    REQUIRE_FALSE(failed.load());
    REQUIRE(logging.OutputExists(planned));
    CheckRecording(planned, media.packets.size());
  }
  const auto log = logging.Read();
  CHECK(log.find("stage=remux.") == std::string::npos);
}

}  // namespace
