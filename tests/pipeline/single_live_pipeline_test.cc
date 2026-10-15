#include "mw/streamer/pipeline/single_live_pipeline.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using namespace mw::streamer;
namespace fs = std::filesystem;
using Runtime = std::unique_ptr<MwStreamerContext, decltype(&Shutdown)>;

Runtime InitRuntime() {
  InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console.enabled = false;
  return Runtime(Init(config), &Shutdown);
}

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

class MediaDirectory final {
 public:
  MediaDirectory()
      : path_(fs::temp_directory_path() /
              ("mw-single-live-pipeline-test-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    fs::create_directories(path_);
    // A tenth of a second of stereo PCM, without an external ffmpeg process.
    constexpr std::uint32_t kDataSize = 4800 * 2 * 2;
    std::ofstream file(File("input.wav"), std::ios::binary);
    const auto write_integer = [&](std::uint32_t value, int bytes) {
      for (int index = 0; index < bytes; ++index) {
        file.put(static_cast<char>((value >> (index * 8)) & 0xff));
      }
    };
    file.write("RIFF", 4);
    write_integer(36 + kDataSize, 4);
    file.write("WAVEfmt ", 8);
    write_integer(16, 4);
    write_integer(1, 2);
    write_integer(2, 2);
    write_integer(48000, 4);
    write_integer(48000 * 4, 4);
    write_integer(4, 2);
    write_integer(16, 2);
    file.write("data", 4);
    write_integer(kDataSize, 4);
    const std::array<char, kDataSize> silence{};
    file.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    if (!file) throw std::runtime_error("写入管线测试 WAV 失败");
  }
  ~MediaDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  std::string File(const std::string& name) const {
    return (path_ / name).generic_u8string();
  }

 private:
  fs::path path_;
};

SingleLivePipelineConfig AudioConfig() {
  SingleLivePipelineConfig config;
  config.input.max_retries = 0;
  config.output.video_pixel_format = AV_PIX_FMT_NONE;
  return config;
}

bool HasAudioPackets(const std::string& path) {
  AVFormatContext* context = nullptr;
  if (avformat_open_input(&context, path.c_str(), nullptr, nullptr) < 0)
    return false;
  struct Closer {
    void operator()(AVFormatContext* input) const {
      avformat_close_input(&input);
    }
  };
  std::unique_ptr<AVFormatContext, Closer> input(context);
  if (avformat_find_stream_info(context, nullptr) < 0) return false;
  const int audio =
      av_find_best_stream(context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (audio < 0) return false;
  ffmpeg::Packet packet;
  while (av_read_frame(context, packet.get()) >= 0) {
    if (packet->stream_index == audio && packet->size > 0) return true;
    packet.Unref();
  }
  return false;
}

TEST_CASE("SingleLivePipeline pairs business callbacks across restarts",
          "[pipeline][lifecycle]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  int initialized = 0;
  int stopped = 0;
  std::atomic<int> frames{0};
  std::atomic<bool> callback_after_stop{false};
  std::atomic<bool> business_active{false};
  SingleLivePipeline pipeline(device, AudioConfig());
  pipeline.SetOnInitialize([&] {
    ++initialized;
    business_active.store(true);
  });
  pipeline.SetOnAudio([&](const ffmpeg::Frame& frame) {
    if (!business_active.load()) callback_after_stop.store(true);
    ++frames;
    return frame.Ref();
  });
  pipeline.SetOnStop([&] {
    business_active.store(false);
    ++stopped;
  });

  CHECK(pipeline.state() == SingleLivePipelineState::kStopped);
  CHECK(pipeline.sink_status().state == FrameSinkState::kWaiting);
  CHECK(pipeline.error() == nullptr);
  pipeline.Stop();
  CHECK(stopped == 0);
  for (int session = 1; session <= 2; ++session) {
    frames.store(0);
    REQUIRE(pipeline.Start(files.File("input.wav")));
    CHECK(initialized == session);
    CHECK_FALSE(pipeline.Start(files.File("input.wav")));
    CHECK(initialized == session);
    CHECK(pipeline.state() == SingleLivePipelineState::kRunning);
    REQUIRE(WaitFor([&] { return frames.load() > 0; }));
    CHECK(pipeline.sink_status().state == FrameSinkState::kRunning);
    pipeline.RequestStop();
    pipeline.Stop();
    CHECK(pipeline.state() == SingleLivePipelineState::kStopped);
    CHECK(stopped == session);
    pipeline.Stop();
    CHECK(stopped == session);
  }
  CHECK_FALSE(callback_after_stop.load());
}

TEST_CASE("SingleLivePipeline wraps video processing as one source frame",
          "[pipeline][video]") {
  auto runtime = InitRuntime();
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  auto config = AudioConfig();
  config.output.width = 64;
  config.output.height = 64;
  config.output.video_pixel_format = AV_PIX_FMT_YUV420P;
  config.output.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.output.video_options = {{"preset", "ultrafast"},
                                 {"tune", "zerolatency"}};
  std::atomic<int> frames{0};
  std::atomic<bool> valid{true};
  SingleLivePipeline pipeline(device, config);
  pipeline.SetOnVideo([&](const ffmpeg::Frame& frame) {
    if (frame->width != 64 || frame->height != 64 ||
        frame->format != AV_PIX_FMT_YUV420P || !frame->data[0]) {
      valid.store(false);
    }
    ++frames;
    return frame.Ref();
  });
  REQUIRE(pipeline.Start(std::string(MW_SINGLE_LIVE_PIPELINE_TEST_DATA_DIR) +
                         "/h264_video.mp4"));
  REQUIRE(WaitFor([&] { return frames.load() >= 3; }));
  pipeline.Stop();
  CHECK(valid.load());
  CHECK(pipeline.error() == nullptr);
}

TEST_CASE("SingleLivePipeline notifies Ended once and continues output",
          "[pipeline][ended]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  std::atomic<int> ended{0};
  std::atomic<int> frames{0};
  std::atomic<bool> status_visible{false};
  SingleLivePipeline pipeline(device, AudioConfig());
  pipeline.SetOnAudio([&](const ffmpeg::Frame& frame) {
    ++frames;
    return frame.Ref();
  });
  pipeline.SetOnEnded([&] {
    status_visible.store(pipeline.input_status().state ==
                         AvMediaInputState::kEnded);
    ++ended;
  });
  REQUIRE(pipeline.Start(files.File("input.wav")));
  REQUIRE(WaitFor([&] { return ended.load() == 1; }));
  CHECK(status_visible.load());
  CHECK(pipeline.state() == SingleLivePipelineState::kRunning);
  // The business callback receives source audio only; after EOF the scheduler
  // keeps producing silence without invoking it.
  std::this_thread::sleep_for(100ms);
  CHECK(pipeline.state() == SingleLivePipelineState::kRunning);
  CHECK(pipeline.sink_status().state == FrameSinkState::kRunning);
  CHECK(frames.load() > 0);
  CHECK(ended.load() == 1);
  pipeline.Stop();
  CHECK(ended.load() == 1);
}

TEST_CASE("SingleLivePipeline loop boundaries do not notify Ended",
          "[pipeline][loop]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  auto config = AudioConfig();
  config.input.loop = true;
  std::atomic<int> ended{0};
  SingleLivePipeline pipeline(device, config);
  pipeline.SetOnEnded([&] { ++ended; });
  REQUIRE(pipeline.Start(files.File("input.wav")));
  REQUIRE(WaitFor([&] { return pipeline.input_status().generation >= 2; }));
  CHECK(pipeline.state() == SingleLivePipelineState::kRunning);
  CHECK(pipeline.input_status().state != AvMediaInputState::kEnded);
  pipeline.Stop();
  CHECK(ended.load() == 0);
}

TEST_CASE("SingleLivePipeline retains first terminal input error until restart",
          "[pipeline][error]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  std::atomic<int> errors{0};
  std::atomic<int> ended{0};
  std::atomic<bool> error_visible{false};
  SingleLivePipeline pipeline(device, AudioConfig());
  pipeline.SetOnError([&](std::exception_ptr error) {
    error_visible.store(error && pipeline.error() == error &&
                        pipeline.state() == SingleLivePipelineState::kFailed);
    ++errors;
    pipeline.RequestStop();
  });
  pipeline.SetOnEnded([&] { ++ended; });
  REQUIRE(pipeline.Start(files.File("missing.wav")));
  REQUIRE(WaitFor([&] { return errors.load() == 1; }));
  const auto error = pipeline.error();
  CHECK(error_visible.load());
  REQUIRE(error != nullptr);
  CHECK_FALSE(pipeline.Start(files.File("input.wav")));
  pipeline.Stop();
  CHECK(pipeline.state() == SingleLivePipelineState::kFailed);
  CHECK(pipeline.error() == error);
  CHECK(errors.load() == 1);
  CHECK(ended.load() == 0);
  REQUIRE(pipeline.Start(files.File("input.wav")));
  CHECK(pipeline.error() == nullptr);
  pipeline.Stop();
  CHECK(pipeline.state() == SingleLivePipelineState::kStopped);
}

TEST_CASE("SingleLivePipeline sink failure marks both pipeline and sink failed",
          "[pipeline][error][sink]") {
  auto runtime = InitRuntime();
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  auto config = AudioConfig();
  config.output.width = 64;
  config.output.height = 64;
  config.output.video_pixel_format = AV_PIX_FMT_YUV420P;
  config.output.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.output.video_options = {{"preset", "ultrafast"},
                                 {"tune", "zerolatency"}};
  std::atomic<int> errors{0};
  std::atomic<bool> state_visible{false};
  int stopped = 0;
  SingleLivePipeline pipeline(device, config);
  pipeline.SetOnStop([&] { ++stopped; });
  pipeline.SetOnVideo([](const ffmpeg::Frame& frame) {
    auto result = frame.Ref();
    // This violates the encoder layout contract on its asynchronous worker.
    result->width += 2;
    return result;
  });
  pipeline.SetOnError([&](std::exception_ptr error) {
    const auto sink = pipeline.sink_status();
    state_visible.store(pipeline.state() == SingleLivePipelineState::kFailed &&
                        sink.state == FrameSinkState::kFailed &&
                        sink.error == error && pipeline.error() == error);
    ++errors;
  });
  REQUIRE(pipeline.Start(std::string(MW_SINGLE_LIVE_PIPELINE_TEST_DATA_DIR) +
                         "/h264_video.mp4"));
  REQUIRE(WaitFor([&] { return errors.load() == 1; }));
  CHECK(state_visible.load());
  const auto error = pipeline.error();
  REQUIRE(error != nullptr);
  pipeline.Stop();
  CHECK(errors.load() == 1);
  CHECK(pipeline.state() == SingleLivePipelineState::kFailed);
  CHECK(pipeline.sink_status().state == FrameSinkState::kFailed);
  CHECK(pipeline.sink_status().error == error);
  CHECK(pipeline.error() == error);
  CHECK(stopped == 1);
  pipeline.Stop();
  CHECK(stopped == 1);
}

TEST_CASE("SingleLivePipeline initialization failure rolls back business work",
          "[pipeline][initialization]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  int initialized = 0;
  int stopped = 0;
  int errors = 0;
  auto config = AudioConfig();
  SECTION("business initialization throws before success") {
    SingleLivePipeline pipeline(device, config);
    const auto original =
        std::make_exception_ptr(std::runtime_error("业务初始化失败"));
    pipeline.SetOnInitialize([&] {
      ++initialized;
      std::rethrow_exception(original);
    });
    pipeline.SetOnStop([&] { ++stopped; });
    pipeline.SetOnError([&](std::exception_ptr) { ++errors; });
    CHECK_FALSE(pipeline.Start(files.File("input.wav")));
    CHECK(pipeline.error() == original);
    CHECK(pipeline.state() == SingleLivePipelineState::kFailed);
    pipeline.Stop();
    CHECK(initialized == 1);
    CHECK(stopped == 0);
    CHECK(errors == 0);
    pipeline.SetOnInitialize([&] { ++initialized; });
    REQUIRE(pipeline.Start(files.File("input.wav")));
    pipeline.Stop();
    CHECK(initialized == 2);
    CHECK(stopped == 1);
  }
  SECTION("encoder initialization fails after business initialization") {
    config.output.audio_encoder_name = "mw_nonexistent_audio_encoder";
    SingleLivePipeline pipeline(device, config);
    pipeline.SetOnInitialize([&] { ++initialized; });
    pipeline.SetOnStop([&] { ++stopped; });
    pipeline.SetOnError([&](std::exception_ptr) { ++errors; });
    CHECK_FALSE(pipeline.Start(files.File("input.wav")));
    CHECK(pipeline.error() != nullptr);
    CHECK(initialized == 1);
    CHECK(stopped == 1);
    CHECK(errors == 0);
    pipeline.Stop();
    CHECK(stopped == 1);
  }
}

TEST_CASE("SingleLivePipeline restart retains static and dynamic recordings",
          "[pipeline][recording]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  std::atomic<int> frames{0};
  auto config = AudioConfig();
  config.input.loop = true;
  SingleLivePipeline pipeline(device, config);
  pipeline.SetOnAudio([&](const ffmpeg::Frame& frame) {
    ++frames;
    return frame.Ref();
  });
  const auto first_path = pipeline.AddPushUrl(files.File("static.mp4"));
  REQUIRE(pipeline.Start(files.File("input.wav")));
  REQUIRE(WaitFor([&] { return frames.load() >= 3; }));
  const auto dynamic_path = pipeline.AddPushUrl(files.File("dynamic.mp4"));
  const int before = frames.load();
  REQUIRE(WaitFor([&] { return frames.load() >= before + 5; }));
  pipeline.Stop();
  CHECK(HasAudioPackets(first_path));
  CHECK(HasAudioPackets(dynamic_path));

  frames.store(0);
  REQUIRE(pipeline.Start(files.File("input.wav")));
  const auto outputs = pipeline.output_info().output_urls;
  REQUIRE(outputs.size() == 2);
  CHECK(outputs[0] != first_path);
  CHECK(outputs[1] != dynamic_path);
  REQUIRE(WaitFor([&] { return frames.load() >= 5; }));
  pipeline.Stop();
  CHECK(HasAudioPackets(outputs[0]));
  CHECK(HasAudioPackets(outputs[1]));
}

TEST_CASE(
    "SingleLivePipeline supports concurrent queries across teardown and "
    "restart",
    "[pipeline][concurrency]") {
  auto runtime = InitRuntime();
  MediaDirectory files;
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  auto config = AudioConfig();
  config.input.loop = true;
  std::atomic<int> frames{0};
  std::atomic<int> queries{0};
  std::atomic<bool> finished{false};
  std::atomic<bool> query_failed{false};
  int stopped = 0;
  bool drained_before_business_stop = true;
  SingleLivePipeline pipeline(device, config);
  pipeline.AddPushUrl(files.File("concurrent.mp4"));
  pipeline.SetOnAudio([&](const ffmpeg::Frame& frame) {
    ++frames;
    return frame.Ref();
  });
  pipeline.SetOnStop([&] {
    ++stopped;
    const auto outputs = pipeline.output_info().output_urls;
    drained_before_business_stop &=
        outputs.size() == 1 && HasAudioPackets(outputs.front());
  });
  std::thread observer([&] {
    try {
      while (!finished.load()) {
        static_cast<void>(pipeline.state());
        static_cast<void>(pipeline.input_status());
        static_cast<void>(pipeline.sink_status());
        static_cast<void>(pipeline.output_info());
        static_cast<void>(pipeline.push_statuses());
        ++queries;
        std::this_thread::yield();
      }
    } catch (...) {
      query_failed.store(true);
    }
  });
  // Avoid fatal assertions while the observer needs to be joined.
  std::vector<bool> starts;
  std::vector<bool> produced;
  for (int session = 0; session < 3; ++session) {
    frames.store(0);
    const bool started = pipeline.Start(files.File("input.wav"));
    starts.push_back(started);
    produced.push_back(started && WaitFor([&] { return frames.load() >= 3; }));
    pipeline.Stop();
  }
  finished.store(true);
  observer.join();
  CHECK(starts == std::vector<bool>{true, true, true});
  CHECK(produced == std::vector<bool>{true, true, true});
  CHECK(stopped == 3);
  CHECK(queries.load() > 0);
  CHECK_FALSE(query_failed.load());
  CHECK(drained_before_business_stop);
}

struct InputFormatCloser {
  void operator()(AVFormatContext* input) const {
    avformat_close_input(&input);
  }
};

struct OutputFormatCloser {
  void operator()(AVFormatContext* output) const {
    if (output->pb) avio_closep(&output->pb);
    avformat_free_context(output);
  }
};

// Combine existing 224x100 video and AAC fixtures without a subprocess or
// transcoding. The larger picture satisfies NVENC's minimum dimensions.
void CreateCudaAudioVideoInput(const std::string& path) {
  AVFormatContext* raw_output = nullptr;
  REQUIRE(avformat_alloc_output_context2(&raw_output, nullptr, "mp4",
                                         path.c_str()) >= 0);
  std::unique_ptr<AVFormatContext, OutputFormatCloser> output(raw_output);
  const std::array<std::string, 2> names{"h264_cuda_dpb.mp4", "h264_aac.mp4"};
  std::array<std::unique_ptr<AVFormatContext, InputFormatCloser>, 2> inputs;
  std::array<int, 2> selected{};
  for (std::size_t track = 0; track < inputs.size(); ++track) {
    const auto source =
        std::string(MW_SINGLE_LIVE_PIPELINE_TEST_DATA_DIR) + "/" + names[track];
    AVFormatContext* raw_input = nullptr;
    REQUIRE(avformat_open_input(&raw_input, source.c_str(), nullptr, nullptr) >=
            0);
    inputs[track].reset(raw_input);
    REQUIRE(avformat_find_stream_info(raw_input, nullptr) >= 0);
    selected[track] = av_find_best_stream(
        raw_input, track == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO, -1, -1,
        nullptr, 0);
    REQUIRE(selected[track] >= 0);
    auto* stream = avformat_new_stream(output.get(), nullptr);
    REQUIRE(stream != nullptr);
    REQUIRE(avcodec_parameters_copy(
                stream->codecpar,
                raw_input->streams[selected[track]]->codecpar) >= 0);
    stream->codecpar->codec_tag = 0;
    stream->time_base = raw_input->streams[selected[track]]->time_base;
  }
  REQUIRE(avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
  REQUIRE(avformat_write_header(output.get(), nullptr) >= 0);
  for (std::size_t track = 0; track < inputs.size(); ++track) {
    ffmpeg::Packet packet;
    int result = 0;
    while ((result = av_read_frame(inputs[track].get(), packet.get())) >= 0) {
      if (packet->stream_index == selected[track]) {
        av_packet_rescale_ts(packet.get(),
                             inputs[track]->streams[selected[track]]->time_base,
                             output->streams[track]->time_base);
        packet->stream_index = static_cast<int>(track);
        packet->pos = -1;
        REQUIRE(av_interleaved_write_frame(output.get(), packet.get()) >= 0);
      }
      packet.Unref();
    }
    REQUIRE(result == AVERROR_EOF);
  }
  REQUIRE(av_write_trailer(output.get()) >= 0);
}

void VerifyCudaRecording(const std::string& path, AVCodecID video_codec) {
  AVFormatContext* raw_input = nullptr;
  REQUIRE(avformat_open_input(&raw_input, path.c_str(), nullptr, nullptr) >= 0);
  std::unique_ptr<AVFormatContext, InputFormatCloser> input(raw_input);
  REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
  REQUIRE(input->nb_streams == 2);
  std::array<std::unique_ptr<ffmpeg::CodecContext>, 2> codecs;
  std::array<int, 2> indexes{};
  std::array<int, 2> decoded{};
  for (std::size_t track = 0; track < codecs.size(); ++track) {
    indexes[track] = av_find_best_stream(
        input.get(), track == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO, -1,
        -1, nullptr, 0);
    REQUIRE(indexes[track] >= 0);
    const auto* parameters = input->streams[indexes[track]]->codecpar;
    CHECK(parameters->codec_id == (track == 0 ? video_codec : AV_CODEC_ID_AAC));
    const auto* decoder = avcodec_find_decoder(parameters->codec_id);
    REQUIRE(decoder != nullptr);
    codecs[track] = std::make_unique<ffmpeg::CodecContext>(decoder);
    REQUIRE(avcodec_parameters_to_context(codecs[track]->get(), parameters) >=
            0);
    REQUIRE(avcodec_open2(codecs[track]->get(), decoder, nullptr) >= 0);
  }
  const auto receive = [&](std::size_t track) {
    ffmpeg::Frame frame;
    for (;;) {
      const int result =
          avcodec_receive_frame(codecs[track]->get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      CHECK(frame->hw_frames_ctx == nullptr);
      REQUIRE(frame->data[0] != nullptr);
      if (track == 0) {
        CHECK(frame->width == 224);
        CHECK(frame->height == 100);
      } else {
        CHECK(frame->sample_rate == 48000);
        CHECK(frame->ch_layout.nb_channels == 2);
      }
      ++decoded[track];
      frame.Unref();
    }
  };
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
    for (std::size_t track = 0; track < codecs.size(); ++track) {
      if (packet->stream_index != indexes[track]) continue;
      REQUIRE(avcodec_send_packet(codecs[track]->get(), packet.get()) >= 0);
      receive(track);
    }
    packet.Unref();
  }
  REQUIRE(result == AVERROR_EOF);
  for (std::size_t track = 0; track < codecs.size(); ++track) {
    REQUIRE(avcodec_send_packet(codecs[track]->get(), nullptr) >= 0);
    receive(track);
    CHECK(decoded[track] > 0);
  }
}

TEST_CASE("SingleLivePipeline shares CUDA device through NVENC and restart",
          "[.][pipeline][cuda]") {
  const char* encoder = "h264_nvenc";
  AVCodecID video_codec = AV_CODEC_ID_H264;
  SECTION("H264 NVENC") {}
  SECTION("HEVC NVENC") {
    encoder = "hevc_nvenc";
    video_codec = AV_CODEC_ID_HEVC;
  }
  auto runtime = InitRuntime();
  MediaDirectory files;
  const auto source = files.File("cuda_audio_video.mp4");
  CreateCudaAudioVideoInput(source);
  auto device =
      std::make_unique<ffmpeg::HwDeviceContext>(ffmpeg::HwDeviceType::kCuda);
  REQUIRE(device->get() != nullptr);
  const auto* native_device =
      reinterpret_cast<const AVHWDeviceContext*>(device->get()->data);
  auto config = AudioConfig();
  config.input.loop = true;
  config.output.width = 224;
  config.output.height = 100;
  config.output.video_pixel_format = AV_PIX_FMT_NV12;
  config.output.video_encoder_name = encoder;
  config.output.video_options = {{"delay", "0"}};
  std::atomic<int> video_frames{0};
  std::atomic<int> audio_frames{0};
  std::atomic<bool> valid{true};
  std::vector<ffmpeg::Frame> retained;
  SingleLivePipeline pipeline(*device, config);
  // Only the pipeline/components and returned frames now own device refs.
  device.reset();
  pipeline.SetOnVideo([&](const ffmpeg::Frame& frame) {
    if (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx) {
      valid.store(false);
    } else {
      const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
          frame->hw_frames_ctx->data);
      if (pool->device_ctx != native_device || !pool->device_ref ||
          pool->device_ref->data !=
              reinterpret_cast<const std::uint8_t*>(native_device)) {
        valid.store(false);
      }
      ffmpeg::Frame downloaded;
      if (av_hwframe_transfer_data(downloaded.get(), frame.get(), 0) < 0 ||
          !downloaded->data[0] || downloaded->format != AV_PIX_FMT_NV12 ||
          downloaded->width != 224 || downloaded->height != 100) {
        valid.store(false);
      }
    }
    if (video_frames.load() == 0) retained.push_back(frame.Ref());
    ++video_frames;
    return frame.Ref();
  });
  pipeline.SetOnAudio([&](const ffmpeg::Frame& frame) {
    if (frame->hw_frames_ctx || frame->format != AV_SAMPLE_FMT_FLTP ||
        frame->sample_rate != 48000 || frame->ch_layout.nb_channels != 2) {
      valid.store(false);
    }
    ++audio_frames;
    return frame.Ref();
  });
  const auto planned = pipeline.AddPushUrl(files.File("nvenc.mp4"));
  std::string previous;
  for (int session = 0; session < 2; ++session) {
    CAPTURE(encoder, session);
    video_frames.store(0);
    audio_frames.store(0);
    REQUIRE(pipeline.Start(source));
    const auto outputs = pipeline.output_info().output_urls;
    REQUIRE(outputs.size() == 1);
    if (session == 0) {
      CHECK(outputs.front() == planned);
    } else {
      CHECK(outputs.front() != previous);
    }
    REQUIRE(WaitFor(
        [&] { return video_frames.load() >= 5 && audio_frames.load() >= 5; }));
    pipeline.Stop();
    CHECK(pipeline.error() == nullptr);
    REQUIRE(valid.load());
    REQUIRE(retained.size() == static_cast<std::size_t>(session + 1));
    const auto& frame = retained.back();
    REQUIRE(frame->format == AV_PIX_FMT_CUDA);
    REQUIRE(frame->hw_frames_ctx != nullptr);
    const auto* pool =
        reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    CHECK(pool->device_ctx == native_device);
    ffmpeg::Frame downloaded;
    REQUIRE(av_hwframe_transfer_data(downloaded.get(), frame.get(), 0) >= 0);
    CHECK(downloaded->width == 224);
    CHECK(downloaded->height == 100);
    CHECK(downloaded->data[0] != nullptr);
    VerifyCudaRecording(outputs.front(), video_codec);
    previous = outputs.front();
  }
}

}  // namespace
