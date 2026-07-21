#include "mw/streamer/internal/output_worker.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mw/streamer/internal/demuxer.h"

namespace mw::streamer::internal {
namespace {

std::filesystem::path MakeOutputPath(std::string_view name) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("mw-streamer-output-worker-" + std::string(name) + "-" +
          std::to_string(suffix) + ".mp4");
}

TEST(OutputWorkerIntegrationTest, WritesAndFinishesFragmentedMp4) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  const std::filesystem::path output_path = MakeOutputPath("complete");

  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  OutputConfig config;
  config.id = "recording";
  config.protocol = OutputProtocol::kFile;
  config.url = output_path.string();
  config.open_timeout = std::chrono::milliseconds(1);
  config.write_timeout = std::chrono::milliseconds(1);

  std::mutex events_mutex;
  std::vector<EndpointEventType> events;
  OutputWorker worker(
      config, input.video_stream().codec_parameters,
      input.video_stream().time_base, nullptr, AVRational{0, 1},
      [&events_mutex, &events](EndpointEventType type, const Status&) {
        std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(type);
      });
  ASSERT_NO_THROW(worker.Start());

  int input_packet_count = 0;
  while (auto packet = input.Read()) {
    EncodedPacket encoded(EncodedMediaType::kVideo, std::move(*packet));
    ASSERT_TRUE(worker.Enqueue(encoded));
    ++input_packet_count;
  }
  worker.Close();
  worker.Wait();
  ASSERT_GT(input_packet_count, 0);

  {
    std::lock_guard<std::mutex> lock(events_mutex);
    EXPECT_TRUE(events.empty());
  }

  Demuxer output;
  ASSERT_NO_THROW(output.Open(output_path.string()));
  int output_packet_count = 0;
  while (output.Read().has_value()) {
    ++output_packet_count;
  }
  EXPECT_EQ(output_packet_count, input_packet_count);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

TEST(OutputWorkerIntegrationTest, ExistingFileFailsWithoutStartingWorker) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  const std::filesystem::path output_path = MakeOutputPath("exists");
  {
    std::ofstream existing(output_path);
    ASSERT_TRUE(existing.is_open());
  }

  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  OutputConfig config;
  config.id = "existing-file";
  config.protocol = OutputProtocol::kFile;
  config.url = output_path.string();

  std::vector<EndpointEventType> events;
  OutputWorker worker(config, input.video_stream().codec_parameters,
                      input.video_stream().time_base, nullptr, AVRational{0, 1},
                      [&events](EndpointEventType type, const Status&) {
                        events.push_back(type);
                      });
  ASSERT_NO_THROW(worker.Start());
  EXPECT_FALSE(worker.is_accepting());
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front(), EndpointEventType::kFailed);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

}  // namespace
}  // namespace mw::streamer::internal
