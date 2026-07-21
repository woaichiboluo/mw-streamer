#include "mw/streamer/internal/fragmented_mp4_muxer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/demuxer.h"

namespace mw::streamer::internal {
namespace {

struct MuxFixture {
  const char* name;
  const char* file_name;
  AVCodecID codec_id;
};

std::uint32_t ReadBigEndian32(const std::array<char, 8>& header) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]))
          << 24U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]))
          << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]))
          << 8U) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
}

std::vector<std::string> ReadTopLevelBoxTypes(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  std::vector<std::string> result;
  while (true) {
    std::array<char, 8> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (input.gcount() == 0) {
      break;
    }
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
      return {};
    }

    const std::uint32_t box_size = ReadBigEndian32(header);
    if (box_size < header.size()) {
      return {};
    }
    result.emplace_back(header.data() + 4, 4);
    input.seekg(static_cast<std::streamoff>(box_size - header.size()),
                std::ios::cur);
    if (!input) {
      return {};
    }
  }
  return result;
}

std::filesystem::path MakeOutputPath(std::string_view name) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("mw-streamer-remux-" + std::string(name) + "-" +
          std::to_string(suffix) + ".mp4");
}

class FragmentedMp4MuxerIntegrationTest
    : public testing::TestWithParam<MuxFixture> {};

TEST_P(FragmentedMp4MuxerIntegrationTest,
       RemuxesPacketsIntoReadableFragmentedMp4WithoutGpu) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / GetParam().file_name;
  const std::filesystem::path output_path = MakeOutputPath(GetParam().name);

  Demuxer input;
  ASSERT_NO_THROW(input.Open(input_path.string()));
  FragmentedMp4Muxer muxer;
  ASSERT_NO_THROW(muxer.Open(output_path, input.video_stream().codec_parameters,
                             input.video_stream().time_base));

  int input_packet_count = 0;
  while (auto packet = input.Read()) {
    ASSERT_EQ(packet->get()->stream_index, input.video_stream().index);
    muxer.WriteVideo(std::move(*packet));
    ++input_packet_count;
  }
  ASSERT_NO_THROW(muxer.Finish());
  ASSERT_GT(input_packet_count, 0);

  Demuxer output;
  ASSERT_NO_THROW(output.Open(output_path.string()));
  EXPECT_EQ(output.video_stream().codec_parameters.get()->codec_id,
            GetParam().codec_id);
  int output_packet_count = 0;
  while (output.Read().has_value()) {
    ++output_packet_count;
  }
  EXPECT_EQ(output_packet_count, input_packet_count);

  const std::vector<std::string> boxes = ReadTopLevelBoxTypes(output_path);
  ASSERT_GE(boxes.size(), 4U);
  EXPECT_EQ(boxes[0], "ftyp");
  EXPECT_EQ(boxes[1], "moov");
  const auto moof_count =
      std::count(boxes.begin(), boxes.end(), std::string("moof"));
  const auto mdat_count =
      std::count(boxes.begin(), boxes.end(), std::string("mdat"));
  EXPECT_GT(moof_count, 0);
  EXPECT_EQ(moof_count, mdat_count);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

TEST(FragmentedMp4MuxerIntegrationTest, RejectsDuplicateVideoDts) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / "h264_160x144.mp4";
  const std::filesystem::path output_path = MakeOutputPath("duplicate-dts");

  {
    Demuxer input;
    input.Open(input_path.string());
    auto first_packet = input.Read();
    ASSERT_TRUE(first_packet.has_value());
    ffmpeg::Packet duplicate = first_packet->Ref();

    FragmentedMp4Muxer muxer;
    muxer.Open(output_path, input.video_stream().codec_parameters,
               input.video_stream().time_base);
    muxer.WriteVideo(std::move(*first_packet));
    EXPECT_THROW(muxer.WriteVideo(std::move(duplicate)), Error);
  }

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

INSTANTIATE_TEST_SUITE_P(
    H264AndH265, FragmentedMp4MuxerIntegrationTest,
    testing::Values(MuxFixture{"H264", "h264_160x144.mp4", AV_CODEC_ID_H264},
                    MuxFixture{"H265", "h265_160x144.mp4", AV_CODEC_ID_HEVC}),
    [](const testing::TestParamInfo<MuxFixture>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace mw::streamer::internal
