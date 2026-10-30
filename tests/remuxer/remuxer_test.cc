#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::RecorderState;
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
  AVDictionary* options = nullptr;
  av_dict_set(&options, "rw_timeout", "3000000", 0);
  const int code = avformat_open_input(&raw, path.c_str(), nullptr, &options);
  av_dict_free(&options);
  ffmpeg::FfmpegException::throwIfError(code, "打开 Remuxer 测试媒体");
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

struct CodecCloser {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

void CheckRecording(const std::string& path, std::size_t expected_packets) {
  const auto input = OpenMedia(path);
  REQUIRE(input->nb_streams == 1);
  const auto* stream = input->streams[0];
  REQUIRE(stream->codecpar->codec_id == AV_CODEC_ID_H264);
  const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecCloser> decoder(
      avcodec_alloc_context3(codec));
  REQUIRE(decoder != nullptr);
  REQUIRE(avcodec_parameters_to_context(decoder.get(), stream->codecpar) >= 0);
  decoder->pkt_timebase = stream->time_base;
  decoder->thread_count = 1;
  REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  std::size_t frames = 0;
  const auto receive = [&] {
    for (;;) {
      ffmpeg::Frame frame;
      const int result = avcodec_receive_frame(decoder.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      ++frames;
    }
  };
  std::size_t packets = 0;
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
    REQUIRE(packet->stream_index == 0);
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    receive();
    ++packets;
    packet.Unref();
  }
  CHECK(result == AVERROR_EOF);
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  receive();
  CHECK(packets == expected_packets);
  CHECK(frames == expected_packets);
}

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

std::string ReadHttp(const std::string& url) {
  AVDictionary* options = nullptr;
  av_dict_set(&options, "rw_timeout", "3000000", 0);
  AVIOContext* raw = nullptr;
  const int opened =
      avio_open2(&raw, url.c_str(), AVIO_FLAG_READ, nullptr, &options);
  av_dict_free(&options);
  ffmpeg::FfmpegException::throwIfError(opened, "读取原生 HLS HTTP 输出");
  const auto close = [](AVIOContext* context) { avio_closep(&context); };
  std::unique_ptr<AVIOContext, decltype(close)> input(raw, close);
  std::string response;
  std::array<unsigned char, 4096> buffer{};
  for (;;) {
    const int count =
        avio_read(input.get(), buffer.data(), static_cast<int>(buffer.size()));
    if (count == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(count, "读取原生 HLS HTTP 内容");
    if (count == 0) break;
    response.append(reinterpret_cast<const char*>(buffer.data()),
                    static_cast<std::size_t>(count));
  }
  return response;
}

void FeedRepeatedMedia(Remuxer& remuxer, const Media& media,
                       std::int64_t& offset) {
  const auto time_base = media.streams.front().time_base;
  const auto& first = media.packets.front();
  const auto& last = media.packets.back();
  const auto duration =
      last->dts - first->dts + std::max<std::int64_t>(last->duration, 1);
  REQUIRE(duration > 0);
  const auto until = offset + av_rescale_q(30, AVRational{1, 1}, time_base);
  while (offset < until) {
    for (const auto& source : media.packets) {
      auto packet = source.Ref();
      packet->pts += offset;
      packet->dts += offset;
      REQUIRE(remuxer.SubmitPacket(packet));
    }
    offset += duration;
  }
}

}  // namespace

TEST_CASE("Remuxer 查询注册输出地址且停止保留地址快照", "[remuxer][outputs]") {
  const auto runtime = InitRuntime();
  Remuxer remuxer({"live", "camera"});
  const std::string target = "rtsp://127.0.0.1:8554/live/remote";
  CHECK(remuxer.push_statuses().empty());
  CHECK(remuxer.AddPushUrl(target) == target);
  CHECK(remuxer.AddPushUrl(target) == target);
  remuxer.AddRtspPublish("127.0.0.1", 8555);
  CHECK(remuxer.AddHlsPublish("127.0.0.1", 8080) ==
        "http://127.0.0.1:8080/live/camera/hls.m3u8");
  const Remuxer::OutputInfo planned = remuxer.output_info();
  CHECK(planned.output_urls == std::vector<std::string>{target});
  CHECK(planned.rtsp_url == "rtsp://127.0.0.1:8555/live/camera");
  CHECK(planned.hls_url == "http://127.0.0.1:8080/live/camera/hls.m3u8");
  CHECK(remuxer.push_statuses().empty());
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

TEST_CASE("Remuxer 本地 MP4 异步录制排空后可以完整解码",
          "[remuxer][outputs][recording][lifecycle]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto requested = directory.File("recording.mp4");
  const auto output = remuxer.AddPushUrl(requested);
  CHECK(remuxer.AddPushUrl(requested) == output);
  CHECK_FALSE(fs::exists(fs::u8path(output)));
  CHECK(remuxer.recording_statuses().empty());
  REQUIRE(remuxer.Start(media.streams));
  CHECK(remuxer.output_info().output_urls == std::vector<std::string>{output});
  REQUIRE(remuxer.recording_statuses().size() == 1);
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Drain();
  remuxer.Stop();
  CHECK(errors.Count() == 0);
  const auto statuses = remuxer.recording_statuses();
  REQUIRE(statuses.size() == 1);
  CHECK(statuses.front().path == output);
  CHECK(statuses.front().status.state == RecorderState::kStopped);
  CHECK(statuses.front().status.written_packets == media.packets.size());
  CHECK(statuses.front().status.dropped_packets == 0);
  CHECK(statuses.front().status.queue_bytes == 0);
  CHECK(statuses.front().status.error.empty());
  CheckRecording(output, media.packets.size());
  CHECK(remuxer.push_statuses().empty());
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  REQUIRE(remuxer.Start(media.streams));
  CHECK(remuxer.recording_statuses().empty());
  remuxer.Stop();
}

TEST_CASE("Remuxer 录像保留非法 NAL 且协议跳过后继续处理",
          "[remuxer][recording][publish][validation]") {
  const auto runtime = InitRuntime();
  Media media;
  const auto* parameters = media.streams.front().codec_parameters.get();
  REQUIRE(parameters->extradata_size >= 5);
  REQUIRE(parameters->extradata[0] == 1);
  const int length_width = (parameters->extradata[4] & 0x03) + 1;
  REQUIRE(media.packets.size() >= 3);
  auto& malformed = media.packets[media.packets.size() / 2];
  REQUIRE(malformed->size > length_width);
  REQUIRE(av_packet_make_writable(malformed.get()) >= 0);
  // Retain the MP4 sample's length prefix and timestamps, but violate the
  // forbidden_zero_bit constraint handled by protocol packet conversion.
  malformed->data[length_width] |= 0x80;

  bool publish = false;
  SECTION("仅 MP4 录像逐包保留输入") {}
  SECTION("启用 RTSP 发布跳过非法 NAL 后继续") { publish = true; }
  OutputDirectory directory;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto output = remuxer.AddPushUrl(directory.File("nonstandard.mp4"));
  if (publish) remuxer.AddRtspPublish("127.0.0.1", ReservePort());
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Drain();
  remuxer.Stop();
  CHECK(errors.Count() == 0);
  const auto recordings = remuxer.recording_statuses();
  REQUIRE(recordings.size() == 1);
  CHECK(recordings.front().status.state == RecorderState::kStopped);
  CHECK(recordings.front().status.error.empty());
  CHECK(recordings.front().status.written_packets == media.packets.size());
  CHECK(recordings.front().status.dropped_packets == 0);

  // Inspect container packets only: the deliberately malformed sample must
  // never be submitted to a decoder, including stream-info probing.
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_open_input(&raw, output.c_str(), nullptr, nullptr) >= 0);
  const std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  input->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
  ffmpeg::Packet packet;
  for (const auto& original : media.packets) {
    REQUIRE(av_read_frame(input.get(), packet.get()) >= 0);
    REQUIRE(packet->size == original->size);
    CHECK(std::equal(original->data, original->data + original->size,
                     packet->data));
    packet.Unref();
  }
  CHECK(av_read_frame(input.get(), packet.get()) == AVERROR_EOF);
}

TEST_CASE("Remuxer 关闭协议后释放流标识且旧录像可以后台收尾",
          "[remuxer][recording][lifecycle][publish]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  const auto port = ReservePort();
  Remuxer previous({"live", "protocol_restart"});
  const auto path = previous.AddPushUrl(directory.File("previous.mp4"));
  previous.AddRtspPublish("127.0.0.1", port);
  const auto planned = previous.output_info();
  REQUIRE(previous.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(previous.SubmitPacket(packet));
  previous.StopProtocols();
  CHECK_FALSE(previous.SubmitPacket(media.packets.front()));
  CHECK(previous.output_info().rtsp_url == planned.rtsp_url);
  CHECK(previous.output_info().output_urls == planned.output_urls);
  CHECK(previous.push_statuses().empty());
  const auto pending = previous.recording_statuses();
  REQUIRE(pending.size() == 1);
  CHECK((pending.front().status.state == RecorderState::kStopping ||
         pending.front().status.state == RecorderState::kStopped));

  // The previous instance and its file workers are still alive. Starting the
  // replacement must not need previous.Stop() to release the source/listener.
  Remuxer replacement({"live", "protocol_restart"});
  replacement.AddRtspPublish("127.0.0.1", port);
  REQUIRE(replacement.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(replacement.SubmitPacket(packet));
  previous.Stop();
  const auto completed = previous.recording_statuses();
  REQUIRE(completed.size() == 1);
  CHECK(completed.front().status.state == RecorderState::kStopped);
  CHECK(completed.front().status.written_packets == media.packets.size());
  CHECK(completed.front().status.dropped_packets == 0);
  CheckRecording(path, media.packets.size());
  replacement.Stop();

  // Protocol teardown also clears registration before a new session.
  REQUIRE(previous.Start(media.streams));
  CHECK(previous.output_info().output_urls.empty());
  CHECK(previous.output_info().rtsp_url.empty());
  CHECK(previous.recording_statuses().empty());
  previous.Stop();
}

TEST_CASE("Remuxer 不接受本地 M3U8 录像", "[remuxer][recording][config]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  Remuxer remuxer;
  CHECK_THROWS_AS(remuxer.AddPushUrl(directory.File("recording.m3u8")),
                  std::invalid_argument);
  CHECK(remuxer.output_info().output_urls.empty());
  CHECK(remuxer.recording_statuses().empty());
}

TEST_CASE("Remuxer 支持多个 MP4 及运行中添加录像",
          "[remuxer][recording][outputs]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  Remuxer remuxer;
  const auto first = remuxer.AddPushUrl(directory.File("first.mp4"));
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  const auto second = remuxer.AddPushUrl(directory.File("second.mp4"));
  CHECK(remuxer.AddPushUrl(directory.File("second.mp4")) == second);
  REQUIRE(remuxer.recording_statuses().size() == 2);
  const auto& front = media.packets.front();
  const auto& last = media.packets.back();
  const auto offset =
      last->dts - front->dts + std::max<std::int64_t>(last->duration, 1);
  for (const auto& source : media.packets) {
    auto packet = source.Ref();
    packet->pts += offset;
    packet->dts += offset;
    REQUIRE(remuxer.SubmitPacket(packet));
  }
  remuxer.Stop();
  const auto statuses = remuxer.recording_statuses();
  REQUIRE(statuses.size() == 2);
  CHECK(statuses[0].status.state == RecorderState::kStopped);
  CHECK(statuses[1].status.state == RecorderState::kStopped);
  CheckRecording(first, 2 * media.packets.size());
  CheckRecording(second, media.packets.size());
}

TEST_CASE("Remuxer 动态录像直接接收当前非关键帧不等待恢复点",
          "[remuxer][recording][outputs]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  Remuxer remuxer;
  REQUIRE(remuxer.Start(media.streams));
  const auto first_non_key = std::find_if(
      media.packets.begin(), media.packets.end(), [](const auto& packet) {
        return (packet->flags & AV_PKT_FLAG_KEY) == 0;
      });
  REQUIRE(first_non_key != media.packets.end());
  for (auto packet = media.packets.begin(); packet != first_non_key; ++packet)
    REQUIRE(remuxer.SubmitPacket(*packet));
  const auto path = remuxer.AddPushUrl(directory.File("non_key_start.mp4"));
  for (auto packet = first_non_key; packet != media.packets.end(); ++packet)
    REQUIRE(remuxer.SubmitPacket(*packet));
  remuxer.Stop();
  const auto recordings = remuxer.recording_statuses();
  REQUIRE(recordings.size() == 1);
  CHECK(recordings.front().status.state == RecorderState::kStopped);
  CHECK(recordings.front().status.error.empty());
  const auto subsequent_packets = static_cast<std::uint64_t>(
      std::distance(first_non_key, media.packets.end()));
  // Packets submitted before registration may still be awaiting dispatch and
  // reach the new recording. All packets submitted afterwards must be written.
  CHECK(recordings.front().status.written_packets >= subsequent_packets);
  CHECK(recordings.front().status.written_packets <= media.packets.size());
  CHECK(recordings.front().status.dropped_packets == 0);
  CHECK(fs::file_size(fs::u8path(path)) > 0);
}

TEST_CASE("Remuxer 单个录像失败不影响其他录像和发布",
          "[remuxer][recording][failure]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto failed = remuxer.AddPushUrl(directory.File("missing/failed.mp4"));
  const auto healthy = remuxer.AddPushUrl(directory.File("healthy.mp4"));
  remuxer.AddRtspPublish("127.0.0.1", ReservePort());
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  CHECK_FALSE(remuxer.output_info().rtsp_url.empty());
  remuxer.Stop();
  const auto statuses = remuxer.recording_statuses();
  REQUIRE(statuses.size() == 2);
  CHECK(statuses[0].path == failed);
  CHECK(statuses[0].status.state == RecorderState::kFailed);
  CHECK_FALSE(statuses[0].status.error.empty());
  CHECK(statuses[0].status.queue_bytes == 0);
  CHECK(statuses[1].path == healthy);
  CHECK(statuses[1].status.state == RecorderState::kStopped);
  CHECK(errors.Count() == 0);
  CheckRecording(healthy, media.packets.size());
}

TEST_CASE("Remuxer 录像队列满直接丢包且不触发链路错误",
          "[remuxer][recording][overflow]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  Errors errors;
  mw::streamer::RemuxerConfig config;
  config.recording_queue_bytes = 1;
  Remuxer remuxer(config);
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  remuxer.AddPushUrl(directory.File("overflow.mp4"));
  REQUIRE(remuxer.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Stop();
  const auto statuses = remuxer.recording_statuses();
  REQUIRE(statuses.size() == 1);
  CHECK(statuses.front().status.state == RecorderState::kStopped);
  CHECK(statuses.front().status.written_packets == 0);
  CHECK(statuses.front().status.dropped_packets == media.packets.size());
  CHECK(errors.Count() == 0);
}

TEST_CASE("Remuxer 多轮 Start Drain Stop 排空并支持重复停止",
          "[remuxer][lifecycle]") {
  const auto runtime = InitRuntime();
  const Media media;
  Errors errors;
  Remuxer remuxer;
  CHECK_FALSE(remuxer.error());
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  for (int session = 0; session < 3; ++session) {
    REQUIRE(remuxer.Start(media.streams));
    CHECK_FALSE(remuxer.error());
    for (const auto& packet : media.packets)
      REQUIRE(remuxer.SubmitPacket(packet));
    remuxer.Drain();
    remuxer.Stop();
    CHECK_FALSE(remuxer.error());
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
  const auto startup_failure = remuxer.error();
  REQUIRE(startup_failure);
  CHECK_THROWS(std::rethrow_exception(startup_failure));
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  CHECK(remuxer.error() == startup_failure);
  CHECK(errors.Count() == 0);
  REQUIRE(remuxer.Start(media.streams));
  CHECK_FALSE(remuxer.error());
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Stop();
  CHECK_FALSE(remuxer.error());
  CHECK(errors.Count() == 0);
}

TEST_CASE("Remuxer 重复启动返回失败且保留当前输出会话",
          "[remuxer][lifecycle][start][outputs]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  Errors errors;
  Remuxer remuxer;
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto output = remuxer.AddPushUrl(directory.File("repeat_start.mp4"));
  REQUIRE(remuxer.Start(media.streams));
  REQUIRE(remuxer.SubmitPacket(media.packets.front()));
  CHECK_FALSE(remuxer.Start(media.streams));
  CHECK_FALSE(remuxer.Start({}));
  CHECK_FALSE(remuxer.error());
  CHECK(remuxer.output_info().output_urls == std::vector<std::string>{output});
  for (std::size_t index = 1; index < media.packets.size(); ++index)
    REQUIRE(remuxer.SubmitPacket(media.packets[index]));
  remuxer.Stop();
  CHECK_FALSE(remuxer.error());
  CHECK(errors.Count() == 0);
  CheckRecording(output, media.packets.size());
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
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
  Remuxer failed({"remuxer_test", stream});
  failed.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  failed.AddRtspPublish("127.0.0.1", port);
  failed.AddHlsPublish("127.0.0.1", port, true);
  CHECK_FALSE(failed.Start(media.streams));
  const auto startup_failure = failed.error();
  REQUIRE(startup_failure);
  CHECK_FALSE(failed.SubmitPacket(media.packets.front()));
  CHECK(errors.Count() == 0);
  Remuxer replacement({"remuxer_test", stream});
  replacement.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  replacement.AddRtspPublish("127.0.0.1", port);
  REQUIRE(replacement.Start(media.streams));
  for (const auto& packet : media.packets)
    REQUIRE(replacement.SubmitPacket(packet));
  replacement.Stop();
  CHECK_FALSE(replacement.error());
  failed.Stop();
  CHECK(failed.error() == startup_failure);
  failed.AddRtspPublish("127.0.0.1", port);
  REQUIRE(failed.Start(media.streams));
  CHECK_FALSE(failed.error());
  for (const auto& packet : media.packets) REQUIRE(failed.SubmitPacket(packet));
  failed.Stop();
  CHECK_FALSE(failed.error());
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
  bool callback_snapshot_matches = false;
  remuxer.SetOnError([&](std::exception_ptr error) {
    callback_snapshot_matches = remuxer.error() == error;
    errors.Add(error);
    if (failure == CallbackFailure::kStandard)
      throw std::runtime_error("测试错误回调异常");
    if (failure == CallbackFailure::kUnknown) throw 17;
  });
  REQUIRE(remuxer.Start(streams));
  REQUIRE(remuxer.SubmitPacket(InvalidPacket(streams.front())));
  const auto error = errors.Wait();
  CHECK(remuxer.error() == error);
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  REQUIRE_THROWS_AS(std::rethrow_exception(error), std::invalid_argument);
  try {
    std::rethrow_exception(error);
  } catch (const std::invalid_argument& exception) {
    CHECK(std::string(exception.what()).find("Annex B") != std::string::npos);
  }
  remuxer.Stop();
  CHECK(callback_snapshot_matches);
  CHECK(remuxer.error() == error);
  CHECK(errors.Count() == 1);
  CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
  CHECK(remuxer.error() == error);
  REQUIRE(remuxer.Start(media.streams));
  CHECK_FALSE(remuxer.error());
  for (const auto& packet : media.packets)
    REQUIRE(remuxer.SubmitPacket(packet));
  remuxer.Stop();
  CHECK_FALSE(remuxer.error());
  CHECK(errors.Count() == 1);
}

TEST_CASE("Remuxer 真实 RTSP 连接失败自动重试且本地发布继续处理",
          "[remuxer][callbacks][network]") {
  OutputDirectory directory;
  const auto log_path = directory.File("network.log");
  auto runtime = InitRuntime(log_path);
  const Media media;
  const std::string target = "rtsp://127.0.0.1:1/remuxer_test/unavailable";
  Errors errors;
  std::string output;
  {
    Remuxer remuxer;
    remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
    output = remuxer.AddPushUrl(directory.File("network.mp4"));
    remuxer.AddRtspPublish("127.0.0.1", ReservePort());
    REQUIRE(remuxer.Start(media.streams));
    CHECK(remuxer.push_statuses().empty());
    const auto published = remuxer.output_info().rtsp_url;
    REQUIRE_FALSE(published.empty());
    CHECK(remuxer.AddPushUrl(target) == target);
    REQUIRE(media.packets.size() > 1);
    const auto initial_packets = media.packets.size() / 2;
    for (std::size_t index = 0; index < initial_packets; ++index)
      REQUIRE(remuxer.SubmitPacket(media.packets[index]));
    // Allow the local connect failure and the SDK's two-second retry delay.
    CHECK_FALSE(errors.WaitForError(std::chrono::seconds(6)));
    const auto statuses = remuxer.push_statuses();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses.front().url == target);
    CHECK(statuses.front().status == 1);
    const auto second_target = target + "_second";
    CHECK(remuxer.AddPushUrl(second_target) == second_target);
    CHECK(remuxer.AddPushUrl(target) == target);
    const auto updated = remuxer.push_statuses();
    REQUIRE(updated.size() == 2);
    CHECK(updated[0].url == target);
    CHECK(updated[1].url == second_target);
    CHECK(updated[1].status == 1);
    for (std::size_t index = initial_packets; index < media.packets.size();
         ++index)
      REQUIRE(remuxer.SubmitPacket(media.packets[index]));
    CHECK(remuxer.output_info().rtsp_url == published);
    remuxer.Stop();
    CHECK(remuxer.output_info().rtsp_url == published);
    CHECK(remuxer.push_statuses().empty());
  }
  runtime.reset();
  CHECK(errors.Count() == 0);
  CheckRecording(output, media.packets.size());
  std::ifstream log(fs::u8path(log_path));
  REQUIRE(log.is_open());
  const std::string messages{std::istreambuf_iterator<char>(log),
                             std::istreambuf_iterator<char>()};
  INFO(messages);
  CHECK(messages.find("target=" + target) != std::string::npos);
  CHECK(messages.find("Remuxer 推流连接失败，将自动重连") != std::string::npos);
  CHECK(messages.find("推流重试") != std::string::npos);
}

TEST_CASE("Remuxer 统一流标识支持 RTSP 与原生 HLS 同时发布及动态启用",
          "[remuxer][outputs][publish][http]") {
  const auto runtime = InitRuntime();
  const Media media;
  OutputDirectory directory;
  const auto rtsp_port = ReservePort();
  auto hls_port = ReservePort();
  while (hls_port == rtsp_port) hls_port = ReservePort();
  const std::string stream =
      "shared_publish_" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  bool dynamic_hls = false;
  SECTION("启动时同时发布") {}
  SECTION("已有数据后动态启用 HLS") { dynamic_hls = true; }
  Errors errors;
  Remuxer remuxer({"remuxer_test", stream});
  remuxer.SetOnError([&](std::exception_ptr error) { errors.Add(error); });
  const auto rtsp_url = "rtsp://127.0.0.1:" + std::to_string(rtsp_port) +
                        "/remuxer_test/" + stream;
  const auto hls_url = "http://127.0.0.1:" + std::to_string(hls_port) +
                       "/remuxer_test/" + stream + "/hls.m3u8";
  for (int session = 0; session < 2; ++session) {
    const auto recording = remuxer.AddPushUrl(
        directory.File("shared_" + std::to_string(session) + ".mp4"));
    remuxer.AddRtspPublish("127.0.0.1", rtsp_port);
    if (!dynamic_hls)
      REQUIRE(remuxer.AddHlsPublish("127.0.0.1", hls_port) == hls_url);
    REQUIRE(remuxer.Start(media.streams));
    std::int64_t offset = 0;
    if (dynamic_hls) {
      FeedRepeatedMedia(remuxer, media, offset);
      REQUIRE(remuxer.AddHlsPublish("127.0.0.1", hls_port) == hls_url);
    }
    CHECK(remuxer.output_info().rtsp_url == rtsp_url);
    CHECK(remuxer.output_info().hls_url == hls_url);
    CHECK(remuxer.push_statuses().empty());
    FeedRepeatedMedia(remuxer, media, offset);
    const auto playlist = ReadHttp(hls_url);
    REQUIRE(playlist.find("#EXTM3U") == 0);
    CHECK(playlist.find("#EXT-X-ENDLIST") == std::string::npos);
    CHECK(playlist.find("#EXT-X-MAP") == std::string::npos);
    std::string segment;
    std::size_t begin = 0;
    while (begin < playlist.size()) {
      const auto end = playlist.find('\n', begin);
      auto line = playlist.substr(begin, end - begin);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty() && line.front() != '#') segment = std::move(line);
      if (end == std::string::npos) break;
      begin = end + 1;
    }
    REQUIRE_FALSE(segment.empty());
    CHECK(segment.find(".ts") != std::string::npos);
    const auto segment_url =
        segment.find("://") == std::string::npos
            ? hls_url.substr(0, hls_url.rfind('/') + 1) + segment
            : segment;
    const auto input = OpenMedia(segment_url);
    const auto index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1,
                                           -1, nullptr, 0);
    REQUIRE(index >= 0);
    CHECK(input->streams[index]->codecpar->codec_id == AV_CODEC_ID_H264);
    ffmpeg::Packet packet;
    REQUIRE(av_read_frame(input.get(), packet.get()) >= 0);
    CHECK(packet->size > 0);
    remuxer.Drain();
    remuxer.Stop();
    CHECK(remuxer.output_info().rtsp_url == rtsp_url);
    CHECK(remuxer.output_info().hls_url == hls_url);
    CHECK_FALSE(remuxer.SubmitPacket(media.packets.front()));
    CHECK(errors.Count() == 0);
    const auto recordings = remuxer.recording_statuses();
    REQUIRE(recordings.size() == 1);
    CHECK(recordings.front().path == recording);
    CHECK(recordings.front().status.state == RecorderState::kStopped);
    CHECK(recordings.front().status.dropped_packets == 0);
    CheckRecording(recording, static_cast<std::size_t>(
                                  recordings.front().status.written_packets));
  }
}

TEST_CASE("Remuxer 配置拒绝非法流标识且默认流名按实例唯一",
          "[remuxer][outputs][config]") {
  CHECK_THROWS_AS((Remuxer({"bad/name", "camera"})), std::invalid_argument);
  CHECK_THROWS_AS((Remuxer({"live", ".."})), std::invalid_argument);
  Remuxer first;
  Remuxer second;
  const auto first_url = first.AddHlsPublish("127.0.0.1", 8080);
  const auto second_url = second.AddHlsPublish("127.0.0.1", 8080);
  CHECK(first_url != second_url);
  CHECK(first.output_info().hls_url == first_url);
  CHECK(second.output_info().hls_url == second_url);
  CHECK(first.push_statuses().empty());
  CHECK(second.push_statuses().empty());
}
