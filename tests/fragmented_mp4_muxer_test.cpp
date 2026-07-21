#include "mw/streamer/internal/fragmented_mp4_muxer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/codec_parameters.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

TEST(FragmentedMp4MuxerTest, DoesNotOverwriteExistingFile) {
  const auto unique_suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      ("mw-streamer-existing-" + std::to_string(unique_suffix) + ".mp4");
  constexpr char kOriginalContents[] = "must not be overwritten";
  {
    std::ofstream output(output_path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output << kOriginalContents;
  }

  ffmpeg::CodecParameters video_parameters;
  video_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  video_parameters.get()->codec_id = AV_CODEC_ID_H264;
  video_parameters.get()->format = AV_PIX_FMT_YUV420P;
  video_parameters.get()->width = 160;
  video_parameters.get()->height = 144;

  FragmentedMp4Muxer muxer;
  try {
    muxer.Open(output_path, video_parameters, AVRational{1, 25});
    FAIL() << "expected an existing-file error";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kAlreadyExists);
  }

  std::ifstream input(output_path, std::ios::binary);
  const std::string actual_contents((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
  EXPECT_EQ(actual_contents, kOriginalContents);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

}  // namespace
}  // namespace mw::streamer::internal
