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
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::Remuxer;
using Runtime = std::unique_ptr<mw::streamer::MwStreamerContext,
                                decltype(&mw::streamer::Shutdown)>;

Runtime InitRuntime(const std::string& log_path = {}) {
  mw::streamer::InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console.enabled = false;
  if (!log_path.empty()) {
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = log_path;
    config.log.modules["zlm"] = mw::log::LogLevel::kWarning;
  }
  return Runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
}

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

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, FormatCloser> OpenMedia(
    const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开 Remuxer 测试媒体");
  std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(input.get(), nullptr), "探测测试媒体轨道");
  return input;
}

std::string SamplePath() {
  return (fs::u8path(MW_STREAMER_TEST_DATA_DIR) / "h264_video.mp4")
      .generic_u8string();
}

struct Media final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;

  Media() {
    const auto input = OpenMedia(SamplePath());
    const int index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1,
                                          -1, nullptr, 0);
    REQUIRE(index >= 0);
    const auto* stream = input->streams[index];
    REQUIRE(stream->codecpar->codec_id == AV_CODEC_ID_H264);
    ffmpeg::StreamInfo info;
    info.stream_index = index;
    info.time_base = stream->time_base;
    info.codec_parameters = ffmpeg::CodecParameters(*stream->codecpar);
    streams.push_back(info);
    ffmpeg::Packet packet;
    int status = 0;
    while ((status = av_read_frame(input.get(), packet.get())) >= 0) {
      if (packet->stream_index == index) {
        packet->time_base = info.time_base;
        packets.push_back(packet.Ref());
      }
      packet.Unref();
    }
    REQUIRE(status == AVERROR_EOF);
    REQUIRE_FALSE(packets.empty());
  }
};

class OutputDirectory final {
 public:
  OutputDirectory()
      : root_(fs::absolute(fs::temp_directory_path()).lexically_normal()),
        path_(root_ / ("mw-remuxer-test-" +
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

struct Errors final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::exception_ptr> values;

  void Add(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex);
    values.push_back(error);
    changed.notify_all();
  }
  std::exception_ptr Wait() {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(changed.wait_for(lock, std::chrono::seconds(10),
                             [&] { return !values.empty(); }));
    REQUIRE(values.front() != nullptr);
    return values.front();
  }
  std::size_t Count() {
    std::lock_guard<std::mutex> lock(mutex);
    return values.size();
  }
  bool WaitForError(std::chrono::seconds duration) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, duration, [&] { return !values.empty(); });
  }
};

std::size_t DecodeVideo(const std::string& path) {
  const auto input = OpenMedia(path);
  REQUIRE(input->nb_streams == 1);
  const auto* stream = input->streams[0];
  REQUIRE(stream->codecpar->codec_id == AV_CODEC_ID_H264);
  const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  REQUIRE(decoder != nullptr);
  ffmpeg::CodecContext codec(decoder);
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_to_context(codec.get(), stream->codecpar),
      "导入录制视频解码参数");
  ffmpeg::FfmpegException::throwIfError(
      avcodec_open2(codec.get(), decoder, nullptr), "打开录制视频解码器");
  ffmpeg::Frame frame;
  std::size_t decoded = 0;
  const auto receive = [&] {
    for (;;) {
      const int status = avcodec_receive_frame(codec.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
      ffmpeg::FfmpegException::throwIfError(status, "解码录制视频帧");
      CHECK(frame->width == stream->codecpar->width);
      CHECK(frame->height == stream->codecpar->height);
      ++decoded;
      frame.Unref();
    }
  };
  ffmpeg::Packet packet;
  int status = 0;
  while ((status = av_read_frame(input.get(), packet.get())) >= 0) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(codec.get(), packet.get()), "提交录制视频包");
    receive();
    packet.Unref();
  }
  REQUIRE(status == AVERROR_EOF);
  ffmpeg::FfmpegException::throwIfError(
      avcodec_send_packet(codec.get(), nullptr), "排空录制视频解码器");
  receive();
  return decoded;
}

ffmpeg::Packet InvalidPacket(const ffmpeg::StreamInfo& stream) {
  ffmpeg::Packet packet;
  ffmpeg::FfmpegException::throwIfError(av_new_packet(packet.get(), 1),
                                        "分配非法视频测试包");
  packet->data[0] = 1;
  packet->stream_index = stream.stream_index;
  packet->time_base = stream.time_base;
  packet->pts = packet->dts = 0;
  return packet;
}

}  // namespace

TEST_CASE("Remuxer 查询注册输出地址且停止保留地址快照", "[remuxer][outputs]") {
  const auto runtime = InitRuntime();
  Remuxer remuxer;
  const std::string target = "rtsp://127.0.0.1:8554/live/remote";
  CHECK(remuxer.AddPushUrl(target) == target);
  CHECK(remuxer.AddPushUrl(target) == target);
  remuxer.AddRtspPublish("live", "camera", "127.0.0.1", 8555);
  CHECK(remuxer.AddHlsPublish("live", "camera", "127.0.0.1", 8080) ==
        "http://127.0.0.1:8080/live/camera/hls.m3u8");
  const Remuxer::OutputInfo planned = remuxer.output_info();
  CHECK(planned.output_urls == std::vector<std::string>{target});
  CHECK(planned.rtsp_url == "rtsp://127.0.0.1:8555/live/camera");
  CHECK(planned.hls_url == "http://127.0.0.1:8080/live/camera/hls.m3u8");
  remuxer.Stop();
  CHECK(remuxer.output_info().output_urls == planned.output_urls);
  CHECK(remuxer.output_info().rtsp_url == planned.rtsp_url);
  CHECK(remuxer.output_info().hls_url == planned.hls_url);
  const Media media;
  REQUIRE(remuxer.Start(media.streams));
  CHECK(remuxer.output_info().output_urls.empty());
  CHECK(remuxer.output_info().rtsp_url.empty());
  CHECK(remuxer.output_info().hls_url.empty());
  remuxer.Stop();
}

TEST_CASE("Remuxer Stop 完成真实 MP4 录制且全部视频帧可解码",
          "[remuxer][recording][integration]") {
  const auto runtime = InitRuntime();
  const Media media;
  const auto expected_frames = DecodeVideo(SamplePath());
  REQUIRE(expected_frames > 0);
  OutputDirectory directory;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto output = remuxer.AddPushUrl(directory.File("recording.mp4"));
  CHECK_FALSE(fs::exists(fs::u8path(output)));
  REQUIRE(remuxer.Start(media.streams));
  CHECK(remuxer.output_info().output_urls == std::vector<std::string>{output});
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Drain();
  remuxer.Stop();
  CHECK(errors.Count() == 0);
  REQUIRE(fs::exists(fs::u8path(output)));
  CHECK(DecodeVideo(output) == expected_frames);
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
}

TEST_CASE("Remuxer 多轮 Start Drain Stop 排空并支持重复停止",
          "[remuxer][lifecycle]") {
  const auto runtime = InitRuntime();
  const Media media;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  for (int session = 0; session < 3; ++session) {
    REQUIRE(remuxer.Start(media.streams));
    for (const auto& packet : media.packets)
      REQUIRE(remuxer.SubmitPacket(packet));
    remuxer.Drain();
    remuxer.Stop();
    remuxer.Drain();
    remuxer.Stop();
    CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  }
  CHECK(errors.Count() == 0);
}

TEST_CASE("Remuxer 非法轨道启动返回失败且无需停止即可重试",
          "[remuxer][lifecycle][start][failure]") {
  const auto runtime = InitRuntime();
  const Media media;
  auto streams = media.streams;
  SECTION("空轨道") { streams.clear(); }
  SECTION("非法时间基") { streams.front().time_base = {0, 1}; }
  SECTION("不支持的编码") {
    streams.front().codec_parameters.get()->codec_id = AV_CODEC_ID_MPEG2VIDEO;
  }
  SECTION("重复轨道") { streams.push_back(streams.front()); }
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  CHECK_FALSE(remuxer.Start(streams));
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  CHECK(errors.Count() == 0);
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Stop();
  CHECK(errors.Count() == 0);
}

TEST_CASE("Remuxer 重复启动返回失败且保留正在录制的会话",
          "[remuxer][lifecycle][start][recording]") {
  const auto runtime = InitRuntime();
  const Media media;
  const auto expected_frames = DecodeVideo(SamplePath());
  REQUIRE(expected_frames > 0);
  OutputDirectory directory;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto output = remuxer.AddPushUrl(directory.File("repeat_start.mp4"));
  REQUIRE(remuxer.Start(media.streams));
  REQUIRE(remuxer.SubmitPacket(media.packets.front()));
  CHECK_FALSE(remuxer.Start(media.streams));
  CHECK_FALSE(remuxer.Start({}));
  CHECK(remuxer.output_info().output_urls == std::vector<std::string>{output});
  for (std::size_t index = 1; index < media.packets.size(); ++index)
    REQUIRE(remuxer.SubmitPacket(media.packets[index]));
  remuxer.Stop();
  CHECK(errors.Count() == 0);
  REQUIRE(fs::exists(fs::u8path(output)));
  CHECK(DecodeVideo(output) == expected_frames);
}

TEST_CASE("Remuxer 输出启动失败回滚已绑定发布路径且允许重新启动",
          "[remuxer][lifecycle][start][failure][outputs]") {
  const auto runtime = InitRuntime();
  const Media media;
  const auto port = ReservePort();
  const std::string stream =
      "start_rollback_" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  Errors errors;
  Remuxer failed;
  failed.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  failed.AddRtspPublish("remuxer_test", stream, "127.0.0.1", port);
  failed.AddHlsPublish("remuxer_test", stream, "127.0.0.1", port, true);
  CHECK_FALSE(failed.Start(media.streams));
  CHECK_FALSE(failed.SubmitPacket(media.packets.front()));
  CHECK(errors.Count() == 0);
  Remuxer replacement;
  replacement.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  replacement.AddRtspPublish("remuxer_test", stream, "127.0.0.1", port);
  REQUIRE(replacement.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(replacement.SubmitPacket(packet));
  replacement.Stop();
  failed.Stop();
  failed.AddRtspPublish("remuxer_test", stream, "127.0.0.1", port);
  REQUIRE(failed.Start(media.streams));
  for (const auto& packet : media.packets) REQUIRE(failed.SubmitPacket(packet));
  failed.Stop();
  CHECK(errors.Count() == 0);
}

TEST_CASE("Remuxer 保留处理异常类型且错误回调抛异常后仍能停止复用",
          "[remuxer][callbacks][failure]") {
  const auto runtime = InitRuntime();
  const Media media;
  auto streams = media.streams;
  auto* parameters = streams.front().codec_parameters.get();
  av_freep(&parameters->extradata);
  parameters->extradata_size = 0;
  enum class CallbackFailure { kNone, kStandard, kUnknown };
  CallbackFailure failure = CallbackFailure::kNone;
  SECTION("回调正常返回") {}
  SECTION("回调抛出标准异常") { failure = CallbackFailure::kStandard; }
  SECTION("回调抛出非标准异常") { failure = CallbackFailure::kUnknown; }
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) {
    errors.Add(error);
    if (failure == CallbackFailure::kStandard)
      throw std::runtime_error("测试错误回调异常");
    if (failure == CallbackFailure::kUnknown) throw 17;
  });
  REQUIRE(remuxer.Start(streams));
  REQUIRE(remuxer.SubmitPacket(InvalidPacket(streams.front())));
  const auto error = errors.Wait();
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  REQUIRE_THROWS_AS(std::rethrow_exception(error), std::invalid_argument);
  try {
    std::rethrow_exception(error);
  } catch (const std::invalid_argument& exception) {
    CHECK(std::string(exception.what()).find("Annex B") != std::string::npos);
  }
  remuxer.Stop();
  CHECK(errors.Count() == 1);
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Stop();
  CHECK(errors.Count() == 1);
}

TEST_CASE("Remuxer 真实 RTSP 连接失败自动重试且健康录制继续完成",
          "[remuxer][callbacks][network]") {
  OutputDirectory directory;
  const auto log_path = directory.File("network.log");
  auto runtime = InitRuntime(log_path);
  const Media media;
  const auto expected_frames = DecodeVideo(SamplePath());
  REQUIRE(expected_frames > 0);
  const std::string target = "rtsp://127.0.0.1:1/remuxer_test/unavailable";
  Errors errors;
  std::string output;
  {
    Remuxer remuxer;
    remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
    output = remuxer.AddPushUrl(directory.File("healthy.mp4"));
    REQUIRE(remuxer.Start(media.streams));
    CHECK(remuxer.AddPushUrl(target) == target);
    REQUIRE(media.packets.size() > 1);
    const auto initial_packets = media.packets.size() / 2;
    for (std::size_t index = 0; index < initial_packets; ++index)
      REQUIRE(remuxer.SubmitPacket(media.packets[index]));
    // Allow the local connect failure and the SDK's two-second retry delay.
    CHECK_FALSE(errors.WaitForError(std::chrono::seconds(6)));
    for (std::size_t index = initial_packets; index < media.packets.size();
         ++index)
      REQUIRE(remuxer.SubmitPacket(media.packets[index]));
    remuxer.Stop();
  }
  runtime.reset();
  CHECK(errors.Count() == 0);
  REQUIRE(fs::exists(fs::u8path(output)));
  CHECK(DecodeVideo(output) == expected_frames);
  std::ifstream log(fs::u8path(log_path));
  REQUIRE(log.is_open());
  const std::string messages{std::istreambuf_iterator<char>(log),
                             std::istreambuf_iterator<char>()};
  INFO(messages);
  CHECK(messages.find("target=" + target) != std::string::npos);
  CHECK(messages.find("Remuxer 推流连接失败，将自动重连") != std::string::npos);
  CHECK(messages.find("推流重试") != std::string::npos);
}
