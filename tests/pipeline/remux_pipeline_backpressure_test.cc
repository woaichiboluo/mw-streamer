#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "../encoder/encoder_test_support.h"
#include "Common/config.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Record/Recorder.h"
#include "Rtsp/RtspSession.h"
#include "Util/mini.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/pipeline/remux_pipeline.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_ = nullptr;
};

class OutputDirectory final {
 public:
  OutputDirectory()
      : root_(std::filesystem::absolute(std::filesystem::current_path())
                  .lexically_normal()),
        path_(root_ / ("remux-pipeline-backpressure-" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()))) {
    std::filesystem::create_directories(path_);
  }
  ~OutputDirectory() {
    if (path_.parent_path() == root_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }
  std::string File() const { return (path_ / "record.mp4").generic_u8string(); }

 private:
  std::filesystem::path root_;
  std::filesystem::path path_;
};

struct FormatCloser final {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::vector<ffmpeg::Packet> ReadPackets(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开测试录像");
  std::unique_ptr<AVFormatContext, FormatCloser> context(raw);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "探测测试录像");
  REQUIRE(raw->nb_streams == 1);
  REQUIRE(raw->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
  std::vector<ffmpeg::Packet> packets;
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(raw, packet.get());
    if (result == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(result, "读取测试录像");
    packets.push_back(std::move(packet));
  }
  return packets;
}

std::size_t PacketBytes(const AVPacket& packet) {
  std::size_t bytes = static_cast<std::size_t>(packet.size);
  for (int index = 0; index < packet.side_data_elems; ++index) {
    bytes += packet.side_data[index].size;
  }
  return bytes;
}

std::pair<std::uint16_t, std::uint16_t> ReservePorts() {
  const auto poller = toolkit::EventPollerPool::Instance().getPoller();
  auto first = std::make_shared<toolkit::TcpServer>(poller);
  auto second = std::make_shared<toolkit::TcpServer>(poller);
  first->start<mediakit::RtspSession>(0, "127.0.0.1");
  second->start<mediakit::RtspSession>(0, "127.0.0.1");
  const std::pair<std::uint16_t, std::uint16_t> ports{first->getPort(),
                                                      second->getPort()};
  poller->sync([&] {
    first.reset();
    second.reset();
  });
  return ports;
}

}  // namespace

TEST_CASE("RemuxPipeline停止等待原包背压完成并排空录制",
          "[pipeline][remux][backpressure]") {
  Runtime runtime;
  OutputDirectory directory;
  const std::string source_path =
      std::string(MW_STREAMER_PIPELINE_TEST_DATA_DIR) + "/h264_video.mp4";
  const auto source = ReadPackets(source_path);
  REQUIRE(source.size() >= 2);
  REQUIRE((source.front()->flags & AV_PKT_FLAG_KEY) != 0);
  const auto capacity = PacketBytes(*source.front().get());
  for (const auto& packet : source) {
    REQUIRE(PacketBytes(*packet.get()) <= capacity);
  }

  encoder_test::Gate gate;
  mw::streamer::RemuxPipelineConfig config;
  config.input.loop = true;
  config.remuxer.max_pending_bytes = capacity;
  mw::streamer::RemuxPipeline pipeline(config);
  pipeline.AddPushUrl(directory.File());
  // Release before pipeline destruction if any subsequent assertion fails.
  encoder_test::ReleaseGate release_pipeline{gate};
  std::string output_path;
  pipeline.SetOnReady(
      [&](const auto& ready) { output_path = ready.output_urls.at(0); });
  pipeline.Start(source_path);
  toolkit::EventPollerPool::Instance().getPoller()->async(
      [&] { gate.Enter(); });
  REQUIRE(gate.Wait());
  REQUIRE(pipeline.state() == mw::streamer::PipelineState::kRunning);

  // Freeze after Start has received the first processed packet. Give local
  // demuxing time to block SubmitPacket on the poller before stopping Input.
  std::this_thread::sleep_for(100ms);
  std::promise<void> started;
  auto entered = started.get_future();
  auto stop = std::async(std::launch::async, [&] {
    started.set_value();
    pipeline.Stop();
  });
  // Future destruction joins its thread, so this guard must precede it.
  encoder_test::ReleaseGate release_stop{gate};
  entered.wait();
  CHECK(stop.wait_for(100ms) == std::future_status::timeout);
  gate.Release();
  REQUIRE(stop.wait_for(10s) == std::future_status::ready);
  stop.get();
  CHECK(pipeline.state() == mw::streamer::PipelineState::kStopped);

  const auto recorded = ReadPackets(output_path);
  // The first packet and subsequent accepted work must survive Stop.
  REQUIRE(recorded.size() >= 2);
  CHECK(recorded.at(1)->dts > recorded.front()->dts);
}

TEST_CASE("RemuxPipeline添加RTSP和HLS发布并在停止后释放发布点",
          "[pipeline][remux][publish]") {
  const bool before_start = GENERATE(true, false);
  Runtime runtime;
  const auto ports = ReservePorts();
  REQUIRE(ports.first != ports.second);
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string app = "pipeline_" + suffix;
  const std::string stream = "publication_" + suffix;
  mw::streamer::RemuxPipelineConfig config;
  config.input.loop = true;
  const auto rtsp_url = "rtsp://127.0.0.1:" + std::to_string(ports.first) +
                        "/" + app + "/" + stream;
  const auto hls_url = "http://127.0.0.1:" + std::to_string(ports.second) +
                       "/" + app + "/" + stream + "/hls.m3u8";
  const auto root =
      toolkit::mINI::Instance()[mediakit::Http::kRootPath].as<std::string>();
  const std::filesystem::path playlist = mediakit::Recorder::getRecordPath(
      mediakit::Recorder::type_hls, {DEFAULT_VHOST, app, stream}, root);
  const std::string source_path =
      std::string(MW_STREAMER_PIPELINE_TEST_DATA_DIR) + "/h264_video.mp4";

  mw::streamer::RemuxPipeline first(config), second(config);
  const auto add_outputs = [&](auto& pipeline) {
    pipeline.AddRtspPublish(app, stream, "127.0.0.1", ports.first);
    CHECK(pipeline.AddHlsPublish(app, stream, "127.0.0.1", ports.second) ==
          hls_url);
  };
  if (before_start) add_outputs(first);
  add_outputs(second);
  first.Start(source_path);
  if (!before_start) add_outputs(first);
  REQUIRE(first.state() == mw::streamer::PipelineState::kRunning);
  CHECK(first.ready_info().rtsp_url == rtsp_url);
  CHECK(first.ready_info().hls_url == hls_url);

  CHECK_THROWS_AS(second.Start(source_path), std::invalid_argument);
  CHECK(second.state() == mw::streamer::PipelineState::kFailed);
  CHECK(first.state() == mw::streamer::PipelineState::kRunning);
  first.Stop();
  REQUIRE(first.state() == mw::streamer::PipelineState::kStopped);

  add_outputs(second);
  second.Start(source_path);
  REQUIRE(second.state() == mw::streamer::PipelineState::kRunning);
  CHECK(second.ready_info().rtsp_url == rtsp_url);
  CHECK(second.ready_info().hls_url == hls_url);
  second.Stop();
  CHECK(second.state() == mw::streamer::PipelineState::kStopped);
  CHECK_FALSE(std::filesystem::exists(playlist));
  if (std::filesystem::exists(playlist.parent_path())) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             playlist.parent_path())) {
      if (!entry.is_regular_file()) continue;
      CAPTURE(entry.path().generic_u8string());
      CHECK(entry.path().extension() != ".ts");
      CHECK(entry.path().extension() != ".m3u8");
    }
  }
}
