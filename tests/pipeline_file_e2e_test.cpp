#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mw/streamer/pipeline.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/audio_auto_decoder.h"
#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/demuxer.h"
#include "mw/streamer/internal/nv_decoder.h"

namespace mw::streamer {
namespace {

std::filesystem::path TestDataPath(const char* file_name) {
  return std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / file_name;
}

class TemporaryOutputFile final {
 public:
  TemporaryOutputFile() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-pipeline-e2e-" + std::to_string(suffix) + ".mp4");
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  ~TemporaryOutputFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
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
  std::vector<std::string> result;
  while (input) {
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
  }
  return input.eof() ? result : std::vector<std::string>{};
}

struct OutputCodecFixture {
  const char* name;
  VideoCodec codec;
  AVCodecID codec_id;
};

PipelineConfig MakePipelineConfig(const std::filesystem::path& input_path,
                                  const std::filesystem::path& output_path,
                                  VideoCodec codec) {
  PipelineConfig config;
  config.id = "file-e2e";

  InputConfig input;
  input.id = "input";
  input.protocol = InputProtocol::kFile;
  input.url = input_path.string();
  config.inputs.push_back(std::move(input));

  OutputConfig output;
  output.id = "output";
  output.protocol = OutputProtocol::kFile;
  output.url = output_path.string();
  config.outputs.push_back(std::move(output));

  config.video_encoder.codec = codec;
  config.video_encoder.width = 160;
  config.video_encoder.height = 144;
  config.video_encoder.frame_rate = {5, 1};
  config.video_encoder.bit_rate = 500'000;
  config.video_encoder.maximum_bit_rate = 500'000;
  config.video_encoder.vbv_buffer_size = 1'000'000;
  config.video_encoder.gop_size = 5;
  config.video_encoder.maximum_b_frames = 0;
  config.standby_image_path =
      TestDataPath("standby_black_160x144.png").string();
  return config;
}

struct VideoVerification {
  int frame_count = 0;
  bool all_frames_are_limited_black = true;
};

VideoVerification DecodeAndVerifyVideo(const std::filesystem::path& path) {
  internal::Demuxer demuxer;
  demuxer.Open(path.string());
  internal::CudaDeviceContext device;
  device.Create(0);
  internal::NvDecoder decoder;
  decoder.Open(demuxer.video_stream(), device);

  VideoVerification verification;
  const auto receive = [&] {
    int received = 0;
    while (std::optional<ffmpeg::Frame> frame = decoder.Receive()) {
      ffmpeg::Frame downloaded;
      downloaded.get()->format = AV_PIX_FMT_NV12;
      downloaded.get()->width = frame->get()->width;
      downloaded.get()->height = frame->get()->height;
      internal::CheckFfmpeg(av_frame_get_buffer(downloaded.get(), 32),
                            "failed to allocate output verification frame");
      internal::CheckFfmpeg(
          av_hwframe_transfer_data(downloaded.get(), frame->get(), 0),
          "failed to download pipeline output frame");

      const AVFrame* raw = downloaded.get();
      for (int row = 0; row < raw->height; ++row) {
        const std::uint8_t* begin = raw->data[0] + row * raw->linesize[0];
        verification.all_frames_are_limited_black &=
            std::all_of(begin, begin + raw->width,
                        [](std::uint8_t value) { return value == 16; });
      }
      for (int row = 0; row < raw->height / 2; ++row) {
        const std::uint8_t* begin = raw->data[1] + row * raw->linesize[1];
        verification.all_frames_are_limited_black &=
            std::all_of(begin, begin + raw->width,
                        [](std::uint8_t value) { return value == 128; });
      }
      ++verification.frame_count;
      ++received;
    }
    return received;
  };

  while (std::optional<ffmpeg::Packet> packet = demuxer.Read()) {
    if (packet->get()->stream_index != decoder.stream_index()) {
      continue;
    }
    while (!decoder.Send(*packet)) {
      if (receive() == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "output NVDEC made no progress");
      }
    }
    receive();
  }
  while (!decoder.SendEndOfStream()) {
    if (receive() == 0) {
      internal::ThrowError(StatusCode::kInternal,
                           "output NVDEC drain made no progress");
    }
  }
  receive();
  return verification;
}

struct AudioVerification {
  std::size_t sample_count = 0;
  float maximum_absolute_sample = 0.0F;
};

AudioVerification DecodeAndVerifyAudio(const std::filesystem::path& path) {
  internal::Demuxer demuxer;
  demuxer.Open(path.string());
  internal::AudioAutoDecoder decoder;
  decoder.Open(demuxer.audio_stream());
  internal::AudioConverter converter;
  AudioVerification verification;

  const auto observe = [&](internal::NormalizedAudioFrame frame) {
    verification.sample_count += frame.samples.size();
    for (float sample : frame.samples) {
      verification.maximum_absolute_sample =
          std::max(verification.maximum_absolute_sample, std::abs(sample));
    }
  };
  const auto receive = [&] {
    int received = 0;
    while (std::optional<ffmpeg::Frame> frame = decoder.Receive()) {
      if (std::optional<internal::NormalizedAudioFrame> converted =
              converter.Convert(*frame)) {
        observe(std::move(*converted));
      }
      ++received;
    }
    return received;
  };

  while (std::optional<ffmpeg::Packet> packet = demuxer.Read()) {
    if (packet->get()->stream_index != decoder.stream_index()) {
      continue;
    }
    while (!decoder.Send(*packet)) {
      if (receive() == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "output audio decoder made no progress");
      }
    }
    receive();
  }
  while (!decoder.SendEndOfStream()) {
    if (receive() == 0) {
      internal::ThrowError(StatusCode::kInternal,
                           "output audio drain made no progress");
    }
  }
  receive();
  while (std::optional<internal::NormalizedAudioFrame> converted =
             converter.Flush()) {
    observe(std::move(*converted));
  }
  return verification;
}

struct RejectingProcessorState {
  std::atomic<int> start_calls{0};
  std::atomic<int> video_calls{0};
  std::atomic<int> audio_calls{0};
  std::atomic<int> stop_calls{0};
};

struct ProcessorContextObservation {
  std::size_t input_count = 0;
};

struct AudioProcessorObservation {
  std::atomic<int> callback_count{0};
  std::atomic<bool> saw_two_inputs{true};
  std::atomic<bool> timestamps_aligned{true};
};

int MW_PROCESSOR_CALL ObserveProcessorContext(const MwProcessorContext* context,
                                              const char*, char*, std::size_t,
                                              void* user_data) {
  auto* observation = static_cast<ProcessorContextObservation*>(user_data);
  observation->input_count = context->input_count;
  return 0;
}

void MW_PROCESSOR_CALL CopyFirstAudioInput(const MwAudioFrameView* inputs,
                                           std::size_t input_count,
                                           MwAudioFrameView* output,
                                           void* user_data) {
  auto* observation = static_cast<AudioProcessorObservation*>(user_data);
  ++observation->callback_count;
  if (input_count != 2) {
    observation->saw_two_inputs.store(false);
    return;
  }
  if (inputs[0].pts_us != inputs[1].pts_us ||
      inputs[0].frame_count != inputs[1].frame_count) {
    observation->timestamps_aligned.store(false);
  }
  std::copy_n(
      inputs[0].samples,
      output->frame_count * static_cast<std::size_t>(output->channel_count),
      output->samples);
}

constexpr char kProcessorRejectionMessage[] =
    "processor rejected the test configuration";

int MW_PROCESSOR_CALL RejectProcessorStart(const MwProcessorContext*,
                                           const char*, char* error_buffer,
                                           std::size_t error_buffer_capacity,
                                           void* user_data) {
  auto* state = static_cast<RejectingProcessorState*>(user_data);
  ++state->start_calls;
  if (error_buffer != nullptr && error_buffer_capacity > 0) {
    std::snprintf(error_buffer, error_buffer_capacity, "%s",
                  kProcessorRejectionMessage);
  }
  return 1;
}

void MW_PROCESSOR_CALL CountRejectedVideo(const MwGpuNv12FrameView*,
                                          std::size_t, MwGpuNv12FrameView*,
                                          void* user_data) {
  auto* state = static_cast<RejectingProcessorState*>(user_data);
  ++state->video_calls;
}

void MW_PROCESSOR_CALL CountRejectedAudio(const MwAudioFrameView*, std::size_t,
                                          MwAudioFrameView*, void* user_data) {
  auto* state = static_cast<RejectingProcessorState*>(user_data);
  ++state->audio_calls;
}

void MW_PROCESSOR_CALL CountRejectedStop(void* user_data) {
  auto* state = static_cast<RejectingProcessorState*>(user_data);
  ++state->stop_calls;
}

TEST(PipelineLifecycleIntegrationTest,
     ProcessorStartFailureStopsCallbacksAndRemainsTheTerminalError) {
  const std::filesystem::path input_path =
      TestDataPath("h264_aac_mono_44100_1s.mp4");
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));
  TemporaryOutputFile output;
  RejectingProcessorState callback_state;

  PipelineConfig config =
      MakePipelineConfig(input_path, output.path(), VideoCodec::kH264);
  config.processor_callbacks.on_start = RejectProcessorStart;
  config.processor_callbacks.on_video = CountRejectedVideo;
  config.processor_callbacks.on_audio = CountRejectedAudio;
  config.processor_callbacks.on_stop = CountRejectedStop;
  config.processor_callbacks.user_data = &callback_state;

  {
    Pipeline pipeline(std::move(config));
    const Status start_status = pipeline.Start();
    if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
      GTEST_SKIP() << start_status.message();
    }
    ASSERT_FALSE(start_status.ok());
    EXPECT_EQ(start_status.code(), StatusCode::kProcessorStartCallbackFailed);
    EXPECT_EQ(start_status.message(), kProcessorRejectionMessage);
    EXPECT_EQ(pipeline.state(), PipelineState::kFailed);

    const Status wait_status = pipeline.Wait();
    EXPECT_EQ(wait_status.code(), start_status.code());
    EXPECT_EQ(wait_status.message(), start_status.message());
    EXPECT_EQ(pipeline.state(), PipelineState::kFailed);
  }

  EXPECT_EQ(callback_state.start_calls.load(), 1);
  EXPECT_EQ(callback_state.video_calls.load(), 0);
  EXPECT_EQ(callback_state.audio_calls.load(), 0);
  EXPECT_EQ(callback_state.stop_calls.load(), 1);
  EXPECT_FALSE(std::filesystem::exists(output.path()));
}

class PipelineFileE2eTest : public testing::TestWithParam<OutputCodecFixture> {
};

TEST_P(PipelineFileE2eTest,
       CompletesFileInputWithDefaultBlackVideoAndSilentAudio) {
  const std::filesystem::path input_path =
      TestDataPath("h264_aac_mono_44100_1s.mp4");
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));
  ASSERT_TRUE(std::filesystem::is_regular_file(
      TestDataPath("standby_black_160x144.png")));
  TemporaryOutputFile output;

  Pipeline pipeline(
      MakePipelineConfig(input_path, output.path(), GetParam().codec));
  const Status start_status = pipeline.Start();
  if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
    GTEST_SKIP() << start_status.message();
  }
  ASSERT_TRUE(start_status.ok()) << start_status.message();

  const Status terminal_status = pipeline.Wait();
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.message();
  EXPECT_EQ(pipeline.state(), PipelineState::kCompleted);
  ASSERT_TRUE(std::filesystem::is_regular_file(output.path()));
  ASSERT_GT(std::filesystem::file_size(output.path()), 0U);

  internal::Demuxer output_demuxer;
  ASSERT_NO_THROW(output_demuxer.Open(output.path().string()));
  EXPECT_EQ(output_demuxer.video_stream().codec_parameters.get()->codec_id,
            GetParam().codec_id);
  ASSERT_TRUE(output_demuxer.has_audio());
  const AVCodecParameters* audio =
      output_demuxer.audio_stream().codec_parameters.get();
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->codec_id, AV_CODEC_ID_AAC);
  EXPECT_EQ(audio->sample_rate, 48'000);
  EXPECT_EQ(audio->ch_layout.nb_channels, 2);

  const std::vector<std::string> boxes = ReadTopLevelBoxTypes(output.path());
  EXPECT_NE(std::find(boxes.begin(), boxes.end(), "moof"), boxes.end());
  EXPECT_NE(std::find(boxes.begin(), boxes.end(), "mdat"), boxes.end());

  VideoVerification video;
  ASSERT_NO_THROW(video = DecodeAndVerifyVideo(output.path()));
  EXPECT_EQ(video.frame_count, 5);
  EXPECT_TRUE(video.all_frames_are_limited_black);

  AudioVerification audio_samples;
  ASSERT_NO_THROW(audio_samples = DecodeAndVerifyAudio(output.path()));
  EXPECT_GT(audio_samples.sample_count, 0U);
  EXPECT_LT(audio_samples.maximum_absolute_sample, 1.0e-3F);
}

TEST(PipelineHlsVodE2eTest, CompletesAtEndListWithoutReconnecting) {
  const std::filesystem::path input_path =
      TestDataPath("h264_aac_mono_44100_1s.m3u8");
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));
  TemporaryOutputFile output;
  PipelineConfig config =
      MakePipelineConfig(input_path, output.path(), VideoCodec::kH264);
  config.inputs.front().protocol = InputProtocol::kHls;
  config.inputs.front().hls_mode = HlsMode::kVod;

  Pipeline pipeline(std::move(config));
  const Status start_status = pipeline.Start();
  if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
    GTEST_SKIP() << start_status.message();
  }
  ASSERT_TRUE(start_status.ok()) << start_status.message();

  const Status terminal_status = pipeline.Wait();
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.message();
  EXPECT_EQ(pipeline.state(), PipelineState::kCompleted);

  VideoVerification video;
  ASSERT_NO_THROW(video = DecodeAndVerifyVideo(output.path()));
  EXPECT_EQ(video.frame_count, 5);
  AudioVerification audio;
  ASSERT_NO_THROW(audio = DecodeAndVerifyAudio(output.path()));
  EXPECT_GT(audio.sample_count, 0U);
}

void VerifyFiniteInputDropsAudioPastVideoEnd(InputProtocol protocol,
                                             HlsMode hls_mode,
                                             const char* input_name) {
  const std::filesystem::path input_path = TestDataPath(input_name);
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));
  TemporaryOutputFile output;
  PipelineConfig config =
      MakePipelineConfig(input_path, output.path(), VideoCodec::kH264);
  config.inputs.front().protocol = protocol;
  config.inputs.front().hls_mode = hls_mode;
  config.queues.audio_fifo_capacity_frames = 4'096;

  Pipeline pipeline(std::move(config));
  const Status start_status = pipeline.Start();
  if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
    GTEST_SKIP() << start_status.message();
  }
  ASSERT_TRUE(start_status.ok()) << start_status.message();

  const Status terminal_status = pipeline.Wait();
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.message();
  EXPECT_EQ(pipeline.state(), PipelineState::kCompleted);

  VideoVerification video;
  ASSERT_NO_THROW(video = DecodeAndVerifyVideo(output.path()));
  EXPECT_EQ(video.frame_count, 5);
  AudioVerification audio;
  ASSERT_NO_THROW(audio = DecodeAndVerifyAudio(output.path()));
  EXPECT_GT(audio.sample_count, 0U);
  EXPECT_LE(audio.sample_count, 100'000U);
}

TEST(PipelineFiniteAudioTailE2eTest, FileDropsAudioPastVideoEndAndCompletes) {
  VerifyFiniteInputDropsAudioPastVideoEnd(InputProtocol::kFile, HlsMode::kLive,
                                          "h264_1s_aac_3s.mp4");
}

TEST(PipelineFiniteAudioTailE2eTest, HlsVodDropsAudioPastVideoEndAndCompletes) {
  VerifyFiniteInputDropsAudioPastVideoEnd(InputProtocol::kHls, HlsMode::kVod,
                                          "h264_1s_aac_3s.m3u8");
}

TEST(PipelineDualFileE2eTest, SynchronizesBothInputsBeforeEncoding) {
  const std::filesystem::path input_path = TestDataPath("h264_160x144.mp4");
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));
  TemporaryOutputFile output;
  ProcessorContextObservation observation;

  PipelineConfig config =
      MakePipelineConfig(input_path, output.path(), VideoCodec::kH264);
  config.inputs.front().id = "left";
  InputConfig right = config.inputs.front();
  right.id = "right";
  config.inputs.push_back(std::move(right));
  config.queues.decoded_video_capacity = 2;
  config.processor_callbacks.on_start = ObserveProcessorContext;
  config.processor_callbacks.user_data = &observation;

  Pipeline pipeline(std::move(config));
  const Status start_status = pipeline.Start();
  if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
    GTEST_SKIP() << start_status.message();
  }
  ASSERT_TRUE(start_status.ok()) << start_status.message();
  const Status terminal_status = pipeline.Wait();
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.message();
  EXPECT_EQ(pipeline.state(), PipelineState::kCompleted);
  EXPECT_EQ(observation.input_count, 2U);

  VideoVerification video;
  ASSERT_NO_THROW(video = DecodeAndVerifyVideo(output.path()));
  EXPECT_EQ(video.frame_count, 5);
  EXPECT_TRUE(video.all_frames_are_limited_black);

  internal::Demuxer output_demuxer;
  ASSERT_NO_THROW(output_demuxer.Open(output.path().string()));
  EXPECT_FALSE(output_demuxer.has_audio());
}

TEST(PipelineDualFileE2eTest, AlignsBothAudioInputsIntoFixedSlots) {
  const std::filesystem::path input_path =
      TestDataPath("h264_aac_mono_44100_1s.mp4");
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));
  TemporaryOutputFile output;
  AudioProcessorObservation observation;

  PipelineConfig config =
      MakePipelineConfig(input_path, output.path(), VideoCodec::kH264);
  config.inputs.front().id = "left";
  InputConfig right = config.inputs.front();
  right.id = "right";
  config.inputs.push_back(std::move(right));
  config.processor_callbacks.on_audio = CopyFirstAudioInput;
  config.processor_callbacks.user_data = &observation;

  Pipeline pipeline(std::move(config));
  const Status start_status = pipeline.Start();
  if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
    GTEST_SKIP() << start_status.message();
  }
  ASSERT_TRUE(start_status.ok()) << start_status.message();
  const Status terminal_status = pipeline.Wait();
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.message();
  EXPECT_GT(observation.callback_count.load(), 0);
  EXPECT_TRUE(observation.saw_two_inputs.load());
  EXPECT_TRUE(observation.timestamps_aligned.load());

  AudioVerification audio;
  ASSERT_NO_THROW(audio = DecodeAndVerifyAudio(output.path()));
  EXPECT_GT(audio.sample_count, 0U);
  EXPECT_GT(audio.maximum_absolute_sample, 1.0e-3F);
}

TEST(PipelineMultiOutputE2eTest, FansOutOneEncodeToTwoRecordings) {
  const std::filesystem::path input_path =
      TestDataPath("h264_aac_mono_44100_1s.mp4");
  TemporaryOutputFile first_output;
  TemporaryOutputFile second_output;
  PipelineConfig config =
      MakePipelineConfig(input_path, first_output.path(), VideoCodec::kH264);
  OutputConfig second = config.outputs.front();
  second.id = "second-output";
  second.url = second_output.path().string();
  config.outputs.push_back(std::move(second));

  Pipeline pipeline(std::move(config));
  const Status start_status = pipeline.Start();
  if (!start_status.ok() && start_status.code() == StatusCode::kUnavailable) {
    GTEST_SKIP() << start_status.message();
  }
  ASSERT_TRUE(start_status.ok()) << start_status.message();
  const Status terminal_status = pipeline.Wait();
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.message();

  for (const std::filesystem::path& path :
       {first_output.path(), second_output.path()}) {
    VideoVerification video;
    ASSERT_NO_THROW(video = DecodeAndVerifyVideo(path));
    EXPECT_EQ(video.frame_count, 5);
    internal::Demuxer demuxer;
    ASSERT_NO_THROW(demuxer.Open(path.string()));
    EXPECT_TRUE(demuxer.has_audio());
  }
}

INSTANTIATE_TEST_SUITE_P(
    H264AndH265, PipelineFileE2eTest,
    testing::Values(
        OutputCodecFixture{"H264", VideoCodec::kH264, AV_CODEC_ID_H264},
        OutputCodecFixture{"H265", VideoCodec::kH265, AV_CODEC_ID_HEVC}),
    [](const testing::TestParamInfo<OutputCodecFixture>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace mw::streamer
