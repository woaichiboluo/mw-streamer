#include "mw/streamer/internal/common/error.h"

#include <cerrno>
#include <string>
#include <system_error>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/error.h>
}

namespace mw::streamer::internal {
namespace {

TEST(ErrorTest, BuildsPlainError) {
  try {
    ThrowError(StatusCode::kInternal, "plain failure");
    FAIL() << "ThrowError returned";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kInternal);
    EXPECT_STREQ(error.what(), "plain failure");
  }
}

TEST(ErrorTest, AppendsFfmpegErrorText) {
  try {
    ThrowError(StatusCode::kFfmpegError, "packet read failed", AVERROR(EINVAL));
    FAIL() << "ThrowError returned";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kFfmpegError);
    EXPECT_NE(std::string(error.what()).find("packet read failed"),
              std::string::npos);
    EXPECT_GT(std::string(error.what()).size(),
              std::string("packet read failed").size());
  }
}

TEST(ErrorTest, AppendsStdErrorText) {
  const std::error_code error = std::make_error_code(std::errc::timed_out);
  try {
    ThrowError(StatusCode::kUnavailable, "connection failed", error);
    FAIL() << "ThrowError returned";
  } catch (const Error& exception) {
    EXPECT_EQ(exception.code(), StatusCode::kUnavailable);
    EXPECT_NE(std::string(exception.what()).find("connection failed"),
              std::string::npos);
    EXPECT_NE(std::string(exception.what()).find(error.message()),
              std::string::npos);
  }
}

}  // namespace
}  // namespace mw::streamer::internal
