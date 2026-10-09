#include <catch2/catch_test_macros.hpp>

#include "hls_test_support.h"

TEST_CASE("原包录制器重复初始化发布TS直播HLS并与RTSP共存",
          "[remuxer][sync][network][hls]") {
  for (int session = 0; session < 2; ++session) {
    CAPTURE(session);
    const auto directory = hls_test::ExerciseMedia<mw::streamer::SyncRemuxer>();
    hls_test::RequireNoHlsFiles(directory);
  }
}

TEST_CASE("原包HLS发布共享HTTP监听并独立结束会话",
          "[remuxer][sync][network][hls]") {
  hls_test::ExerciseLifecycle<mw::streamer::SyncRemuxer>();
}
