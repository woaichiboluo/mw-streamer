#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/ffmpeg/frame.h"
#include "mw/pipeline/pipeline.h"
#include "mw/processor/analysis_processor_sink.h"
#include "mw/processor/transform_processor_sink.h"

namespace {

using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::input::Input;
using mw::streamer::input::InputState;
using mw::streamer::media::FrameReady;
using mw::streamer::media::FrameStreamsReady;
using mw::streamer::media::StreamEnded;
using mw::streamer::media::StreamEndReason;
using mw::streamer::media::StreamsReady;
using mw::streamer::media::TimelineReset;
using mw::streamer::media::TimelineResetReason;
using mw::streamer::performance::NodeSnapshot;
using mw::streamer::performance::OperationSnapshot;
using mw::streamer::performance::PerformanceType;
using mw::streamer::performance::PerformanceUnit;
using mw::streamer::processor::AnalysisProcessorSink;
using mw::streamer::processor::TransformProcessorSink;
using mw::streamer::sink::Sink;
using mw::streamer::sink::SinkMediaType;
using mw::streamer::sink::SinkMessage;
using namespace mw::streamer::pipeline;

FrameStreamsReady Streams(std::uint64_t generation = 1) {
  StreamInfo video;
  video.stream_index = 0;
  video.time_base = {1, 90000};
  auto* video_parameters = video.codec_parameters.get();
  video_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  video_parameters->codec_id = AV_CODEC_ID_H264;
  video_parameters->width = 64;
  video_parameters->height = 32;
  video_parameters->format = AV_PIX_FMT_YUV420P;
  video_parameters->framerate = {25, 1};
  StreamInfo audio;
  audio.stream_index = 1;
  audio.time_base = {1, 44100};
  auto* audio_parameters = audio.codec_parameters.get();
  audio_parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_parameters->codec_id = AV_CODEC_ID_AAC;
  audio_parameters->sample_rate = 44100;
  av_channel_layout_default(&audio_parameters->ch_layout, 2);
  return {generation, {std::move(video), std::move(audio)}, nullptr};
}

// Message tests use the same Pipeline dispatcher as production. No decoding
// is needed: this input only announces readiness and the bridge supplies the
// raw-frame metadata expected by the processor.
class MessageInput final : public Input {
 public:
  void Start(Observer& observer) override {
    state_.store(InputState::kReady);
    observer.OnStreamsReady({1, {}});
  }
  void Stop() noexcept override { state_.store(InputState::kStopped); }
  InputState state() const noexcept override { return state_.load(); }

 private:
  std::atomic<InputState> state_{InputState::kIdle};
};

class FrameBridge final : public Sink {
 public:
  FrameBridge()
      : Sink("bridge", SinkMediaType::kPacket, SinkMediaType::kFrame) {}
  ~FrameBridge() override { Stop(); }
  void OnStreamsReady(const StreamsReady&) override {
    StartMessages();
    SendStreamsReady(Streams());
  }
};

Frame Video(int width = 64, int height = 32) {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  frame->pts = 9000;
  frame->duration = 3600;
  frame->time_base = {1, 90000};
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->chroma_location = AVCHROMA_LOC_LEFT;
  REQUIRE(av_frame_get_buffer(frame.get(), 32) >= 0);
  for (int plane = 0; plane < 3; ++plane) {
    const int rows = plane == 0 ? height : height / 2;
    std::memset(frame->data[plane], 0x21,
                static_cast<std::size_t>(rows) * frame->linesize[plane]);
  }
  return frame;
}

Frame Audio() {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 4;
  frame->pts = 480;
  frame->duration = 4;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  auto* samples = reinterpret_cast<float*>(frame->extended_data[0]);
  for (int index = 0; index < 8; ++index) {
    samples[index] = static_cast<float>(index + 1);
  }
  return frame;
}

struct CallbackState {
  int starts = 0;
  int videos = 0;
  int audios = 0;
  int resets = 0;
  int ends = 0;
  int stops = 0;
  bool fail_start = false;
  bool independent_video = true;
  bool independent_audio = true;
  std::string initial_config;
  std::string updated_config;
  MwStreamerProcessorSourceInfo source{};
  MwStreamerVideoOutputSize video_output_size{64, 32};
};

void Boundary(MwStreamerProcessorBoundaryReason reason, void* context) {
  auto& state = *static_cast<CallbackState*>(context);
  if (reason == kMwStreamerProcessorTimelineReset) {
    ++state.resets;
  } else if (reason == kMwStreamerProcessorEndOfInput) {
    ++state.ends;
  }
}

void Update(const char* config, void* context) {
  static_cast<CallbackState*>(context)->updated_config = config;
}

void StopCallback(void* context) {
  ++static_cast<CallbackState*>(context)->stops;
}

MwStreamerAnalysisProcessorCallbacks AnalysisCallbacks(CallbackState& state) {
  MwStreamerAnalysisProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start =
      [](const MwStreamerAnalysisProcessorStartRequest* request,
         void* context) {
        auto& state = *static_cast<CallbackState*>(context);
        ++state.starts;
        state.initial_config = request->config->config;
        state.source = *request->source_info;
        return state.fail_start ? kMwStreamerProcessorStartFailed
                                : kMwStreamerProcessorStartSuccess;
      };
  callbacks.process_video = [](const MwStreamerVideoFrameView*, void* context) {
    ++static_cast<CallbackState*>(context)->videos;
  };
  callbacks.process_audio = [](const MwStreamerAudioFrameView*, void* context) {
    ++static_cast<CallbackState*>(context)->audios;
  };
  callbacks.on_boundary = Boundary;
  callbacks.on_config_update = Update;
  callbacks.on_stop = StopCallback;
  return callbacks;
}

MwStreamerTransformProcessorCallbacks TransformCallbacks(CallbackState& state) {
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start =
      [](const MwStreamerTransformProcessorStartRequest* request,
         void* context) {
        auto& state = *static_cast<CallbackState*>(context);
        ++state.starts;
        state.initial_config = request->config->config;
        state.source = *request->source_info;
        if (request->video_output_size) {
          *request->video_output_size = state.video_output_size;
        }
        return state.fail_start ? kMwStreamerProcessorStartFailed
                                : kMwStreamerProcessorStartSuccess;
      };
  callbacks.process_video =
      [](const MwStreamerTransformVideoProcessRequest* request, void* context) {
        auto& state = *static_cast<CallbackState*>(context);
        ++state.videos;
        state.independent_video &=
            request->input->buffer.storage.linear.planes[0].address !=
            request->output->storage.linear.planes[0].address;
        for (std::uint32_t plane = 0;
             plane < request->output->storage.linear.plane_count; ++plane) {
          const auto& view = request->output->storage.linear.planes[plane];
          auto* data = reinterpret_cast<std::uint8_t*>(view.address);
          for (std::uint32_t row = 0; row < view.row_count; ++row) {
            std::memset(data + row * view.stride_bytes, 0x40 + state.videos,
                        view.row_bytes);
          }
        }
      };
  callbacks.process_audio =
      [](const MwStreamerTransformAudioProcessRequest* request, void* context) {
        auto& state = *static_cast<CallbackState*>(context);
        ++state.audios;
        state.independent_audio &=
            request->input->data != request->output->data;
        const auto samples =
            request->input->channel_count * request->input->samples_per_channel;
        for (std::uint32_t index = 0; index < samples; ++index) {
          request->output->data[index] = request->input->data[index] *
                                         static_cast<float>(state.audios + 1);
        }
      };
  callbacks.on_boundary = Boundary;
  callbacks.on_config_update = Update;
  callbacks.on_stop = StopCallback;
  return callbacks;
}

struct Recorded {
  std::vector<FrameReady> video;
  std::vector<FrameReady> audio;
  std::vector<std::string> events;
  int stops = 0;
  bool throw_video = false;
  std::promise<void>* video_entered = nullptr;
  std::shared_future<void> release_video;
};

class Recorder final : public Sink {
 public:
  explicit Recorder(Recorded& recorded, std::string id = "recorder")
      : Sink(std::move(id), SinkMediaType::kFrame), recorded_(recorded) {}

  void EmitMessage(const SinkMessage& message) { SendMessage(message); }

  NodeSnapshot GetOwnPerformance() const override {
    NodeSnapshot snapshot;
    snapshot.name = "Recorder";
    return snapshot;
  }

  void OnStreamsReady(const FrameStreamsReady&) override {
    StartMessages();
    recorded_.events.emplace_back("ready");
  }
  void OnVideoFrame(const FrameReady& frame) override {
    if (recorded_.throw_video) {
      throw std::runtime_error("consumer failed");
    }
    if (recorded_.video_entered) {
      recorded_.video_entered->set_value();
      recorded_.release_video.wait();
    }
    recorded_.video.push_back(frame);
  }
  void OnAudioFrame(const FrameReady& frame) override {
    recorded_.audio.push_back(frame);
  }
  void OnTimelineReset(const TimelineReset&) override {
    recorded_.events.emplace_back("reset");
  }
  void OnInputEnded(const StreamEnded&) override {
    recorded_.events.emplace_back("end");
  }
  void Stop() noexcept override { ++recorded_.stops; }

 private:
  Recorded& recorded_;
};

const OperationSnapshot& Operation(const NodeSnapshot& snapshot,
                                   PerformanceType type) {
  for (const auto& operation : snapshot.operations) {
    if (operation.type == type) {
      return operation;
    }
  }
  throw std::logic_error("缺少预期的Processor统计项");
}

void CheckVideoMetadata(const Frame& actual, const Frame& input) {
  CHECK(actual->pts == input->pts);
  CHECK(actual->duration == input->duration);
  CHECK(actual->time_base.num == input->time_base.num);
  CHECK(actual->time_base.den == input->time_base.den);
  CHECK(actual->color_range == input->color_range);
  CHECK(actual->colorspace == input->colorspace);
  CHECK(actual->color_primaries == input->color_primaries);
  CHECK(actual->color_trc == input->color_trc);
  CHECK(actual->chroma_location == input->chroma_location);
}

}  // namespace

TEST_CASE("AnalysisProcessorSink只消费输入并保留跨代处理上下文") {
  CallbackState state;
  {
    AnalysisProcessorSink sink("processor", AnalysisCallbacks(state));
    sink.UpdateConfig("initial");
    sink.OnStreamsReady(Streams());
    auto video = Video();
    auto audio = Audio();
    sink.OnVideoFrame({1, video});
    sink.OnAudioFrame({1, audio});
    sink.UpdateConfig("updated");
    sink.OnInputEnded({1, StreamEndReason::kInterrupted});
    CHECK(state.ends == 0);
    sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
    sink.OnStreamsReady(Streams(2));
    sink.OnVideoFrame({2, video});
    sink.OnInputEnded({2, StreamEndReason::kEof});
    sink.Stop();
    sink.Stop();
    CHECK(video->data[0][0] == 0x21);
    CHECK(reinterpret_cast<float*>(audio->data[0])[0] == 1.0F);
  }
  CHECK(state.starts == 1);
  CHECK(state.videos == 2);
  CHECK(state.audios == 1);
  CHECK(state.resets == 1);
  CHECK(state.ends == 1);
  CHECK(state.stops == 1);
  CHECK(state.initial_config == "initial");
  CHECK(state.updated_config == "updated");
  CHECK(state.source.video.width == 64);
  CHECK(state.source.audio.sample_rate == 44100);
}

TEST_CASE("TransformProcessorSink无回调时引用原始音视频而不复制或改写") {
  Recorded first;
  Recorded second;
  auto video = Video();
  auto audio = Audio();
  {
    TransformProcessorSink sink("processor", {});
    sink.AddSink(std::make_unique<Recorder>(first));
    sink.AddSink(std::make_unique<Recorder>(second, "second"));
    sink.OnStreamsReady(Streams());
    sink.OnVideoFrame({1, video});
    sink.OnAudioFrame({1, audio});
    sink.OnInputEnded({1, StreamEndReason::kEof});
    sink.Stop();
    sink.Stop();
  }
  for (const auto* output : {&first, &second}) {
    REQUIRE(output->video.size() == 1);
    REQUIRE(output->audio.size() == 1);
    CHECK(output->video[0].generation == 1);
    CHECK(output->audio[0].generation == 1);
    const auto& out_video = output->video[0].frame;
    const auto& out_audio = output->audio[0].frame;
    CHECK(out_video->data[0] == video->data[0]);
    CHECK(out_audio->data[0] == audio->data[0]);
    CHECK(out_video->width == 64);
    CHECK(out_video->height == 32);
    CheckVideoMetadata(out_video, video);
    CHECK(out_audio->pts == audio->pts);
    CHECK(out_audio->duration == audio->duration);
    CHECK(out_audio->nb_samples == 4);
    CHECK(out_audio->sample_rate == 48000);
    CHECK(out_audio->format == AV_SAMPLE_FMT_FLT);
    CHECK(av_channel_layout_compare(&out_audio->ch_layout, &audio->ch_layout) ==
          0);
    CHECK(output->events == std::vector<std::string>{"ready", "end"});
    CHECK(output->stops == 1);
  }
}

TEST_CASE("TransformProcessorSink无回调时透传任意视频尺寸") {
  Recorded recorded;
  TransformProcessorSink sink("processor", {});
  sink.AddSink(std::make_unique<Recorder>(recorded));
  sink.OnStreamsReady(Streams());
  sink.OnVideoFrame({1, Video()});
  sink.OnVideoFrame({1, Video(32, 16)});
  REQUIRE(recorded.video.size() == 2);
  CHECK(recorded.video[0].frame->data[0][0] == 0x21);
  CHECK(recorded.video[1].frame->width == 32);
  CHECK(recorded.video[1].frame->height == 16);
}

TEST_CASE("TransformProcessorSink未设置尺寸时使用1920x1080") {
  Recorded recorded;
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.process_video = [](const MwStreamerTransformVideoProcessRequest*,
                               void*) {};
  TransformProcessorSink sink("processor", callbacks);
  sink.AddSink(std::make_unique<Recorder>(recorded));
  sink.OnStreamsReady(Streams());
  sink.OnVideoFrame({1, Video()});
  REQUIRE(recorded.video.size() == 1);
  CHECK(recorded.video.front().frame->width == 1920);
  CHECK(recorded.video.front().frame->height == 1080);
}

TEST_CASE("TransformProcessorSink拒绝on_start返回无效输出尺寸") {
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.on_start =
      [](const MwStreamerTransformProcessorStartRequest* request, void*) {
        request->video_output_size->width = 0;
        return kMwStreamerProcessorStartSuccess;
      };
  Recorded recorded;
  TransformProcessorSink sink("processor", callbacks);
  sink.AddSink(std::make_unique<Recorder>(recorded));
  CHECK_THROWS_AS(sink.OnStreamsReady(Streams()), std::invalid_argument);
}

TEST_CASE("TransformProcessorSink拒绝处理期间改变输出尺寸") {
  struct State {
    int calls = 0;
  } state;
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start =
      [](const MwStreamerTransformProcessorStartRequest* request, void*) {
        *request->video_output_size = {32, 16};
        return kMwStreamerProcessorStartSuccess;
      };
  callbacks.process_video =
      [](const MwStreamerTransformVideoProcessRequest* request, void* context) {
        auto& state = *static_cast<State*>(context);
        if (++state.calls == 2) request->output->width = 16;
      };
  Recorded recorded;
  TransformProcessorSink sink("processor", callbacks);
  sink.AddSink(std::make_unique<Recorder>(recorded));
  sink.OnStreamsReady(Streams());
  sink.OnVideoFrame({1, Video()});
  CHECK_THROWS_AS(sink.OnVideoFrame({1, Video()}),
                  mw::streamer::sink::FatalError);
}

TEST_CASE("TransformProcessorSink回调使用独立输出并向多个下游保留结果") {
  CallbackState state;
  Recorded first;
  Recorded second;
  auto video = Video();
  auto audio = Audio();
  {
    state.video_output_size = {32, 16};
    TransformProcessorSink sink("processor", TransformCallbacks(state));
    sink.UpdateConfig("initial");
    sink.AddSink(std::make_unique<Recorder>(first));
    sink.AddSink(std::make_unique<Recorder>(second, "second"));
    sink.OnStreamsReady(Streams());
    sink.OnVideoFrame({1, video});
    sink.OnAudioFrame({1, audio});
    sink.UpdateConfig("updated");
    sink.OnTimelineReset(
        {2, TimelineResetReason::kSeek, std::chrono::milliseconds(0)});
    sink.OnStreamsReady(Streams(2));
    sink.OnVideoFrame({2, video});
    sink.OnAudioFrame({2, audio});
    sink.OnInputEnded({2, StreamEndReason::kEof});
    sink.Stop();
    sink.Stop();
  }
  CHECK(state.independent_video);
  CHECK(state.independent_audio);
  CHECK(state.starts == 1);
  CHECK(state.stops == 1);
  CHECK(state.resets == 1);
  CHECK(state.ends == 1);
  CHECK(state.updated_config == "updated");
  CHECK(video->data[0][0] == 0x21);
  CHECK(reinterpret_cast<float*>(audio->data[0])[0] == 1.0F);
  for (const auto* output : {&first, &second}) {
    REQUIRE(output->video.size() == 2);
    REQUIRE(output->audio.size() == 2);
    CHECK(output->video[0].frame->data[0][0] == 0x41);
    CHECK(output->video[1].frame->data[0][0] == 0x42);
    CHECK(output->video[0].frame->data[0] != output->video[1].frame->data[0]);
    CHECK(output->video[0].frame->width == 32);
    CHECK(output->video[0].frame->height == 16);
    CHECK(output->video[1].generation == 2);
    CheckVideoMetadata(output->video[0].frame, video);
    CHECK(reinterpret_cast<float*>(output->audio[0].frame->data[0])[0] == 2.0F);
    CHECK(reinterpret_cast<float*>(output->audio[1].frame->data[0])[0] == 3.0F);
    CHECK(output->audio[0].frame->data[0] != output->audio[1].frame->data[0]);
    CHECK(output->audio[0].frame->pts == audio->pts);
    CHECK(output->audio[0].frame->duration == audio->duration);
    CHECK(output->events ==
          std::vector<std::string>{"ready", "reset", "ready", "end"});
    CHECK(output->stops == 1);
  }
  CHECK(first.video[0].frame->data[0] == second.video[0].frame->data[0]);
  CHECK(first.audio[0].frame->data[0] == second.audio[0].frame->data[0]);
}

TEST_CASE("ProcessorSink启动失败不会配对停止回调") {
  CallbackState state;
  state.fail_start = true;
  SECTION("Analysis") {
    AnalysisProcessorSink sink("processor", AnalysisCallbacks(state));
    CHECK_THROWS_AS(sink.OnStreamsReady(Streams()), std::exception);
    sink.Stop();
    sink.Stop();
  }
  SECTION("Transform") {
    Recorded recorded;
    TransformProcessorSink sink("processor", TransformCallbacks(state));
    sink.AddSink(std::make_unique<Recorder>(recorded));
    CHECK_THROWS_AS(sink.OnStreamsReady(Streams()), std::exception);
    CHECK(recorded.events.empty());
    sink.Stop();
    sink.Stop();
    CHECK(recorded.stops == 1);
  }
  CHECK(state.starts == 1);
  CHECK(state.stops == 0);
}

TEST_CASE("TransformProcessorSink限制下游注册并传播下游异常") {
  SECTION("空下游") {
    TransformProcessorSink sink("processor", {});
    CHECK_THROWS_AS(sink.AddSink(nullptr), std::exception);
    CHECK_THROWS_AS(sink.OnStreamsReady(Streams()), std::exception);
  }
  SECTION("已启动后注册") {
    Recorded recorded;
    TransformProcessorSink sink("processor", {});
    sink.AddSink(std::make_unique<Recorder>(recorded));
    sink.OnStreamsReady(Streams());
    CHECK_THROWS_AS(sink.AddSink(std::make_unique<Recorder>(recorded)),
                    std::exception);
  }
  SECTION("停止后注册") {
    Recorded recorded;
    TransformProcessorSink sink("processor", {});
    sink.Stop();
    CHECK_THROWS_AS(sink.AddSink(std::make_unique<Recorder>(recorded)),
                    std::exception);
  }
  SECTION("下游写入失败") {
    Recorded recorded;
    recorded.throw_video = true;
    TransformProcessorSink sink("processor", {});
    sink.AddSink(std::make_unique<Recorder>(recorded));
    sink.OnStreamsReady(Streams());
    CHECK_THROWS_AS(sink.OnVideoFrame({1, Video()}), std::exception);
    sink.Stop();
    CHECK(recorded.stops == 1);
  }
}

TEST_CASE("TransformProcessorSink允许配置与媒体并发且Stop等待两者") {
  using namespace std::chrono_literals;
  struct BlockingState {
    std::promise<void> video_entered;
    std::promise<void> update_entered;
    std::shared_future<void> release_video;
    std::shared_future<void> release_update;
    std::atomic<bool> video_active = false;
    std::atomic<bool> update_active = false;
    std::atomic<bool> concurrent_update = false;
    std::atomic<bool> stop_overlapped = false;
    std::atomic<int> stop_calls = 0;
  } state;
  std::promise<void> release_video;
  std::promise<void> release_update;
  state.release_video = release_video.get_future().share();
  state.release_update = release_update.get_future().share();
  auto video_entered = state.video_entered.get_future();
  auto update_entered = state.update_entered.get_future();

  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerTransformProcessorStartRequest*,
                          void*) { return kMwStreamerProcessorStartSuccess; };
  callbacks.process_video =
      [](const MwStreamerTransformVideoProcessRequest* request, void* context) {
        auto& state = *static_cast<BlockingState*>(context);
        state.video_active.store(true);
        state.video_entered.set_value();
        state.release_video.wait();
        for (std::uint32_t plane = 0;
             plane < request->output->storage.linear.plane_count; ++plane) {
          const auto& view = request->output->storage.linear.planes[plane];
          auto* data = reinterpret_cast<std::uint8_t*>(view.address);
          for (std::uint32_t row = 0; row < view.row_count; ++row) {
            std::memset(data + row * view.stride_bytes, 0x55, view.row_bytes);
          }
        }
        state.video_active.store(false);
      };
  callbacks.on_config_update = [](const char*, void* context) {
    auto& state = *static_cast<BlockingState*>(context);
    state.update_active.store(true);
    state.concurrent_update.store(state.video_active.load());
    state.update_entered.set_value();
    state.release_update.wait();
    state.update_active.store(false);
  };
  callbacks.on_stop = [](void* context) {
    auto& state = *static_cast<BlockingState*>(context);
    state.stop_overlapped.store(state.video_active.load() ||
                                state.update_active.load());
    state.stop_calls.fetch_add(1);
  };

  Recorded recorded;
  TransformProcessorSink sink("processor", callbacks);
  sink.AddSink(std::make_unique<Recorder>(recorded));
  sink.OnStreamsReady(Streams());
  auto frame = Video();
  auto video = std::async(std::launch::async, [&]() {
    sink.OnVideoFrame({1, frame});
  });
  const auto video_started = video_entered.wait_for(2s);
  auto update = std::async(std::launch::async,
                           [&]() { sink.UpdateConfig("concurrent"); });
  const auto update_started = update_entered.wait_for(2s);
  std::promise<void> stop_entered;
  auto stop_entered_future = stop_entered.get_future();
  auto stop = std::async(std::launch::async, [&]() {
    stop_entered.set_value();
    sink.Stop();
  });
  const auto stop_started = stop_entered_future.wait_for(2s);
  const auto stop_before_release = stop.wait_for(20ms);
  release_video.set_value();
  const auto video_finished = video.wait_for(2s);
  const auto stop_while_updating = stop.wait_for(20ms);
  release_update.set_value();
  const auto update_finished = update.wait_for(2s);
  const auto stop_finished = stop.wait_for(2s);

  // Release both callbacks before assertions so a failed concurrency check
  // cannot leave async future destruction waiting on this test's own gate.
  REQUIRE(video_started == std::future_status::ready);
  REQUIRE(update_started == std::future_status::ready);
  REQUIRE(stop_started == std::future_status::ready);
  CHECK(stop_before_release == std::future_status::timeout);
  CHECK(stop_while_updating == std::future_status::timeout);
  REQUIRE(video_finished == std::future_status::ready);
  REQUIRE(update_finished == std::future_status::ready);
  REQUIRE(stop_finished == std::future_status::ready);
  CHECK_NOTHROW(video.get());
  CHECK_NOTHROW(update.get());
  CHECK_NOTHROW(stop.get());
  CHECK(state.concurrent_update.load());
  CHECK_FALSE(state.stop_overlapped.load());
  CHECK(state.stop_calls.load() == 1);
  CHECK(recorded.stops == 1);
}

TEST_CASE("AnalysisProcessorSink按音视频分别累计且读取不会清零") {
  CallbackState state;
  AnalysisProcessorSink sink("processor", AnalysisCallbacks(state));
  sink.OnStreamsReady(Streams());
  sink.OnAudioFrame({1, Audio()});
  sink.OnVideoFrame({1, Video()});
  sink.OnVideoFrame({1, Video()});
  const auto first = sink.GetPerformance();
  CHECK(first.name == "AnalysisProcessorSink");
  const auto& audio = Operation(first, PerformanceType::kAudioProcessor);
  CHECK(audio.input_unit == PerformanceUnit::kSample);
  CHECK(audio.input_count == 4);
  CHECK(audio.output_unit == PerformanceUnit::kNone);
  CHECK(audio.output_count == 0);
  CHECK(audio.started_calls == 1);
  CHECK(audio.completed_calls == 1);
  const auto& video = Operation(first, PerformanceType::kVideoProcessor);
  CHECK(video.input_unit == PerformanceUnit::kFrame);
  CHECK(video.input_count == 2);
  CHECK(video.completed_calls == 2);
  CHECK(video.in_flight == 0);
  sink.Stop();
  const auto second = sink.GetPerformance();
  const auto& after_stop = Operation(second, PerformanceType::kVideoProcessor);
  CHECK(after_stop.input_count == video.input_count);
  CHECK(after_stop.completed_calls == video.completed_calls);
  CHECK(after_stop.total_time == video.total_time);
}

TEST_CASE("ProcessorSink缺少回调不产生处理调用统计") {
  SECTION("Analysis忽略但记录输入") {
    AnalysisProcessorSink sink("processor", {});
    sink.OnStreamsReady(Streams());
    sink.OnAudioFrame({1, Audio()});
    sink.OnVideoFrame({1, Video()});
    const auto snapshot = sink.GetPerformance();
    CHECK(Operation(snapshot, PerformanceType::kAudioProcessor).input_count ==
          4);
    CHECK(Operation(snapshot, PerformanceType::kVideoProcessor).input_count ==
          1);
    for (const auto& operation : snapshot.operations) {
      CHECK(operation.started_calls == 0);
      CHECK(operation.output_count == 0);
    }
  }
  SECTION("Transform透传且输出不按下游数量重复累计") {
    Recorded first;
    Recorded second;
    TransformProcessorSink sink("processor", {});
    sink.AddSink(std::make_unique<Recorder>(first));
    sink.AddSink(std::make_unique<Recorder>(second, "second"));
    sink.OnStreamsReady(Streams());
    sink.OnAudioFrame({1, Audio()});
    sink.OnVideoFrame({1, Video()});
    const auto snapshot = sink.GetPerformance();
    CHECK(snapshot.name == "TransformProcessorSink");
    REQUIRE(snapshot.downstream.size() == 2);
    CHECK(snapshot.downstream[0].name == "Recorder");
    CHECK(snapshot.downstream[1].name == "Recorder");
    const auto& audio = Operation(snapshot, PerformanceType::kAudioProcessor);
    CHECK(audio.input_unit == PerformanceUnit::kSample);
    CHECK(audio.output_unit == PerformanceUnit::kSample);
    CHECK(audio.input_count == 4);
    CHECK(audio.output_count == 4);
    const auto& video = Operation(snapshot, PerformanceType::kVideoProcessor);
    CHECK(video.input_unit == PerformanceUnit::kFrame);
    CHECK(video.output_unit == PerformanceUnit::kFrame);
    CHECK(video.input_count == 1);
    CHECK(video.output_count == 1);
    for (const auto& operation : snapshot.operations) {
      CHECK(operation.started_calls == 0);
      CHECK(operation.total_time == std::chrono::nanoseconds::zero());
    }
  }
}

TEST_CASE("ProcessorSink记录回调异常而不把下游异常算成处理失败") {
  SECTION("Analysis回调异常") {
    MwStreamerAnalysisProcessorCallbacks callbacks{};
    callbacks.process_video = [](const MwStreamerVideoFrameView*, void*) {
      throw std::runtime_error("analysis failure");
    };
    AnalysisProcessorSink sink("processor", callbacks);
    sink.OnStreamsReady(Streams());
    CHECK_THROWS_AS(sink.OnVideoFrame({1, Video()}), std::runtime_error);
    const auto snapshot = sink.GetPerformance();
    const auto& video = Operation(snapshot, PerformanceType::kVideoProcessor);
    CHECK(video.input_count == 1);
    CHECK(video.started_calls == 1);
    CHECK(video.failed_calls == 1);
    CHECK(video.completed_calls == 1);
    CHECK(video.in_flight == 0);
  }
  SECTION("Transform回调异常") {
    MwStreamerTransformProcessorCallbacks callbacks{};
    callbacks.process_audio = [](const MwStreamerTransformAudioProcessRequest*,
                                 void*) {
      throw std::runtime_error("transform failure");
    };
    Recorded recorded;
    TransformProcessorSink sink("processor", callbacks);
    sink.AddSink(std::make_unique<Recorder>(recorded));
    sink.OnStreamsReady(Streams());
    CHECK_THROWS_AS(sink.OnAudioFrame({1, Audio()}), std::runtime_error);
    const auto snapshot = sink.GetPerformance();
    const auto& audio = Operation(snapshot, PerformanceType::kAudioProcessor);
    CHECK(audio.input_count == 4);
    CHECK(audio.output_count == 0);
    CHECK(audio.failed_calls == 1);
    CHECK(audio.completed_calls == 1);
    CHECK(audio.in_flight == 0);
  }
  SECTION("下游异常") {
    CallbackState state;
    Recorded recorded;
    recorded.throw_video = true;
    TransformProcessorSink sink("processor", TransformCallbacks(state));
    sink.AddSink(std::make_unique<Recorder>(recorded));
    sink.OnStreamsReady(Streams());
    CHECK_THROWS_AS(sink.OnVideoFrame({1, Video()}), std::runtime_error);
    const auto snapshot = sink.GetPerformance();
    const auto& video = Operation(snapshot, PerformanceType::kVideoProcessor);
    CHECK(video.output_count == 1);
    CHECK(video.completed_calls == 1);
    CHECK(video.failed_calls == 0);
  }
}

TEST_CASE("TransformProcessorSink慢下游不阻塞采集或计入自身耗时") {
  using namespace std::chrono_literals;
  CallbackState state;
  Recorded recorded;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  recorded.video_entered = &entered;
  recorded.release_video = release.get_future().share();
  TransformProcessorSink sink("processor", TransformCallbacks(state));
  sink.AddSink(std::make_unique<Recorder>(recorded));
  sink.OnStreamsReady(Streams());
  auto frame = Video();
  auto worker = std::async(std::launch::async, [&]() {
    sink.OnVideoFrame({1, frame});
  });
  const auto started = entered_future.wait_for(2s);
  auto reader = std::async(std::launch::async, [&]() {
    return std::make_pair(sink.GetPerformance(), sink.GetPerformance());
  });
  const auto read_status = reader.wait_for(2s);
  release.set_value();
  worker.get();
  const auto snapshots = reader.get();
  REQUIRE(started == std::future_status::ready);
  REQUIRE(read_status == std::future_status::ready);
  const auto& first =
      Operation(snapshots.first, PerformanceType::kVideoProcessor);
  const auto& second =
      Operation(snapshots.second, PerformanceType::kVideoProcessor);
  CHECK(first.completed_calls == 1);
  CHECK(first.in_flight == 0);
  CHECK(first.output_count == 1);
  CHECK(second.completed_calls == first.completed_calls);
  CHECK(second.total_time == first.total_time);
  const auto after = sink.GetPerformance();
  CHECK(Operation(after, PerformanceType::kVideoProcessor).total_time ==
        first.total_time);
}

TEST_CASE("AnalysisProcessorSink执行回调期间快照保留正在处理的调用") {
  using namespace std::chrono_literals;
  struct State {
    std::promise<void> entered;
    std::shared_future<void> release;
  } state;
  auto entered = state.entered.get_future();
  std::promise<void> release;
  state.release = release.get_future().share();
  MwStreamerAnalysisProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.process_video = [](const MwStreamerVideoFrameView*, void* context) {
    auto& state = *static_cast<State*>(context);
    state.entered.set_value();
    state.release.wait();
  };
  AnalysisProcessorSink sink("processor", callbacks);
  sink.OnStreamsReady(Streams());
  auto frame = Video();
  auto worker = std::async(std::launch::async, [&]() {
    sink.OnVideoFrame({1, frame});
  });
  const auto started = entered.wait_for(2s);
  auto reader =
      std::async(std::launch::async, [&]() { return sink.GetPerformance(); });
  const auto read_status = reader.wait_for(2s);
  release.set_value();
  worker.get();
  const auto snapshot = reader.get();
  REQUIRE(started == std::future_status::ready);
  REQUIRE(read_status == std::future_status::ready);
  const auto& video = Operation(snapshot, PerformanceType::kVideoProcessor);
  CHECK(video.input_count == 1);
  CHECK(video.started_calls == 1);
  CHECK(video.in_flight == 1);
  CHECK(video.completed_calls == 0);
  const auto after = sink.GetPerformance();
  const auto& completed = Operation(after, PerformanceType::kVideoProcessor);
  CHECK(completed.in_flight == 0);
  CHECK(completed.completed_calls == 1);
}

TEST_CASE("两种Processor通过通用消息入口回调并在Stop前等待消息结束") {
  using namespace std::chrono_literals;
  struct State {
    std::promise<void> entered;
    std::shared_future<void> release;
    std::atomic<bool> message_done{false};
    std::atomic<bool> stop_after_message{false};
    std::string type;
    std::string payload;
  } state;
  auto entered = state.entered.get_future();
  std::promise<void> release;
  state.release = release.get_future().share();
  const auto on_message = [](const MwStreamerMessage* message, void* context) {
    auto& state = *static_cast<State*>(context);
    state.type = message->type;
    state.payload.assign(static_cast<const char*>(message->payload),
                         message->payload_size);
    state.entered.set_value();
    state.release.wait();
    state.message_done.store(true);
  };
  const auto on_stop = [](void* context) {
    auto& state = *static_cast<State*>(context);
    state.stop_after_message.store(state.message_done.load());
  };
  Recorded recorded;
  std::unique_ptr<Sink> sink;
  SECTION("Analysis") {
    MwStreamerAnalysisProcessorCallbacks callbacks{};
    callbacks.user_context = &state;
    callbacks.on_message = on_message;
    callbacks.on_stop = on_stop;
    sink = std::make_unique<AnalysisProcessorSink>("processor", callbacks);
  }
  SECTION("Transform") {
    MwStreamerTransformProcessorCallbacks callbacks{};
    callbacks.user_context = &state;
    callbacks.on_message = on_message;
    callbacks.on_stop = on_stop;
    auto transform =
        std::make_unique<TransformProcessorSink>("processor", callbacks);
    transform->AddSink(std::make_unique<Recorder>(recorded));
    sink = std::move(transform);
  }
  auto sender = std::make_unique<Recorder>(recorded, "sender");
  auto* sender_ptr = sender.get();
  auto bridge = std::make_unique<FrameBridge>();
  bridge->AddSink(std::move(sink));
  bridge->AddSink(std::move(sender));
  Pipeline pipeline(std::make_unique<MessageInput>());
  pipeline.AddSink(std::move(bridge));
  pipeline.SetMessageReceiver("sender", "processor");
  const SinkMessage message{"child", "feedback", "data", 4, std::nullopt};
  sender_ptr->EmitMessage(message);
  CHECK(entered.wait_for(0ms) == std::future_status::timeout);
  pipeline.Start();
  sender_ptr->EmitMessage(message);
  const auto entered_status = entered.wait_for(2s);
  auto stopping = std::async(std::launch::async, [&]() { pipeline.Stop(); });
  const auto stop_status = stopping.wait_for(20ms);
  release.set_value();
  stopping.get();
  REQUIRE(entered_status == std::future_status::ready);
  CHECK(stop_status == std::future_status::timeout);
  CHECK(state.type == "feedback");
  CHECK(state.payload == "data");
  CHECK(state.stop_after_message.load());
  sender_ptr->EmitMessage(message);
}

TEST_CASE("下游显式绑定Processor后消息直接送达而不经过中间Processor") {
  using namespace std::chrono_literals;
  std::promise<std::string> received;
  auto result = received.get_future();
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.user_context = &received;
  callbacks.on_message = [](const MwStreamerMessage* message, void* context) {
    static_cast<std::promise<std::string>*>(context)->set_value(message->type);
  };
  Recorded recorded;
  auto root = std::make_unique<TransformProcessorSink>("processor", callbacks);
  auto intermediate = std::make_unique<TransformProcessorSink>(
      "intermediate", MwStreamerTransformProcessorCallbacks{});
  auto sender = std::make_unique<Recorder>(recorded, "sender");
  auto* sender_ptr = sender.get();
  intermediate->AddSink(std::move(sender));
  root->AddSink(std::move(intermediate));
  auto bridge = std::make_unique<FrameBridge>();
  bridge->AddSink(std::move(root));
  Pipeline pipeline(std::make_unique<MessageInput>());
  pipeline.AddSink(std::move(bridge));
  pipeline.SetMessageReceiver("sender", "processor");
  pipeline.Start();
  sender_ptr->EmitMessage({"child", "feedback", nullptr, 0, std::nullopt});
  const auto status = result.wait_for(2s);
  pipeline.Stop();
  REQUIRE(status == std::future_status::ready);
  CHECK(result.get() == "feedback");
}
