#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "mw/config/toml.h"
#include "mw/log/logging.h"

namespace {

using mw::streamer::config::LoadInitConfigFromToml;

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

  std::string Read() const {
    std::ifstream input(path_, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  }

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

TEST_CASE("Init模板与默认值保持一致") {
  const auto config = LoadInitConfigFromToml(
      std::filesystem::path(MW_TOML_TEMPLATE_DIR) / "init.toml");
  CHECK(config.log.console.enabled);
  CHECK(config.zlm.enable_cpu_affinity);
  TemporaryToml empty("");
  const auto defaults = LoadInitConfigFromToml(empty.path());
  CHECK(defaults.log.modules.streamer == mw::streamer::log::LogLevel::kInfo);
  CHECK(defaults.log.console.enabled);
  CHECK(defaults.zlm.enable_cpu_affinity);
}

TEST_CASE("Init配置拒绝语法类型范围和枚举错误") {
  for (const auto text : {
           "[broken",
           "[zlm]\nevent_poller_threads = 'two'",
           "[log.console]\nenabled = 1",
           "[log.console]\nlevel = 'invalid'",
           "[log.async]\nqueue_size = -1",
       }) {
    CAPTURE(text);
    TemporaryToml file(text);
    CHECK_THROWS(LoadInitConfigFromToml(file.path()));
  }
}

TEST_CASE("Init配置忽略未知字段并记录警告") {
  TemporaryToml config_file(R"(
unknown = 1

[zlm]
event_poller_threads = 4
unused = true
)");
  TemporaryToml log_file("");
  mw::streamer::log::LogConfig log_config;
  log_config.console.enabled = false;
  log_config.rotating_file.enabled = true;
  log_config.rotating_file.path = log_file.path().string();
  log_config.rotating_file.max_files = 1;

  {
    mw::streamer::log::Logging logging(log_config);
    const auto config = LoadInitConfigFromToml(config_file.path());
    CHECK(config.zlm.event_poller_threads == 4);
  }

  const auto warnings = log_file.Read();
  CHECK(warnings.find("[streamer] 忽略未知TOML配置项: unknown") !=
        std::string::npos);
  CHECK(warnings.find("[streamer] 忽略未知TOML配置项: zlm.unused") !=
        std::string::npos);
}
