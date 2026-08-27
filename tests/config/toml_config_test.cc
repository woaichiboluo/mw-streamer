#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "mw/config/toml.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::config::LoadFilePipelineConfigFromToml;
using mw::streamer::config::LoadInitConfigFromToml;
using mw::streamer::config::LoadRemuxPipelineConfigFromToml;
using mw::streamer::config::LoadStreamingPipelineConfigFromToml;

class TemporaryToml final {
 public:
  explicit TemporaryToml(std::string_view content) {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-config-" + std::to_string(suffix) + ".toml");
    std::ofstream output(path_, std::ios::binary);
    REQUIRE(output);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output);
  }

  ~TemporaryToml() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("TOML加载完整Init配置") {
  TemporaryToml file(R"(
[log.modules]
zlm = "info"
srt = "debug"
ffmpeg = "warning"
streamer = "error"
processor = "critical"

[log.console]
enabled = false
color = false
level = "debug"

[log.rotating_file]
enabled = true
path = "./streamer.log"
level = "info"
max_file_size = 4096
max_files = 7

[log.async]
enabled = true
queue_size = 256
overflow = "block"

[zlm]
event_poller_threads = 2
work_threads = 3
enable_cpu_affinity = false
)");

  const auto config = LoadInitConfigFromToml(file.path());
  CHECK(config.log.modules.zlm == mw::streamer::log::LogLevel::kInfo);
  CHECK(config.log.modules.srt == mw::streamer::log::LogLevel::kDebug);
  CHECK(config.log.modules.ffmpeg == mw::streamer::log::LogLevel::kWarning);
  CHECK(config.log.modules.streamer == mw::streamer::log::LogLevel::kError);
  CHECK(config.log.modules.processor == mw::streamer::log::LogLevel::kCritical);
  CHECK_FALSE(config.log.console.enabled);
  CHECK_FALSE(config.log.console.color);
  CHECK(config.log.console.level == mw::streamer::log::LogLevel::kDebug);
  CHECK(config.log.rotating_file.enabled);
  CHECK(config.log.rotating_file.path == "./streamer.log");
  CHECK(config.log.rotating_file.level == mw::streamer::log::LogLevel::kInfo);
  CHECK(config.log.rotating_file.max_file_size == 4096);
  CHECK(config.log.rotating_file.max_files == 7);
  CHECK(config.log.async.enabled);
  CHECK(config.log.async.queue_size == 256);
  CHECK(config.log.async.overflow == mw::streamer::log::OverflowPolicy::kBlock);
  CHECK(config.zlm.event_poller_threads == 2);
  CHECK(config.zlm.work_threads == 3);
  CHECK_FALSE(config.zlm.enable_cpu_affinity);
}

TEST_CASE("TOML加载完整StreamingPipeline配置") {
  TemporaryToml file(R"(
input_url = "srt://127.0.0.1:9000"
cache_duration_ms = 1500
audio_queue_capacity = 64
video_queue_capacity = 32
max_track_wait_ms = 250
input_targets = ["./input.mp4", "rtsp://127.0.0.1/input"]
output_targets = ["./output.mp4"]

[zlm.player]
connect_timeout_ms = 3000
media_timeout_ms = 4000
local_bind_ip = "127.0.0.2"

[zlm.output.pusher]
connect_timeout_ms = 5000
local_bind_ip = "127.0.0.3"

[zlm.output.muxer]
paced_sender_interval_ms = 5

[zlm.output.recording]
file_buffer_size = 131072
hls_segment_duration_ms = 1000

[reconnect_policy]
max_retries = 8
min_delay_ms = 100
max_delay_ms = 1000
delay_step_ms = 50

[audio_decoder]
decoder_name = "aac"

[video_decoder]
decoder_name = "h264"
backend = "software"
device_index = 1

[processor]
output_width = 1280
output_height = 720

[processor.config]
model = "detector"

[processor.config.runtime]
batch = 4

[audio_encoder]
encoder_name = "aac"

[audio_encoder.properties]
b = "128k"

[video_encoder]
codec = "h265"
encoder_name = "hevc_nvenc"
frame_rate = { num = 25, den = 1 }

[video_encoder.properties]
preset = "p4"

[standby]
enabled = true
image_path = "./standby.png"
)");

  const auto config = LoadStreamingPipelineConfigFromToml(file.path());
  CHECK(config.input_url == "srt://127.0.0.1:9000");
  CHECK(config.cache_duration == 1500ms);
  CHECK(config.audio_queue_capacity == 64);
  CHECK(config.video_queue_capacity == 32);
  CHECK(config.max_track_wait == 250ms);
  REQUIRE(config.input_targets.size() == 2);
  CHECK(config.input_targets[0] == "./input.mp4");
  REQUIRE(config.output_targets.size() == 1);
  CHECK(config.output_targets[0] == "./output.mp4");
  CHECK(config.zlm.player.connect_timeout == 3000ms);
  CHECK(config.zlm.player.media_timeout == 4000ms);
  CHECK(config.zlm.player.local_bind_ip == "127.0.0.2");
  CHECK(config.zlm.output.pusher.connect_timeout == 5000ms);
  CHECK(config.zlm.output.pusher.local_bind_ip == "127.0.0.3");
  CHECK(config.zlm.output.muxer.paced_sender_interval == 5ms);
  CHECK(config.zlm.output.recording.file_buffer_size == 131072);
  CHECK(config.zlm.output.recording.hls_segment_duration == 1000ms);
  CHECK(config.reconnect_policy.max_retries == 8);
  CHECK(config.reconnect_policy.min_delay == 100ms);
  CHECK(config.reconnect_policy.max_delay == 1000ms);
  CHECK(config.reconnect_policy.delay_step == 50ms);
  CHECK(config.audio_decoder.decoder_name == "aac");
  CHECK(config.video_decoder.decoder_name == "h264");
  CHECK(config.video_decoder.backend ==
        mw::streamer::decoder::VideoDecoderBackend::kSoftware);
  CHECK(config.video_decoder.device_index == 1);
  CHECK(config.processor.output_width == 1280);
  CHECK(config.processor.output_height == 720);
  CHECK(config.processor.config.find("model = 'detector'") !=
        std::string::npos);
  CHECK(config.processor.config.find("[runtime]") != std::string::npos);
  CHECK(config.processor.config.find("batch = 4") != std::string::npos);
  CHECK(config.processor.config.find("processor") == std::string::npos);
  CHECK(config.audio_encoder.encoder_name == "aac");
  CHECK(config.audio_encoder.properties.at("b") == "128k");
  CHECK(config.video_encoder.codec == kMwStreamerCodecH265);
  CHECK(config.video_encoder.encoder_name == "hevc_nvenc");
  CHECK(config.video_encoder.frame_rate.num == 25);
  CHECK(config.video_encoder.frame_rate.den == 1);
  CHECK(config.video_encoder.properties.at("preset") == "p4");
  CHECK(config.standby.enabled);
  CHECK(config.standby.image_path == "./standby.png");
}

TEST_CASE("TOML加载Remux与FilePipeline配置") {
  SECTION("Remux") {
    TemporaryToml file(R"(
input_url = "rtsp://127.0.0.1/source"
output_targets = ["./source.mp4"]

[reconnect_policy]
max_retries = 3
)");
    const auto config = LoadRemuxPipelineConfigFromToml(file.path());
    CHECK(config.input_url == "rtsp://127.0.0.1/source");
    REQUIRE(config.output_targets.size() == 1);
    CHECK(config.output_targets[0] == "./source.mp4");
    CHECK(config.reconnect_policy.max_retries == 3);
    CHECK(config.reconnect_policy.min_delay == 2000ms);
  }

  SECTION("File") {
    TemporaryToml file(R"(
input_path = "./input.mp4"

[video_decoder]
backend = "cuda"
device_index = 2

[processor.config]
task = "offline"
)");
    const auto config = LoadFilePipelineConfigFromToml(file.path());
    CHECK(config.input_path == "./input.mp4");
    CHECK(config.video_decoder.backend ==
          mw::streamer::decoder::VideoDecoderBackend::kCuda);
    CHECK(config.video_decoder.device_index == 2);
    CHECK(config.processor.config.find("task = 'offline'") !=
          std::string::npos);
  }
}

TEST_CASE("仓库TOML配置模板与加载器保持一致") {
  const std::filesystem::path template_dir = MW_TOML_TEMPLATE_DIR;

  const auto init = LoadInitConfigFromToml(template_dir / "init.toml");
  CHECK(init.log.console.enabled);
  CHECK(init.zlm.enable_cpu_affinity);

  const auto streaming =
      LoadStreamingPipelineConfigFromToml(template_dir / "streaming.toml");
  CHECK(streaming.input_url == "rtsp://127.0.0.1/live/input");
  CHECK(streaming.video_encoder.frame_rate.num == 25);
  CHECK(streaming.video_encoder.frame_rate.den == 1);
  REQUIRE(streaming.input_targets.size() == 1);
  REQUIRE(streaming.output_targets.size() == 1);

  const auto remux =
      LoadRemuxPipelineConfigFromToml(template_dir / "remux.toml");
  CHECK(remux.input_url == "rtsp://127.0.0.1/live/input");
  REQUIRE(remux.output_targets.size() == 1);

  const auto file = LoadFilePipelineConfigFromToml(template_dir / "file.toml");
  CHECK(file.input_path == "./input.mp4");
  CHECK(file.processor.config.find("mode = 'offline'") != std::string::npos);
}

TEST_CASE("TOML缺失字段保留C++默认值") {
  TemporaryToml empty("");

  const auto init = LoadInitConfigFromToml(empty.path());
  CHECK(init.log.console.enabled);
  CHECK(init.log.modules.streamer == mw::streamer::log::LogLevel::kInfo);
  CHECK(init.zlm.enable_cpu_affinity);

  const auto streaming = LoadStreamingPipelineConfigFromToml(empty.path());
  CHECK(streaming.audio_queue_capacity == 256);
  CHECK(streaming.video_queue_capacity == 128);
  CHECK(streaming.max_track_wait == 500ms);
  CHECK(streaming.video_decoder.backend ==
        mw::streamer::decoder::VideoDecoderBackend::kCuda);
  CHECK(streaming.video_encoder.codec == kMwStreamerCodecH264);

  const auto remux = LoadRemuxPipelineConfigFromToml(empty.path());
  CHECK(remux.reconnect_policy.max_retries == -1);
  CHECK(remux.zlm.player.connect_timeout == 10000ms);

  const auto local_file = LoadFilePipelineConfigFromToml(empty.path());
  CHECK(local_file.video_decoder.backend ==
        mw::streamer::decoder::VideoDecoderBackend::kCuda);
  CHECK(local_file.processor.config.empty());
}

TEST_CASE("TOML严格拒绝未知字段") {
  SECTION("根字段") {
    TemporaryToml file("input_urll = \"typo\"\n");
    CHECK_THROWS_AS(LoadStreamingPipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("已移除的编码开关") {
    TemporaryToml file("encoded_output_enabled = true\n");
    CHECK_THROWS_AS(LoadStreamingPipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("嵌套字段") {
    TemporaryToml file("[zlm.player]\nmedia_timeout = 1000\n");
    CHECK_THROWS_AS(LoadRemuxPipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("Processor配置内容保持开放") {
    TemporaryToml file("[processor.config.arbitrary]\nvalue = 1\n");
    CHECK_NOTHROW(LoadFilePipelineConfigFromToml(file.path()));
  }
}

TEST_CASE("TOML严格拒绝语法类型范围与枚举错误") {
  SECTION("语法") {
    TemporaryToml file("input_path = [\n");
    CHECK_THROWS_AS(LoadFilePipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("类型") {
    TemporaryToml file("input_path = 42\n");
    CHECK_THROWS_AS(LoadFilePipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("无符号范围") {
    TemporaryToml file("audio_queue_capacity = -1\n");
    CHECK_THROWS_AS(LoadStreamingPipelineConfigFromToml(file.path()),
                    std::out_of_range);
  }
  SECTION("有符号范围") {
    TemporaryToml file("[video_decoder]\ndevice_index = 9223372036854775807\n");
    CHECK_THROWS_AS(LoadFilePipelineConfigFromToml(file.path()),
                    std::out_of_range);
  }
  SECTION("枚举") {
    TemporaryToml file("[video_decoder]\nbackend = \"vulkan\"\n");
    CHECK_THROWS_AS(LoadFilePipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("数组元素") {
    TemporaryToml file("output_targets = [\"ok\", 2]\n");
    CHECK_THROWS_AS(LoadRemuxPipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("Encoder属性值") {
    TemporaryToml file("[video_encoder.properties]\npreset = 4\n");
    CHECK_THROWS_AS(LoadStreamingPipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
  SECTION("Processor config必须为表") {
    TemporaryToml file("[processor]\nconfig = \"opaque\"\n");
    CHECK_THROWS_AS(LoadFilePipelineConfigFromToml(file.path()),
                    std::invalid_argument);
  }
}
