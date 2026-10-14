#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/output/encode_push_sink.h"
#include "mw/streamer/output/output.h"

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

AvEncoderConfig VideoConfig() {
  AvEncoderConfig config;
  config.width = 64;
  config.height = 64;
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.video_options = {{"preset", "ultrafast"}, {"tune", "zerolatency"}};
  return config;
}

OutputFrame VideoFrame(int width = 64) {
  OutputFrame output;
  output.frame.video.emplace();
  auto& frame = *output.frame.video;
  frame->format = AV_PIX_FMT_NV12;
  frame->width = width;
  frame->height = 64;
  frame->pts = 90000;
  frame->time_base = {1, 90000};
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 Sink 测试视频帧");
  for (int plane = 0; plane < 2; ++plane) {
    const int rows = plane == 0 ? 64 : 32;
    for (int row = 0; row < rows; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane],
                  plane == 0 ? 80 : 128, static_cast<std::size_t>(width));
    }
  }
  return output;
}

OutputFrame AudioVideoFrame() {
  auto output = VideoFrame();
  output.frame.audio.emplace();
  auto& frame = *output.frame.audio;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = 1600;
  frame->pts = 45000;
  frame->time_base = {1, 90000};
  av_channel_layout_default(&frame->ch_layout, 2);
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 Sink 测试音频帧");
  for (int channel = 0; channel < 2; ++channel) {
    std::memset(frame->extended_data[channel], 0,
                static_cast<std::size_t>(frame->nb_samples) * sizeof(float));
  }
  output.audio_samples = frame->nb_samples;
  return output;
}

class OutputDirectory final {
 public:
  OutputDirectory()
      : root_(fs::absolute(fs::temp_directory_path()).lexically_normal()),
        path_(root_ / ("mw-encode-push-sink-test-" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()))) {
    fs::create_directories(path_);
  }
  ~OutputDirectory() {
    if (path_.parent_path() == root_) {
      std::error_code error;
      fs::remove_all(path_, error);
    }
  }
  std::string File(const std::string& name) const {
    return (path_ / name).generic_u8string();
  }

 private:
  fs::path root_;
  fs::path path_;
};

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::size_t DecodeFrames(const std::string& path, AVMediaType type,
                         unsigned int expected_streams) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开 Sink 录制媒体");
  std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(input.get(), nullptr), "探测 Sink 录制轨道");
  REQUIRE(input->nb_streams == expected_streams);
  const int index = av_find_best_stream(input.get(), type, -1, -1, nullptr, 0);
  REQUIRE(index >= 0);
  const auto* stream = input->streams[index];
  const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  REQUIRE(decoder != nullptr);
  ffmpeg::CodecContext codec(decoder);
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_to_context(codec.get(), stream->codecpar),
      "导入 Sink 录制解码参数");
  ffmpeg::FfmpegException::throwIfError(
      avcodec_open2(codec.get(), decoder, nullptr), "打开 Sink 录制解码器");
  ffmpeg::Frame frame;
  std::size_t decoded = 0;
  const auto receive = [&] {
    for (;;) {
      const int status = avcodec_receive_frame(codec.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
      ffmpeg::FfmpegException::throwIfError(status, "解码 Sink 录制帧");
      if (type == AVMEDIA_TYPE_VIDEO) {
        CHECK(frame->width == 64);
        CHECK(frame->height == 64);
      } else {
        CHECK(frame->sample_rate == 48000);
        CHECK(frame->ch_layout.nb_channels == 2);
      }
      ++decoded;
      frame.Unref();
    }
  };
  ffmpeg::Packet packet;
  int status = 0;
  while ((status = av_read_frame(input.get(), packet.get())) >= 0) {
    if (packet->stream_index == index) {
      ffmpeg::FfmpegException::throwIfError(
          avcodec_send_packet(codec.get(), packet.get()), "提交 Sink 录制包");
      receive();
    }
    packet.Unref();
  }
  REQUIRE(status == AVERROR_EOF);
  ffmpeg::FfmpegException::throwIfError(
      avcodec_send_packet(codec.get(), nullptr), "排空 Sink 录制解码器");
  receive();
  return decoded;
}

struct Errors final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::exception_ptr> values;

  void Add(std::exception_ptr error) {
    std::lock_guard lock(mutex);
    values.push_back(error);
    changed.notify_all();
  }
  std::exception_ptr Wait() {
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, 10s, [&] { return !values.empty(); })) {
      return nullptr;
    }
    return values.front();
  }
  std::size_t Count() {
    std::lock_guard lock(mutex);
    return values.size();
  }
};

std::uint16_t ReservePort() {
#if defined(_WIN32)
  WSADATA data;
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
  struct Socket final {
#if defined(_WIN32)
    SOCKET handle = INVALID_SOCKET;
    ~Socket() {
      closesocket(handle);
      WSACleanup();
    }
#else
    int handle = -1;
    ~Socket() { close(handle); }
#endif
  };
  Socket socket;
  socket.handle = ::socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32)
  REQUIRE(socket.handle != INVALID_SOCKET);
#else
  REQUIRE(socket.handle >= 0);
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(bind(socket.handle, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == 0);
#if defined(_WIN32)
  int size = sizeof(address);
#else
  socklen_t size = sizeof(address);
#endif
  REQUIRE(getsockname(socket.handle, reinterpret_cast<sockaddr*>(&address),
                      &size) == 0);
  return ntohs(address.sin_port);
}

}  // namespace

TEST_CASE("EncodePushSink 未初始化时立即拒绝帧且不提供同步时钟",
          "[output][encode_push_sink][lifecycle]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  EncodePushSink sink(device);
  CHECK_FALSE(sink.has_synchronization_clock());
  auto result = sink.Send(VideoFrame());
  REQUIRE(result.valid());
  REQUIRE(result.wait_for(0s) == std::future_status::ready);
  CHECK_FALSE(result.get());
}

TEST_CASE("EncodePushSink 接收 future 即时完成且析构排空延迟音视频包",
          "[output][encode_push_sink][recording][integration]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  OutputDirectory directory;
  Errors errors;
  std::string path;
  {
    EncodePushSink sink(device);
    sink.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
    path = sink.AddPushUrl(directory.File("delayed.mp4"));
    auto config = VideoConfig();
    config.audio_sample_format = AV_SAMPLE_FMT_FLTP;
    config.max_b_frames = 2;
    config.video_options = {{"preset", "ultrafast"}};
    REQUIRE(sink.Initialize(config));
    auto result = sink.Send(AudioVideoFrame());
    REQUIRE(result.wait_for(0s) == std::future_status::ready);
    REQUIRE(result.get());
    CHECK(sink.output_info().output_urls == std::vector<std::string>{path});
  }
  CHECK(errors.Count() == 0);
  REQUIRE(fs::exists(fs::u8path(path)));
  CHECK(DecodeFrames(path, AVMEDIA_TYPE_VIDEO, 2) == 1);
  CHECK(DecodeFrames(path, AVMEDIA_TYPE_AUDIO, 2) > 0);
}

TEST_CASE("EncodePushSink 两阶段初始化失败回滚并保留输出供重试",
          "[output][encode_push_sink][failure][lifecycle]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  OutputDirectory directory;
  Errors errors;
  std::string path;
  {
    EncodePushSink sink(device);
    sink.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
    path = sink.AddPushUrl(directory.File("retry.mp4"));
    auto invalid = VideoConfig();
    SECTION("编码器启动失败") { invalid.width = 0; }
    SECTION("编码器支持但 Remuxer 不支持的 MPEG4 轨道") {
      invalid.video_encoder_name = "mpeg4";
      invalid.video_pixel_format = AV_PIX_FMT_YUV420P;
      invalid.video_options.clear();
    }
    CHECK_FALSE(sink.Initialize(invalid));
    CHECK_FALSE(sink.Send(VideoFrame()).get());
    CHECK(errors.Count() == 0);
    CHECK(sink.output_info().output_urls == std::vector<std::string>{path});
    REQUIRE(sink.Initialize(VideoConfig()));
    REQUIRE(sink.Send(VideoFrame()).get());
  }
  CHECK(errors.Count() == 0);
  CHECK(DecodeFrames(path, AVMEDIA_TYPE_VIDEO, 1) == 1);
}

TEST_CASE("EncodePushSink 重复初始化失败不会破坏运行中的编码输出",
          "[output][encode_push_sink][lifecycle]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  OutputDirectory directory;
  std::string path;
  {
    EncodePushSink sink(device);
    path = sink.AddPushUrl(directory.File("repeated.mp4"));
    REQUIRE(sink.Initialize(VideoConfig()));
    CHECK_FALSE(sink.Initialize(VideoConfig()));
    CHECK_FALSE(sink.Initialize({}));
    REQUIRE(sink.Send(VideoFrame()).get());
  }
  CHECK(DecodeFrames(path, AVMEDIA_TYPE_VIDEO, 1) == 1);
}

TEST_CASE("EncodePushSink 运行中动态注册录制和本地发布",
          "[output][encode_push_sink][outputs][integration]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  OutputDirectory directory;
  std::string path;
  const auto rtsp_port = ReservePort();
  {
    EncodePushSink sink(device);
    REQUIRE(sink.Initialize(VideoConfig()));
    path = sink.AddPushUrl(directory.File("dynamic.mp4"));
    CHECK(sink.AddPushUrl(directory.File("dynamic.mp4")) == path);
    sink.AddRtspPublish("sink_test", "dynamic", "127.0.0.1", rtsp_port, false);
    const auto hls_port = ReservePort();
    const auto hls = sink.AddHlsPublish("sink_test", "dynamic", "127.0.0.1",
                                        hls_port, false);
    const auto info = sink.output_info();
    CHECK(info.output_urls == std::vector<std::string>{path});
    CHECK(info.rtsp_url == "rtsp://127.0.0.1:" + std::to_string(rtsp_port) +
                               "/sink_test/dynamic");
    CHECK(info.hls_url == hls);
    REQUIRE(sink.Send(VideoFrame()).get());
  }
  CHECK(DecodeFrames(path, AVMEDIA_TYPE_VIDEO, 1) == 1);
}

TEST_CASE("EncodePushSink 同步提交参数异常进入 future 而非终止错误回调",
          "[output][encode_push_sink][failure]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  Errors errors;
  EncodePushSink sink(device);
  sink.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  REQUIRE(sink.Initialize(VideoConfig()));
  std::future<bool> result;
  REQUIRE_NOTHROW(result = sink.Send({}));
  REQUIRE(result.valid());
  REQUIRE(result.wait_for(0s) == std::future_status::ready);
  CHECK_THROWS_AS(result.get(), std::invalid_argument);
  CHECK(errors.Count() == 0);
  CHECK(sink.Send(VideoFrame()).get());
}

TEST_CASE("EncodePushSink 异步不可恢复错误只通知一次并拒绝后续帧",
          "[output][encode_push_sink][failure][callback]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  Errors errors;
  int callback_failure = 0;
  SECTION("普通观察者") {}
  SECTION("观察者抛出标准异常") { callback_failure = 1; }
  SECTION("观察者抛出未知异常") { callback_failure = 2; }
  {
    EncodePushSink sink(device);
    sink.SetOnError([&](std::exception_ptr error) {
      errors.Add(error);
      if (callback_failure == 1) throw std::runtime_error("测试观察者异常");
      if (callback_failure == 2) throw 7;
    });
    REQUIRE(sink.Initialize(VideoConfig()));
    REQUIRE(sink.Send(VideoFrame(32)).get());
    const auto error = errors.Wait();
    REQUIRE(error != nullptr);
    CHECK_THROWS_AS(std::rethrow_exception(error), std::invalid_argument);
    CHECK_FALSE(sink.Send(VideoFrame()).get());
    CHECK_FALSE(sink.Initialize(VideoConfig()));
  }
  CHECK(errors.Count() == 1);
}

TEST_CASE("Output 接收真实 EncodePushSink 终止拒绝后标记失败并释放引用",
          "[output][encode_push_sink][failure][ownership][integration]") {
  const auto runtime = InitRuntime();
  const ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCpu);
  Errors errors;
  auto sink = std::make_shared<EncodePushSink>(device);
  sink->SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const std::weak_ptr<EncodePushSink> weak = sink;
  Output output({sink});
  REQUIRE(output.Start(VideoConfig()));
  CHECK_FALSE(output.HasSynchronizationClock());
  sink.reset();
  REQUIRE_FALSE(weak.expired());
  REQUIRE(output.Send(VideoFrame(32)));
  const auto error = errors.Wait();
  REQUIRE(error != nullptr);
  CHECK_THROWS_AS(std::rethrow_exception(error), std::invalid_argument);
  REQUIRE(output.Send(VideoFrame()));
  const auto status = output.status();
  REQUIRE(status.frame_sinks.size() == 1);
  CHECK(status.frame_sinks.front().state == FrameSinkState::kFailed);
  CHECK(weak.expired());
  CHECK(errors.Count() == 1);
}
