#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

extern "C" {
#include <libavformat/avformat.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/decoder/video_decoder.h"
#include "mw/ffmpeg/codec_parameters.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/opencv_adapter/cuda_mat_adapter.h"
#include "mw/opencv_adapter/host_mat_adapter.h"
#include "mw/pipeline/streaming_pipeline.h"
#include "mw/processor/internal/frame_adapter.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::VideoDecoder;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::decoder::VideoDecoderConfig;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::InputFormatContext;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::opencv_adapter::CudaMatAdapter;
using mw::streamer::opencv_adapter::HostMatAdapter;
using mw::streamer::pipeline::StreamingPipeline;
using mw::streamer::pipeline::StreamingPipelineConfig;
using mw::streamer::pipeline::StreamingPipelineStatus;
using mw::streamer::processor::internal::VideoFrameAdapter;

constexpr std::uint32_t kWidth = 160;
constexpr std::uint32_t kHeight = 90;
constexpr std::uint64_t kFramesAfterRunning = 10;

enum class MatBackend {
  kHost,
  kCuda,
};

struct PipelineCase {
  const char* name;
  VideoDecoderBackend decoder;
  MatBackend mat;
  const char* encoder_name;
  MwStreamerMemoryType expected_memory;
  MwStreamerExecutionType expected_execution;
};

constexpr std::array<PipelineCase, 5> kPipelineCases = {{
    {"软件解码到Mat再由软件编码", VideoDecoderBackend::kSoftware,
     MatBackend::kHost, "libx264", kMwStreamerMemoryHost,
     kMwStreamerExecutionCpu},
    {"软件解码到Mat再由硬件编码", VideoDecoderBackend::kSoftware,
     MatBackend::kHost, "h264_nvenc", kMwStreamerMemoryHost,
     kMwStreamerExecutionCpu},
    {"硬件解码到Mat再由硬件编码", VideoDecoderBackend::kCuda, MatBackend::kHost,
     "h264_nvenc", kMwStreamerMemoryCuda, kMwStreamerExecutionCuda},
    {"软件解码到GpuMat再由软件编码", VideoDecoderBackend::kSoftware,
     MatBackend::kCuda, "libx264", kMwStreamerMemoryHost,
     kMwStreamerExecutionCpu},
    {"硬件解码到GpuMat再由硬件编码", VideoDecoderBackend::kCuda,
     MatBackend::kCuda, "h264_nvenc", kMwStreamerMemoryCuda,
     kMwStreamerExecutionCuda},
}};

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-opencv-adapter-pipeline-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

struct ProcessorState {
  const PipelineCase* test_case = nullptr;
  std::atomic_uint64_t processed_frames{0};
  std::condition_variable condition;
  std::mutex mutex;
};

std::filesystem::path SamplePath() {
  return std::filesystem::path(MW_OPENCV_ADAPTER_TEST_DATA_DIR) /
         "packet_queue_8s.mp4";
}

std::filesystem::path FindRecordedMp4(const std::filesystem::path& directory) {
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
      return entry.path();
    }
  }
  return {};
}

void NormalizeColorInfo(MwStreamerVideoColorInfo* color) {
  if (color->range == kMwStreamerColorRangeUnknown) {
    color->range = kMwStreamerColorRangeLimited;
  }
  if (color->space == kMwStreamerColorSpaceUnknown) {
    color->space = kMwStreamerColorSpaceBt709;
  }
  if (color->primaries == kMwStreamerColorPrimariesUnknown) {
    color->primaries = kMwStreamerColorPrimariesBt709;
  }
  if (color->transfer == kMwStreamerColorTransferUnknown) {
    color->transfer = kMwStreamerColorTransferBt709;
  }
  if (color->chroma_location == kMwStreamerChromaLocationUnknown) {
    color->chroma_location = kMwStreamerChromaLocationLeft;
  }
}

MwStreamerVideoFrameView MakeInputPrototype(
    const MwStreamerStreamingVideoProcessRequest& request) {
  auto prototype = *request.input;
  NormalizeColorInfo(&prototype.color);
  return prototype;
}

MwStreamerVideoFrameView MakeOutputPrototype(
    const MwStreamerStreamingVideoProcessRequest& request,
    const MwStreamerVideoColorInfo& color) {
  MwStreamerVideoFrameView prototype{};
  prototype.buffer = *request.output;
  prototype.color = color;
  prototype.timestamp = request.input->timestamp;
  return prototype;
}

cv::Rect OsdBackground(int rows, int columns) {
  constexpr int kWidth = 46;
  constexpr int kHeight = 28;
  if (columns < kWidth + 6 || rows < kHeight + 6) {
    throw std::invalid_argument("视频尺寸不足以叠加测试OSD");
  }
  return {columns - kWidth - 6, 6, kWidth, kHeight};
}

std::array<cv::Rect, 3> OsdBars(const cv::Rect& background) {
  return {{{background.x + 3, background.y + 3, 10, 22},
           {background.x + 18, background.y + 3, 10, 22},
           {background.x + 33, background.y + 3, 10, 22}}};
}

void DrawOsd(cv::Mat* image) {
  if (!image || image->type() != CV_8UC3) {
    throw std::invalid_argument("测试OSD要求CV_8UC3 Mat");
  }
  const auto background = OsdBackground(image->rows, image->cols);
  const auto bars = OsdBars(background);
  (*image)(background).setTo(cv::Scalar(0, 0, 0));
  (*image)(bars[0]).setTo(cv::Scalar(255, 0, 0));
  (*image)(bars[1]).setTo(cv::Scalar(0, 255, 0));
  (*image)(bars[2]).setTo(cv::Scalar(0, 0, 255));
}

void DrawOsd(cv::cuda::GpuMat* image) {
  if (!image || image->type() != CV_8UC3) {
    throw std::invalid_argument("测试OSD要求CV_8UC3 GpuMat");
  }
  const auto background = OsdBackground(image->rows, image->cols);
  const auto bars = OsdBars(background);
  (*image)(background).setTo(cv::Scalar(0, 0, 0));
  (*image)(bars[0]).setTo(cv::Scalar(255, 0, 0));
  (*image)(bars[1]).setTo(cv::Scalar(0, 255, 0));
  (*image)(bars[2]).setTo(cv::Scalar(0, 0, 255));
}

bool HasOsd(const cv::Mat& image) {
  if (image.type() != CV_8UC3) {
    return false;
  }
  const auto bars = OsdBars(OsdBackground(image.rows, image.cols));
  for (int index = 0; index < 3; ++index) {
    const cv::Rect sample = {bars[index].x + 2, bars[index].y + 3,
                             bars[index].width - 4, bars[index].height - 6};
    const auto mean = cv::mean(image(sample));
    const double expected = mean[index];
    const double other_a = mean[(index + 1) % 3];
    const double other_b = mean[(index + 2) % 3];
    if (expected < 150.0 || expected < other_a + 50.0 ||
        expected < other_b + 50.0) {
      return false;
    }
  }
  return true;
}

MwStreamerProcessorStartResult OnProcessorStart(
    const MwStreamerStreamingProcessorStartRequest* request,
    void* user_context) {
  const auto* state = static_cast<const ProcessorState*>(user_context);
  if (!request || !request->execution || !state || !state->test_case ||
      request->execution->type != state->test_case->expected_execution) {
    return kMwStreamerProcessorStartFailed;
  }
  return kMwStreamerProcessorStartSuccess;
}

void ProcessVideo(const MwStreamerStreamingVideoProcessRequest* request,
                  void* user_context) {
  auto* state = static_cast<ProcessorState*>(user_context);
  if (!request || !request->input || !request->output || !state ||
      !state->test_case ||
      request->input->buffer.memory_type != state->test_case->expected_memory ||
      request->output->memory_type != state->test_case->expected_memory) {
    throw std::invalid_argument("Pipeline Adapter测试收到错误的视频内存类型");
  }

  const auto input_prototype = MakeInputPrototype(*request);
  const auto output_prototype =
      MakeOutputPrototype(*request, input_prototype.color);
  if (state->test_case->mat == MatBackend::kHost) {
    auto bgr = HostMatAdapter::ToBgr(input_prototype);
    DrawOsd(&bgr);
    HostMatAdapter::FromBgr(bgr, output_prototype).CopyTo(*request->output);
  } else {
    auto bgr = CudaMatAdapter::ToBgr(input_prototype);
    DrawOsd(&bgr);
    CudaMatAdapter::FromBgr(bgr, output_prototype).CopyTo(*request->output);
  }

  state->processed_frames.fetch_add(1, std::memory_order_release);
  state->condition.notify_all();
}

StreamingPipelineConfig MakePipelineConfig(
    const PipelineCase& test_case, const std::filesystem::path& output) {
  StreamingPipelineConfig config;
  config.input_url = SamplePath().string();
  config.output_targets = {output.string()};
  config.processor.output_width = kWidth;
  config.processor.output_height = kHeight;
  config.video_decoder.backend = test_case.decoder;
  config.video_encoder.codec = kMwStreamerCodecH264;
  config.video_encoder.encoder_name = test_case.encoder_name;
  config.video_encoder.frame_rate = {10, 1};
  if (std::string_view(test_case.encoder_name).find("_nvenc") !=
      std::string_view::npos) {
    config.video_encoder.properties = {{"preset", "p1"}, {"tune", "ull"}};
  } else {
    config.video_encoder.properties = {{"tune", "zerolatency"}};
  }
  return config;
}

bool RecordedVideoHasOsd(const std::filesystem::path& input_path,
                         std::uint64_t* decoded_frames) {
  InputFormatContext input(input_path.string());
  input.FindStreamInfo();
  const int video_index =
      av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0) {
    throw std::runtime_error("Pipeline Adapter测试输出缺少视频轨");
  }
  const auto* stream = input->streams[video_index];
  StreamInfo stream_info{
      video_index,
      CodecParameters(*stream->codecpar),
      stream->time_base,
  };
  VideoDecoderConfig decoder_config;
  decoder_config.backend = VideoDecoderBackend::kSoftware;
  VideoDecoder decoder(std::move(stream_info), decoder_config);

  bool osd_seen = false;
  decoder.SetOnFrame([&](const mw::streamer::ffmpeg::Frame& frame) {
    const VideoFrameAdapter adapter(frame);
    auto prototype = adapter.view();
    NormalizeColorInfo(&prototype.color);
    const auto bgr = HostMatAdapter::ToBgr(prototype);
    osd_seen = osd_seen || HasOsd(bgr);
    ++*decoded_frames;
  });

  Packet packet;
  while (input.ReadPacket(packet)) {
    if (packet->stream_index == video_index) {
      decoder.Decode(packet);
    }
    packet.Unref();
  }
  decoder.Drain();
  return osd_seen;
}

}  // namespace

TEST_CASE("OpenCV Adapter通过Pipeline叠加OSD并写回软硬件输出") {
  for (const auto& test_case : kPipelineCases) {
    DYNAMIC_SECTION(test_case.name) {
      TestDirectory directory;
      ProcessorState state;
      state.test_case = &test_case;
      StreamingPipeline pipeline(
          MakePipelineConfig(test_case, directory.path() / "processed.mp4"));
      MwStreamerStreamingProcessorCallbacks callbacks{};
      callbacks.user_context = &state;
      callbacks.on_start = OnProcessorStart;
      callbacks.process_video = ProcessVideo;
      pipeline.SetProcessorCallbacks(callbacks);
      pipeline.SetOnStatus(
          [&](StreamingPipelineStatus) { state.condition.notify_all(); });

      pipeline.Start();
      {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, 15s, [&]() {
          const auto status = pipeline.status();
          return status == StreamingPipelineStatus::kRunning ||
                 status == StreamingPipelineStatus::kFailed ||
                 status == StreamingPipelineStatus::kStopped;
        }));
      }
      REQUIRE(pipeline.status() == StreamingPipelineStatus::kRunning);
      const auto frames_at_running =
          state.processed_frames.load(std::memory_order_acquire);
      {
        std::unique_lock<std::mutex> lock(state.mutex);
        REQUIRE(state.condition.wait_for(lock, 15s, [&]() {
          return state.processed_frames.load(std::memory_order_acquire) >=
                     frames_at_running + kFramesAfterRunning ||
                 pipeline.status() == StreamingPipelineStatus::kFailed ||
                 pipeline.status() == StreamingPipelineStatus::kStopped;
        }));
      }
      REQUIRE(state.processed_frames.load(std::memory_order_acquire) >=
              frames_at_running + kFramesAfterRunning);

      pipeline.Stop();
      REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
      const auto performance = pipeline.CollectPerformance();
      CHECK(performance.video.decode.frames > 0);
      CHECK(performance.video.process.frames ==
            state.processed_frames.load(std::memory_order_acquire));
      CHECK(performance.video.encode.frames > 0);

      const auto output_path = FindRecordedMp4(directory.path());
      REQUIRE_FALSE(output_path.empty());
      std::uint64_t decoded_frames = 0;
      CHECK(RecordedVideoHasOsd(output_path, &decoded_frames));
      CHECK(decoded_frames > 0);
    }
  }
}
