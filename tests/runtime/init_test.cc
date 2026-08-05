#include "mw/init/init.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/log.h>
}

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "Util/logger.h"

namespace {

using StreamerLog =
    mw::streamer::log::Module<mw::streamer::log::LogModule::kStreamer>;

class TemporaryLogFile {
 public:
  TemporaryLogFile() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-init-test-" + std::to_string(suffix) + ".log");
  }

  ~TemporaryLogFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

  std::string Read() const {
    std::ifstream stream(path_);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
  }

 private:
  std::filesystem::path path_;
};

mw::streamer::InitConfig MakeFileInitConfig(const std::filesystem::path& path) {
  mw::streamer::InitConfig config;
  config.log.console.enabled = false;
  config.log.rotating_file.enabled = true;
  config.log.rotating_file.path = path.string();
  config.log.rotating_file.level = mw::streamer::log::LogLevel::kTrace;
  config.log.rotating_file.max_file_size = 1024 * 1024;
  config.log.rotating_file.max_files = 1;
  return config;
}

}  // namespace

TEST_CASE("init has a one-time process lifecycle",
          "[init][logging][lifecycle]") {
  CHECK_FALSE(mw::streamer::IsInitialized());
  CHECK(mw::streamer::log::detail::ShouldLog(
      mw::streamer::log::LogModule::kStreamer,
      mw::streamer::log::LogLevel::kInfo));
  CHECK_FALSE(mw::streamer::log::detail::ShouldLog(
      mw::streamer::log::LogModule::kStreamer,
      mw::streamer::log::LogLevel::kDebug));
  StreamerLog::Info("default logger is available");

  mw::streamer::InitConfig invalid_config;
  invalid_config.log.console.enabled = false;
  invalid_config.zlm.event_poller_threads = 1;
  invalid_config.zlm.work_threads = 1;
  CHECK_THROWS_AS(mw::streamer::Init(invalid_config), std::invalid_argument);
  CHECK_FALSE(mw::streamer::IsInitialized());

  TemporaryLogFile file;
  auto config = MakeFileInitConfig(file.path());
  config.log.modules.streamer = mw::streamer::log::LogLevel::kDebug;
  config.log.modules.zlm = mw::streamer::log::LogLevel::kInfo;
  config.log.modules.ffmpeg = mw::streamer::log::LogLevel::kInfo;
  config.log.async.enabled = true;
  config.log.async.queue_size = 128;
  config.log.async.overflow = mw::streamer::log::OverflowPolicy::kBlock;
  config.zlm.event_poller_threads = 2;
  config.zlm.work_threads = 3;
  config.zlm.enable_cpu_affinity = false;

  mw::streamer::Init(config);
  CHECK(mw::streamer::IsInitialized());
  CHECK(toolkit::EventPollerPool::Instance().getExecutorSize() == 2);
  CHECK(toolkit::WorkThreadPool::Instance().getExecutorSize() == 3);

  // The first successful configuration wins. Later calls do not validate or
  // replace it.
  CHECK_NOTHROW(mw::streamer::Init(invalid_config));
  CHECK(toolkit::EventPollerPool::Instance().getExecutorSize() == 2);
  CHECK(toolkit::WorkThreadPool::Instance().getExecutorSize() == 3);

  StreamerLog::Info("configured async message");
  InfoL << "zlm init bridge message";
  av_log(nullptr, AV_LOG_INFO, "ffmpeg init bridge message\n");
  std::atomic<bool> started{false};
  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    started = true;
    while (!stop.load()) {
      StreamerLog::Debug("concurrent shutdown message");
      std::this_thread::yield();
    }
  });

  while (!started.load()) {
    std::this_thread::yield();
  }
  mw::streamer::Shutdown();
  stop = true;
  writer.join();

  CHECK_FALSE(mw::streamer::IsInitialized());
  CHECK_NOTHROW(mw::streamer::Shutdown());
  CHECK_THROWS_AS(mw::streamer::Init(config), std::logic_error);
  CHECK(mw::streamer::log::detail::ShouldLog(
      mw::streamer::log::LogModule::kStreamer,
      mw::streamer::log::LogLevel::kInfo));

  const auto content = file.Read();
  CHECK(content.find("[streamer] configured async message") !=
        std::string::npos);
  CHECK(content.find("[ZLM] zlm init bridge message") != std::string::npos);
  CHECK(content.find("[FFMPEG] ffmpeg init bridge message") !=
        std::string::npos);
}
