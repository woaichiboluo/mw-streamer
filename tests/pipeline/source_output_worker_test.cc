#include "mw/pipeline/internal/remux/source_output_worker.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
}

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::pipeline::internal::remux::SourceOutputWorker;
using mw::streamer::pipeline::internal::remux::SourceOutputWorkerState;

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-source-output-worker-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

StreamInfo MakeVideoStream() {
  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters.get()->codec_id = AV_CODEC_ID_H264;
  parameters.get()->width = 64;
  parameters.get()->height = 64;
  return {0, std::move(parameters), AVRational{1, 1000}};
}

Packet MakePacket(std::int64_t dts, std::int64_t pts) {
  Packet packet;
  packet->stream_index = 0;
  packet->dts = dts;
  packet->pts = pts;
  packet->flags = AV_PKT_FLAG_KEY;
  return packet;
}

}  // namespace

TEST_CASE("SourceOutputWorker允许PTS回退并在DTS回退后停止旁路") {
  TestDirectory directory;
  std::size_t failure_calls = 0;
  SourceOutputWorker worker({(directory.path() / "source.mp4").string()}, {}, 8,
                            nullptr, [&](const char*) { ++failure_calls; });
  worker.Open({MakeVideoStream()});
  REQUIRE(worker.state() == SourceOutputWorkerState::kRunning);
  REQUIRE(worker.output_session());

  CHECK(worker.Write(1, MakePacket(100, 200)));
  CHECK(worker.Write(1, MakePacket(101, 150)));
  CHECK_FALSE(worker.Write(2, MakePacket(100, 201)));
  CHECK(worker.state() == SourceOutputWorkerState::kFailed);
  CHECK_FALSE(worker.Write(2, MakePacket(102, 202)));
  CHECK(failure_calls == 1);

  worker.Stop();
  worker.Stop();
  CHECK(worker.state() == SourceOutputWorkerState::kFailed);
  CHECK_FALSE(worker.output_session());
}
