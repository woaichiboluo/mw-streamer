#include "mw/streamer/internal/common/ffmpeg_log.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "spdlog/common.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/spdlog.h"

extern "C" {
#include <libavutil/log.h>
}

namespace mw::streamer::internal {
namespace {

struct CapturedLogMessage {
  spdlog::level::level_enum level;
  std::string text;
};

class CapturingSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  [[nodiscard]] const std::vector<CapturedLogMessage>& messages() const {
    return messages_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    messages_.push_back({message.level, std::string(message.payload.data(),
                                                    message.payload.size())});
  }

  void flush_() override {}

 private:
  std::vector<CapturedLogMessage> messages_;
};

class ScopedLogConfiguration final {
 public:
  explicit ScopedLogConfiguration(std::shared_ptr<spdlog::logger> logger)
      : previous_logger_(spdlog::default_logger()),
        previous_ffmpeg_level_(av_log_get_level()) {
    spdlog::set_default_logger(std::move(logger));
    av_log_set_level(AV_LOG_TRACE);
  }

  ~ScopedLogConfiguration() {
    av_log_set_level(previous_ffmpeg_level_);
    spdlog::set_default_logger(std::move(previous_logger_));
  }

  ScopedLogConfiguration(const ScopedLogConfiguration&) = delete;
  ScopedLogConfiguration& operator=(const ScopedLogConfiguration&) = delete;

 private:
  std::shared_ptr<spdlog::logger> previous_logger_;
  int previous_ffmpeg_level_;
};

TEST(FfmpegLogTest, MapsFfmpegLevelsToSpdlogLevels) {
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_QUIET), spdlog::level::off);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_PANIC), spdlog::level::critical);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_FATAL), spdlog::level::critical);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_ERROR), spdlog::level::err);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_WARNING), spdlog::level::warn);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_INFO), spdlog::level::info);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_VERBOSE), spdlog::level::debug);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_DEBUG), spdlog::level::debug);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_TRACE), spdlog::level::trace);
  EXPECT_EQ(MapFfmpegLogLevel(AV_LOG_ERROR | AV_LOG_C(42)), spdlog::level::err);
}

TEST(FfmpegLogTest, InstallationIsIdempotent) {
  EXPECT_NO_THROW(InstallFfmpegLogBridge());
  EXPECT_NO_THROW(InstallFfmpegLogBridge());
}

TEST(FfmpegLogTest, BridgeForwardsUrlTextWithoutModification) {
  const auto sink = std::make_shared<CapturingSink>();
  const auto logger = std::make_shared<spdlog::logger>("ffmpeg-url-test", sink);
  logger->set_level(spdlog::level::trace);
  ScopedLogConfiguration log_configuration(logger);
  InstallFfmpegLogBridge();

  av_log(nullptr, AV_LOG_INFO, "%s",
         "connect srt://publisher:secret@host:9000?streamid=publish:path\n");

  ASSERT_EQ(sink->messages().size(), 1U);
  EXPECT_EQ(sink->messages()[0].text,
            "ffmpeg: connect srt://publisher:secret@host:9000?"
            "streamid=publish:path");
}

TEST(FfmpegLogTest, JoinsFragmentsSplitsLinesAndFoldsRepeats) {
  const auto sink = std::make_shared<CapturingSink>();
  const auto logger = std::make_shared<spdlog::logger>("ffmpeg-log-test", sink);
  logger->set_level(spdlog::level::trace);
  ScopedLogConfiguration log_configuration(logger);
  InstallFfmpegLogBridge();

  av_log(nullptr, AV_LOG_INFO, "%s", "fragment-");
  av_log(nullptr, AV_LOG_INFO, "%s", "joined\n");
  av_log(nullptr, AV_LOG_WARNING, "%s", "first\nsecond\n");
  av_log(nullptr, AV_LOG_ERROR, "%s", "repeated\n");
  av_log(nullptr, AV_LOG_ERROR, "%s", "repeated\n");
  av_log(nullptr, AV_LOG_ERROR, "%s", "repeated\n");
  av_log(nullptr, AV_LOG_INFO, "%s", "after-repeat\n");

  ASSERT_EQ(sink->messages().size(), 6U);
  EXPECT_EQ(sink->messages()[0].level, spdlog::level::info);
  EXPECT_EQ(sink->messages()[0].text, "ffmpeg: fragment-joined");
  EXPECT_EQ(sink->messages()[1].level, spdlog::level::warn);
  EXPECT_EQ(sink->messages()[1].text, "ffmpeg: first");
  EXPECT_EQ(sink->messages()[2].level, spdlog::level::warn);
  EXPECT_EQ(sink->messages()[2].text, "ffmpeg: second");
  EXPECT_EQ(sink->messages()[3].level, spdlog::level::err);
  EXPECT_EQ(sink->messages()[3].text, "ffmpeg: repeated");
  EXPECT_EQ(sink->messages()[4].level, spdlog::level::err);
  EXPECT_EQ(sink->messages()[4].text, "ffmpeg: Last message repeated 2 times");
  EXPECT_EQ(sink->messages()[5].level, spdlog::level::info);
  EXPECT_EQ(sink->messages()[5].text, "ffmpeg: after-repeat");
}

}  // namespace
}  // namespace mw::streamer::internal
