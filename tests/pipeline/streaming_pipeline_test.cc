#include "mw/pipeline/streaming_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "Extension/Track.h"
#include "Record/MP4Demuxer.h"
#include "mw/ffmpeg/frame_view.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::ffmpeg::AudioFrameViewAdapter;
using mw::streamer::ffmpeg::VideoFrameViewAdapter;
using mw::streamer::output::OutputSink;
using mw::streamer::pipeline::StreamingPipeline;
using mw::streamer::pipeline::StreamingPipelineConfig;
using mw::streamer::pipeline::StreamingPipelineStatus;

struct RawOutputState {
  std::atomic_size_t video_frames = 0;
  std::atomic_size_t audio_frames = 0;
  std::atomic_size_t invalid_frames = 0;
};

void OnRawVideo(const MwStreamerVideoFrameView* frame, void* user_context) {
  auto& state = *static_cast<RawOutputState*>(user_context);
  if (!frame || frame->buffer.width == 0 || frame->buffer.height == 0 ||
      frame->timestamp.time_base.num <= 0 ||
      frame->timestamp.time_base.den <= 0) {
    state.invalid_frames.fetch_add(1, std::memory_order_relaxed);
  }
  state.video_frames.fetch_add(1, std::memory_order_relaxed);
}

void OnRawAudio(const MwStreamerAudioFrameView* frame, void* user_context) {
  auto& state = *static_cast<RawOutputState*>(user_context);
  if (!frame || !frame->data || frame->sample_rate != 48000 ||
      frame->channel_count == 0 || frame->samples_per_channel == 0 ||
      frame->timestamp.time_base.num <= 0 ||
      frame->timestamp.time_base.den <= 0) {
    state.invalid_frames.fetch_add(1, std::memory_order_relaxed);
  }
  state.audio_frames.fetch_add(1, std::memory_order_relaxed);
}

struct TestOutputSinkState : RawOutputState {
  std::atomic_size_t start_calls = 0;
  std::atomic_size_t stop_calls = 0;
  std::atomic<bool> event_accepted = false;
  std::mutex mutex;
  std::thread::id event_submit_thread;
};

class TestOutputSink final : public OutputSink {
 public:
  explicit TestOutputSink(TestOutputSinkState& state) : state_(state) {}

  void Start() override {
    state_.start_calls.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.event_submit_thread = std::this_thread::get_id();
    }
    const std::string payload = "pointer-down";
    state_.event_accepted.store(
        NotifyEvent("interaction", payload.data(), payload.size(),
                    MwStreamerMediaTimestamp{5, 1, {1, 10}}),
        std::memory_order_relaxed);
  }

  void WriteAudio(mw::streamer::ffmpeg::Frame frame) override {
    if (!frame.get() || !frame->data[0] || frame->sample_rate != 48000 ||
        frame->ch_layout.nb_channels == 0 || frame->nb_samples == 0 ||
        frame->format != AV_SAMPLE_FMT_FLT || frame->pts == AV_NOPTS_VALUE ||
        frame->time_base.num <= 0 || frame->time_base.den <= 0) {
      state_.invalid_frames.fetch_add(1, std::memory_order_relaxed);
    }
    const AudioFrameViewAdapter adapter(frame);
    OnRawAudio(&adapter.view(), static_cast<RawOutputState*>(&state_));
  }

  void WriteVideo(mw::streamer::ffmpeg::Frame frame) override {
    if (!frame.get() || !frame->data[0] || frame->width == 0 ||
        frame->height == 0 || frame->format == AV_PIX_FMT_NONE ||
        frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
        frame->time_base.den <= 0) {
      state_.invalid_frames.fetch_add(1, std::memory_order_relaxed);
    }
    const VideoFrameViewAdapter adapter(frame);
    OnRawVideo(&adapter.view(), static_cast<RawOutputState*>(&state_));
  }

  void Stop() noexcept override {
    state_.stop_calls.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  TestOutputSinkState& state_;
};

class FailingOutputSink final : public OutputSink {
 public:
  explicit FailingOutputSink(TestOutputSinkState& state) : state_(state) {}

  void Start() override {
    state_.start_calls.fetch_add(1, std::memory_order_relaxed);
  }

  void WriteAudio(mw::streamer::ffmpeg::Frame) override {}

  void WriteVideo(mw::streamer::ffmpeg::Frame) override {
    throw std::runtime_error("preview render failed");
  }

  void Stop() noexcept override {
    state_.stop_calls.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  TestOutputSinkState& state_;
};

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-pipeline-" + std::to_string(suffix));
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

std::filesystem::path SamplePath(const char* name) {
  return std::filesystem::path(MW_STREAMING_PIPELINE_TEST_DATA_DIR) / name;
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

void WaitForTerminalStatus(StreamingPipeline& pipeline,
                           std::condition_variable& condition,
                           std::mutex& mutex) {
  std::unique_lock<std::mutex> lock(mutex);
  REQUIRE(condition.wait_for(lock, 10s, [&]() {
    return pipeline.status() == StreamingPipelineStatus::kStopped ||
           pipeline.status() == StreamingPipelineStatus::kFailed;
  }));
}

StreamingPipelineConfig MakeSoftwareConfig(
    const std::filesystem::path& input, const std::filesystem::path& output) {
  StreamingPipelineConfig config;
  config.input_url = input.string();
  config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  config.processor.output_width = 64;
  config.processor.output_height = 64;
  config.video_encoder.frame_rate = {10, 1};
  config.video_encoder.properties["tune"] = "zerolatency";
  config.output_targets = {output.string()};
  return config;
}

}  // namespace

TEST_CASE("StreamingPipeline完成音视频处理并生成fMP4") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   directory.path() / "result.mp4");
  config.standby.enabled = true;
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<StreamingPipelineStatus> statuses;
  std::atomic_size_t end_of_input_calls = 0;
  std::atomic_size_t stop_calls = 0;
  struct CallbackState {
    std::atomic_size_t* end_of_input_calls;
    std::atomic_size_t* stop_calls;
  } callback_state{&end_of_input_calls, &stop_calls};
  TestOutputSinkState sink_state;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* user_context) {
    if (reason == kMwStreamerProcessorEndOfInput) {
      static_cast<CallbackState*>(user_context)
          ->end_of_input_calls->fetch_add(1);
    }
  };
  callbacks.on_stop = [](void* user_context) {
    static_cast<CallbackState*>(user_context)->stop_calls->fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.AddOutputSink("preview",
                         std::make_unique<TestOutputSink>(sink_state));
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  CHECK(end_of_input_calls.load() == 1);
  CHECK(stop_calls.load() == 0);
  const auto performance = pipeline.CollectPerformance();
  CHECK(performance.interval > 0ns);
  CHECK_FALSE(performance.input.is_network);
  CHECK(performance.input.received_bytes == 0);
  CHECK(performance.outputs.empty());
  REQUIRE(performance.has_video);
  REQUIRE(performance.has_audio);
  CHECK(performance.video.decode.frames == 20);
  CHECK(performance.video.process.frames == 20);
  CHECK(performance.video.encode.frames == 21);
  CHECK(performance.video.decode.latency.sample_count > 0);
  CHECK(performance.video.process.latency.sample_count == 20);
  CHECK(performance.video.encode.latency.sample_count == 21);
  CHECK(performance.audio.decode.samples > 0);
  CHECK(performance.audio.process.samples > 0);
  CHECK(performance.audio.encode.samples > 0);
  CHECK(performance.audio.decode.latency.sample_count > 0);
  CHECK(performance.audio.process.latency.sample_count == 95);
  CHECK(performance.audio.encode.latency.sample_count == 95);
  CHECK(performance.video.dropped_packets == 0);
  CHECK(performance.audio.dropped_packets == 0);
  CHECK(sink_state.video_frames.load(std::memory_order_relaxed) > 0);
  CHECK(sink_state.audio_frames.load(std::memory_order_relaxed) > 0);
  CHECK(sink_state.invalid_frames.load(std::memory_order_relaxed) == 0);

  const auto empty_performance = pipeline.CollectPerformance();
  CHECK(empty_performance.video.decode.frames == 0);
  CHECK(empty_performance.video.process.frames == 0);
  CHECK(empty_performance.video.encode.frames == 0);
  CHECK(empty_performance.audio.decode.samples == 0);
  CHECK(empty_performance.audio.process.samples == 0);
  CHECK(empty_performance.audio.encode.samples == 0);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(stop_calls.load() == 1);
  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == StreamingPipelineStatus::kStarting);
    CHECK(statuses[1] == StreamingPipelineStatus::kRunning);
    CHECK(statuses[2] == StreamingPipelineStatus::kStopped);
  }

  const auto output_path = FindRecordedMp4(directory.path());
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  CHECK(demuxer.getTracks(true).size() == 2);
  CHECK(demuxer.getDurationMS() >= 1800);
}

TEST_CASE("StreamingPipeline仅Raw输出并异步通知Processor事件") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   directory.path() / "unused.mp4");
  config.output_targets.clear();

  struct CallbackState {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t event_calls = 0;
    std::string sink_id;
    std::string type;
    std::string payload;
    std::thread::id callback_thread;
  } callback_state;
  TestOutputSinkState sink_state;
  std::mutex status_mutex;
  std::condition_variable status_condition;
  StreamingPipeline pipeline(std::move(config));

  MwStreamerStreamingProcessorCallbacks processor_callbacks{};
  processor_callbacks.user_context = &callback_state;
  processor_callbacks.on_output_event = [](const MwStreamerOutputEvent* event,
                                           void* user_context) {
    auto& state = *static_cast<CallbackState*>(user_context);
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      ++state.event_calls;
      state.sink_id = event->sink_id;
      state.type = event->type;
      state.payload.assign(static_cast<const char*>(event->payload),
                           event->payload_size);
      state.callback_thread = std::this_thread::get_id();
    }
    state.condition.notify_all();
  };
  pipeline.SetProcessorCallbacks(processor_callbacks);
  CHECK_THROWS_AS(
      pipeline.AddOutputSink("", std::make_unique<TestOutputSink>(sink_state)),
      std::invalid_argument);
  CHECK_THROWS_AS(pipeline.AddOutputSink("null", std::unique_ptr<OutputSink>{}),
                  std::invalid_argument);
  pipeline.AddOutputSink("preview",
                         std::make_unique<TestOutputSink>(sink_state));
  CHECK_THROWS_AS(pipeline.AddOutputSink(
                      "preview", std::make_unique<TestOutputSink>(sink_state)),
                  std::invalid_argument);
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { status_condition.notify_all(); });

  pipeline.Start();
  CHECK_THROWS_AS(pipeline.AddOutputSink(
                      "late", std::make_unique<TestOutputSink>(sink_state)),
                  std::logic_error);
  {
    std::unique_lock<std::mutex> lock(status_mutex);
    REQUIRE(status_condition.wait_for(lock, 10s, [&pipeline]() {
      return pipeline.status() == StreamingPipelineStatus::kRunning ||
             pipeline.status() == StreamingPipelineStatus::kFailed;
    }));
  }
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kRunning);

  {
    std::unique_lock<std::mutex> lock(callback_state.mutex);
    REQUIRE(callback_state.condition.wait_for(lock, 2s, [&callback_state]() {
      return callback_state.event_calls == 1;
    }));
    CHECK(callback_state.sink_id == "preview");
    CHECK(callback_state.type == "interaction");
    CHECK(callback_state.payload == "pointer-down");
    std::lock_guard<std::mutex> sink_lock(sink_state.mutex);
    CHECK(callback_state.callback_thread != sink_state.event_submit_thread);
  }

  WaitForTerminalStatus(pipeline, status_condition, status_mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  const auto performance = pipeline.CollectPerformance();
  CHECK(performance.video.process.frames > 0);
  CHECK(performance.audio.process.samples > 0);
  CHECK(performance.video.encode.frames == 0);
  CHECK(performance.audio.encode.samples == 0);
  CHECK(sink_state.start_calls.load(std::memory_order_relaxed) == 1);
  CHECK(sink_state.stop_calls.load(std::memory_order_relaxed) == 1);
  CHECK(sink_state.event_accepted.load(std::memory_order_relaxed));
  CHECK(sink_state.video_frames.load(std::memory_order_relaxed) > 0);
  CHECK(sink_state.audio_frames.load(std::memory_order_relaxed) > 0);
  CHECK(sink_state.invalid_frames.load(std::memory_order_relaxed) == 0);
  CHECK(std::filesystem::is_empty(directory.path()));
  pipeline.Stop();
  CHECK(sink_state.stop_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("StreamingPipeline隔离失败Sink并继续向健康Sink输出") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   directory.path() / "unused.mp4");
  config.output_targets.clear();

  TestOutputSinkState failing_state;
  TestOutputSinkState healthy_state;
  std::mutex status_mutex;
  std::condition_variable status_condition;
  StreamingPipeline pipeline(std::move(config));
  pipeline.AddOutputSink("failing",
                         std::make_unique<FailingOutputSink>(failing_state));
  pipeline.AddOutputSink("healthy",
                         std::make_unique<TestOutputSink>(healthy_state));
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { status_condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, status_condition, status_mutex);

  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  CHECK(failing_state.start_calls.load(std::memory_order_relaxed) == 1);
  CHECK(failing_state.stop_calls.load(std::memory_order_relaxed) == 1);
  CHECK(healthy_state.start_calls.load(std::memory_order_relaxed) == 1);
  CHECK(healthy_state.stop_calls.load(std::memory_order_relaxed) == 1);
  CHECK(healthy_state.video_frames.load(std::memory_order_relaxed) > 0);
  CHECK(healthy_state.audio_frames.load(std::memory_order_relaxed) > 0);
  CHECK(healthy_state.invalid_frames.load(std::memory_order_relaxed) == 0);
  CHECK(std::filesystem::is_empty(directory.path()));
  pipeline.Stop();
  CHECK(failing_state.stop_calls.load(std::memory_order_relaxed) == 1);
  CHECK(healthy_state.stop_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("StreamingPipeline软件解码后使用NVENC硬件编码") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "result.mp4");
  config.processor.output_width = 256;
  config.processor.output_height = 144;
  config.video_encoder.encoder_name = "h264_nvenc";
  config.video_encoder.properties = {
      {"preset", "p1"},
      {"tune", "ull"},
  };

  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);

  const auto performance = pipeline.CollectPerformance();
  CHECK(performance.video.decode.frames == 20);
  CHECK(performance.video.process.frames == 20);
  CHECK(performance.video.encode.frames == 20);
  pipeline.Stop();

  const auto output_path = FindRecordedMp4(directory.path());
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  CHECK(demuxer.getTracks(true).size() == 1);
  CHECK(demuxer.getDurationMS() >= 1800);
}

TEST_CASE("StreamingPipeline没有Sink时不编码并正常结束") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   directory.path() / "unused.mp4");
  config.output_targets.clear();
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<StreamingPipelineStatus> statuses;
  StreamingPipeline pipeline(std::move(config));
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);

  const auto performance = pipeline.CollectPerformance();
  REQUIRE(performance.has_video);
  REQUIRE(performance.has_audio);
  CHECK(performance.video.process.frames == 20);
  CHECK(performance.video.encode.frames == 0);
  CHECK(performance.audio.process.samples > 0);
  CHECK(performance.audio.encode.samples == 0);
  CHECK(performance.video.encode.latency.sample_count == 0);
  CHECK(performance.audio.encode.latency.sample_count == 0);
  CHECK(performance.outputs.empty());
  CHECK(std::filesystem::is_empty(directory.path()));

  pipeline.Stop();
  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == StreamingPipelineStatus::kStarting);
    CHECK(statuses[1] == StreamingPipelineStatus::kRunning);
    CHECK(statuses[2] == StreamingPipelineStatus::kStopped);
  }
}

TEST_CASE("StreamingPipeline同时录像源输入和处理后输出") {
  TestDirectory directory;
  const auto input_directory = directory.path() / "input";
  const auto output_directory = directory.path() / "processed";
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   output_directory / "result.mp4");
  config.processor.output_width = 32;
  config.processor.output_height = 32;
  config.input_targets = {(input_directory / "source.mp4").string()};

  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  pipeline.Stop();

  const auto input_path = FindRecordedMp4(input_directory);
  const auto output_path = FindRecordedMp4(output_directory);
  REQUIRE_FALSE(input_path.empty());
  REQUIRE_FALSE(output_path.empty());

  mediakit::MP4Demuxer input_demuxer;
  input_demuxer.openMP4(input_path.string());
  const auto input_tracks = input_demuxer.getTracks(true);
  REQUIRE(input_tracks.size() == 2);
  const auto input_video = std::find_if(
      input_tracks.begin(), input_tracks.end(), [](const auto& track) {
        return track->getTrackType() == mediakit::TrackVideo;
      });
  REQUIRE(input_video != input_tracks.end());
  const auto typed_input_video =
      std::dynamic_pointer_cast<mediakit::VideoTrack>(*input_video);
  REQUIRE(typed_input_video);
  CHECK(typed_input_video->getVideoWidth() == 64);
  CHECK(typed_input_video->getVideoHeight() == 64);
  CHECK(input_demuxer.getDurationMS() >= 1800);

  mediakit::MP4Demuxer output_demuxer;
  output_demuxer.openMP4(output_path.string());
  const auto output_tracks = output_demuxer.getTracks(true);
  REQUIRE(output_tracks.size() == 2);
  const auto output_video = std::find_if(
      output_tracks.begin(), output_tracks.end(), [](const auto& track) {
        return track->getTrackType() == mediakit::TrackVideo;
      });
  REQUIRE(output_video != output_tracks.end());
  const auto typed_output_video =
      std::dynamic_pointer_cast<mediakit::VideoTrack>(*output_video);
  REQUIRE(typed_output_video);
  CHECK(typed_output_video->getVideoWidth() == 32);
  CHECK(typed_output_video->getVideoHeight() == 32);
  CHECK(output_demuxer.getDurationMS() >= 1800);
}

TEST_CASE("StreamingPipeline隔离输入旁路目录创建失败") {
  TestDirectory directory;
  const auto blocked_path = directory.path() / "blocked";
  std::ofstream(blocked_path).put('x');
  const auto output_directory = directory.path() / "processed";
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   output_directory / "result.mp4");
  config.input_targets = {(blocked_path / "source.mp4").string()};

  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  pipeline.Stop();

  CHECK_FALSE(std::filesystem::is_directory(blocked_path));
  const auto output_path = FindRecordedMp4(output_directory);
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  CHECK(demuxer.getTracks(true).size() == 2);
  CHECK(demuxer.getDurationMS() >= 1800);
}

TEST_CASE("StreamingPipeline支持纯视频处理") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "video.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  pipeline.Stop();

  const auto output_path = FindRecordedMp4(directory.path());
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  const auto tracks = demuxer.getTracks(true);
  REQUIRE(tracks.size() == 1);
  CHECK(tracks.front()->getTrackType() == mediakit::TrackVideo);
}

TEST_CASE("StreamingPipeline同步拒绝无效配置") {
  StreamingPipelineConfig config;
  StreamingPipeline pipeline(std::move(config));

  CHECK_NOTHROW(pipeline.UpdateProcessorConfig("updated"));
  CHECK_THROWS_AS(pipeline.Start(), std::invalid_argument);
  CHECK(pipeline.status() == StreamingPipelineStatus::kIdle);
}

TEST_CASE("StreamingPipeline同步拒绝负数轨道等待时间") {
  StreamingPipelineConfig config;
  config.input_url = "input.mp4";
  config.output_targets = {"output.mp4"};
  config.max_track_wait = -1ms;
  StreamingPipeline pipeline(std::move(config));

  CHECK_THROWS_AS(pipeline.Start(), std::invalid_argument);
  CHECK(pipeline.status() == StreamingPipelineStatus::kIdle);
}

TEST_CASE("StreamingPipeline保留Processor启动失败状态") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "failed.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.on_start = [](const MwStreamerStreamingProcessorStartRequest*,
                          void*) -> MwStreamerProcessorStartResult {
    return kMwStreamerProcessorStartFailed;
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kFailed);
  pipeline.Stop();
  CHECK(pipeline.status() == StreamingPipelineStatus::kFailed);
}

TEST_CASE("StreamingPipeline消化运行期异常并等待外部停止") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "failed-runtime.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<StreamingPipelineStatus> statuses;
  std::atomic_size_t stop_calls = 0;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &stop_calls;
  callbacks.process_video = [](const MwStreamerStreamingVideoProcessRequest*,
                               void*) {
    throw std::runtime_error("业务处理失败");
  };
  callbacks.on_stop = [](void* user_context) {
    static_cast<std::atomic_size_t*>(user_context)->fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kFailed);
  CHECK(stop_calls.load() == 0);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.status() == StreamingPipelineStatus::kFailed);
  CHECK(stop_calls.load() == 1);
  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE_FALSE(statuses.empty());
    CHECK(statuses.back() == StreamingPipelineStatus::kFailed);
  }
}

TEST_CASE("StreamingPipeline可在异步初始化期间立即停止") {
  TestDirectory directory;
  for (int iteration = 0; iteration < 10; ++iteration) {
    auto config = MakeSoftwareConfig(
        SamplePath("h264_video.mp4"),
        directory.path() / ("stopped-" + std::to_string(iteration) + ".mp4"));
    StreamingPipeline pipeline(std::move(config));

    pipeline.Start();
    pipeline.Stop();
    CHECK(pipeline.status() == StreamingPipelineStatus::kStopped);
  }
}

TEST_CASE("StreamingPipeline性能采集可与外部停止并发") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "collected.mp4");
  StreamingPipeline pipeline(std::move(config));
  std::atomic_bool collecting = true;
  std::atomic_bool collection_attempted = false;
  std::atomic_size_t collection_count = 0;
  std::exception_ptr collection_error;

  pipeline.Start();
  std::thread collector([&]() {
    try {
      while (collecting.load(std::memory_order_acquire)) {
        static_cast<void>(pipeline.CollectPerformance());
        collection_count.fetch_add(1, std::memory_order_release);
        collection_attempted.store(true, std::memory_order_release);
      }
    } catch (...) {
      collection_error = std::current_exception();
      collection_attempted.store(true, std::memory_order_release);
    }
  });
  while (!collection_attempted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  pipeline.Stop();
  collecting.store(false, std::memory_order_release);
  collector.join();

  if (collection_error) {
    std::rethrow_exception(collection_error);
  }
  CHECK(pipeline.status() == StreamingPipelineStatus::kStopped);
}

TEST_CASE("StreamingPipeline支持外部线程更新Processor配置并停止") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "updated.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  struct CallbackState {
    std::mutex mutex;
    std::string initial_config;
    std::string updated_config;
    std::size_t update_calls = 0;
  } callback_state;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.on_start =
      [](const MwStreamerStreamingProcessorStartRequest* request,
         void* user_context) {
        auto& state = *static_cast<CallbackState*>(user_context);
        std::lock_guard<std::mutex> lock(state.mutex);
        state.initial_config = request->config->config;
        return kMwStreamerProcessorStartSuccess;
      };
  callbacks.update_config = [](const char* updated_config, void* user_context) {
    auto& state = *static_cast<CallbackState*>(user_context);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.updated_config = updated_config;
    ++state.update_calls;
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.UpdateProcessorConfig("initial");
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    CHECK(callback_state.update_calls == 0);
  }
  pipeline.Start();
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(condition.wait_for(lock, 10s, [&pipeline]() {
      return pipeline.status() == StreamingPipelineStatus::kRunning ||
             pipeline.status() == StreamingPipelineStatus::kFailed;
    }));
  }
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kRunning);
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    CHECK(callback_state.initial_config == "initial");
    CHECK(callback_state.update_calls == 0);
  }
  pipeline.UpdateProcessorConfig("updated");
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    CHECK(callback_state.updated_config == "updated");
    CHECK(callback_state.update_calls == 1);
  }
  pipeline.Stop();
  CHECK(pipeline.status() == StreamingPipelineStatus::kStopped);
  CHECK_THROWS_AS(pipeline.UpdateProcessorConfig("stopped"), std::logic_error);
}
