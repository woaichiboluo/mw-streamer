#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <string>
#include <string_view>

extern "C" {
#include <libavutil/error.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace ffmpeg = mw::streamer::ffmpeg;

TEST_CASE("AvErrorStr描述FFmpeg错误") {
  CHECK(ffmpeg::AvErrorStr(AVERROR_EOF) == "End of file");
}

TEST_CASE("FFmpeg成功返回值不会抛出异常") {
  REQUIRE_NOTHROW(ffmpeg::FfmpegException::throwIfError(0, "处理数据"));
  REQUIRE_NOTHROW(ffmpeg::FfmpegException::throwIfError(1, "处理数据"));
}

TEST_CASE("FFmpeg异常保留错误码和操作描述") {
  try {
    ffmpeg::FfmpegException::throwIfError(AVERROR(EINVAL), "打开解码器");
    FAIL("负返回值应抛出FfmpegException");
  } catch (const ffmpeg::FfmpegException& exception) {
    CHECK(exception.error_code() == AVERROR(EINVAL));
    const std::runtime_error& base = exception;
    const std::string message = base.what();
    CHECK(message.find("打开解码器") != std::string::npos);
    CHECK(message.find(ffmpeg::AvErrorStr(AVERROR(EINVAL))) != std::string::npos);
  }
}

TEST_CASE("FFmpeg异常复制string_view的描述内容") {
  std::string operation = "打开解码器后缀";
  const std::string_view view(operation.data(),
                              std::string_view("打开解码器").size());
  ffmpeg::FfmpegException exception(AVERROR(EINVAL), view);
  operation.clear();

  const std::string message = exception.what();
  CHECK(message.find("打开解码器失败: ") == 0);
  CHECK(message.find("后缀") == std::string::npos);
}
