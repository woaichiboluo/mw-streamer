#include <catch2/catch_test_macros.hpp>

#include "hls_test_support.h"

TEST_CASE("异步录制器重复初始化发布TS直播HLS并与RTSP共存",
          "[remuxer][async][network][hls]") {
  for (int session = 0; session < 2; ++session) {
    CAPTURE(session);
    const auto directory =
        hls_test::ExerciseMedia<mw::streamer::AsyncRemuxer>();
    hls_test::RequireNoHlsFiles(directory);
  }
}

TEST_CASE("异步HLS发布共享HTTP监听并独立结束会话",
          "[remuxer][async][network][hls]") {
  hls_test::ExerciseLifecycle<mw::streamer::AsyncRemuxer>();
}
