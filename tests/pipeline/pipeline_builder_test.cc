#include "mw/pipeline/pipeline_builder.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "mw/config/toml.h"
#include "mw/ffmpeg/input_format_context.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::input::FileInputConfig;
using mw::streamer::input::InputState;
using namespace mw::streamer::pipeline;
namespace config = mw::streamer::config;

class TestDirectory final {
 public:
  TestDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("mw-pipeline-builder-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

PipelineConfig FileInputConfig() {
  PipelineConfig pipeline;
  pipeline.input.options.url =
      std::filesystem::absolute(
          std::filesystem::path(MW_PIPELINE_BUILDER_TEST_DATA_DIR) /
          "h264_aac.mp4")
          .string();
  pipeline.input.options.reconnect_policy.max_retries = 0;
  return pipeline;
}

struct AnalysisState {
  std::atomic<int> starts{0};
  std::atomic<int> audios{0};
  std::atomic<int> videos{0};
  std::atomic<int> ends{0};
  std::atomic<int> stops{0};
  std::atomic<bool> valid_frames{true};
  int audios_at_end = 0;
  int videos_at_end = 0;
  bool has_audio = false;
  bool has_video = false;
  std::string initial_config;
};

MwStreamerFileProcessorCallbacks AnalysisCallbacks(AnalysisState& state) {
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerFileProcessorStartRequest* request,
                          void* context) {
    auto& state = *static_cast<AnalysisState*>(context);
    ++state.starts;
    state.has_audio = request->source_info->has_audio;
    state.has_video = request->source_info->has_video;
    state.initial_config = request->config->config;
    return kMwStreamerProcessorStartSuccess;
  };
  callbacks.process_audio = [](const MwStreamerAudioFrameView* frame,
                               void* context) {
    auto& state = *static_cast<AnalysisState*>(context);
    if (!frame->data || frame->samples_per_channel == 0 ||
        frame->channel_count == 0) {
      state.valid_frames = false;
    }
    ++state.audios;
  };
  callbacks.process_video = [](const MwStreamerVideoFrameView* frame,
                               void* context) {
    auto& state = *static_cast<AnalysisState*>(context);
    if (frame->buffer.width != 64 || frame->buffer.height != 64 ||
        frame->buffer.memory_type != kMwStreamerMemoryHost) {
      state.valid_frames = false;
    }
    ++state.videos;
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* context) {
    if (reason != kMwStreamerProcessorEndOfInput) return;
    auto& state = *static_cast<AnalysisState*>(context);
    state.audios_at_end = state.audios;
    state.videos_at_end = state.videos;
    ++state.ends;
  };
  callbacks.on_stop = [](void* context) {
    ++static_cast<AnalysisState*>(context)->stops;
  };
  return callbacks;
}

void CheckRecordedMedia(const std::filesystem::path& path) {
  mw::streamer::ffmpeg::InputFormatContext input(path.string());
  input.FindStreamInfo();
  REQUIRE(input->nb_streams == 2);
  std::vector<int> counts(input->nb_streams, 0);
  mw::streamer::ffmpeg::Packet packet;
  while (input.ReadPacket(packet)) {
    ++counts.at(packet->stream_index);
    packet.Unref();
  }
  bool has_audio = false;
  bool has_video = false;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    CHECK(counts[i] > 0);
    const auto& parameters = *input->streams[i]->codecpar;
    if (parameters.codec_type == AVMEDIA_TYPE_AUDIO) {
      CHECK(parameters.codec_id == AV_CODEC_ID_AAC);
      has_audio = true;
    } else if (parameters.codec_type == AVMEDIA_TYPE_VIDEO) {
      CHECK(parameters.codec_id == AV_CODEC_ID_H264);
      CHECK(parameters.width == 64);
      CHECK(parameters.height == 64);
      has_video = true;
    }
  }
  CHECK(has_audio);
  CHECK(has_video);
}

TEST_CASE("Pipeline builder owns typed configuration and binds analysis by ID",
          "[pipeline][builder]") {
  AnalysisState state;
  auto pipeline = [&] {
    auto config = FileInputConfig();
    config.input.downstream = {"decode"};
    auto decoder = std::make_unique<DecoderNodeConfig>("decode");
    decoder->options.video_decoder.backend =
        mw::streamer::decoder::VideoDecoderBackend::kSoftware;
    decoder->downstream = {"analysis"};
    auto analysis = std::make_unique<AnalysisProcessorNodeConfig>("analysis");
    analysis->options.config = "mode = 'integration'";
    // Forward references must work independently of declaration order.
    config.sinks.push_back(std::move(analysis));
    config.sinks.push_back(std::move(decoder));
    ProcessorBindings bindings;
    bindings.analysis.emplace("analysis", AnalysisCallbacks(state));
    return BuildPipeline(config, bindings);
  }();

  // Both configuration and binding containers have been destroyed.
  pipeline->Start();
  const bool finished = WaitUntil([&] {
    return state.ends > 0 || pipeline->state() == PipelineState::kFailed;
  });
  pipeline->Stop();
  INFO(pipeline->error());
  INFO(pipeline->input_status().error);
  REQUIRE(finished);
  CHECK(pipeline->state() == PipelineState::kStopped);
  CHECK(state.starts.load() == 1);
  CHECK(state.ends.load() == 1);
  CHECK(state.stops.load() == 1);
  CHECK(state.has_audio);
  CHECK(state.has_video);
  CHECK(state.initial_config == "mode = 'integration'");
  CHECK(state.valid_frames.load());
  CHECK(state.audios.load() > 0);
  CHECK(state.videos.load() > 0);
  CHECK(state.audios_at_end == state.audios.load());
  CHECK(state.videos_at_end == state.videos.load());
}

TEST_CASE("Round-tripped TOML builds a playable local stream-copy recording",
          "[pipeline][builder][toml]") {
  TestDirectory directory;
  auto pipeline = [&] {
    auto original = FileInputConfig();
    original.input.downstream = {"recording"};
    auto recording = std::make_unique<RemuxNodeConfig>("recording");
    recording->options.target = (directory.path() / "original.mp4").string();
    original.sinks.push_back(std::move(recording));
    auto parsed = config::ParsePipelineConfigFromToml(
        config::SerializePipelineConfigToToml(original));
    auto restored = config::ParsePipelineConfigFromToml(
        config::SerializePipelineConfigToToml(parsed));
    return BuildPipeline(restored);
  }();

  pipeline->Start();
  const bool finished = WaitUntil([&] {
    const auto input_state = pipeline->input_status().state;
    return input_state == InputState::kEnded ||
           input_state == InputState::kFailed ||
           pipeline->state() == PipelineState::kFailed;
  });
  const auto input_status = pipeline->input_status();
  // Input EOF precedes completion of the Remux queue; Stop drains it.
  pipeline->Stop();
  INFO(pipeline->error());
  INFO(input_status.error);
  REQUIRE(finished);
  REQUIRE(input_status.state == InputState::kEnded);
  REQUIRE(pipeline->state() == PipelineState::kStopped);
  std::vector<std::filesystem::path> recordings;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory.path())) {
    if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
      recordings.push_back(entry.path());
    }
  }
  REQUIRE(recordings.size() == 1);
  CheckRecordedMedia(recordings.front());
}

}  // namespace
