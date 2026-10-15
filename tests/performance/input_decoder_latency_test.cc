#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "mw/log.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/av_input.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;

class InputDecoderLog final {
 public:
  explicit InputDecoderLog(mw::log::LogLevel level)
      : directory_(
            fs::temp_directory_path() /
            ("mw-input-decoder-latency-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        path_(directory_ / "performance.log"),
        runtime_(nullptr, &mw::streamer::Shutdown) {
    fs::create_directories(directory_);
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    config.log.async.enabled = false;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = path_.string();
    config.log.modules = {{"perf.input", level},
                          {"perf.decoder.video", level},
                          {"perf.decoder.audio", level},
                          {"streamer", mw::log::LogLevel::kOff}};
    runtime_.reset(mw::streamer::Init(config));
  }

  ~InputDecoderLog() {
    runtime_.reset();
    std::error_code error;
    fs::remove_all(directory_, error);
  }

  std::string Read() {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file), {}};
  }

 private:
  fs::path directory_;
  fs::path path_;
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime_;
};

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct CodecCloser {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

struct DecodeFixture {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Packet> packets;
  std::size_t frames = 0;
  std::size_t delayed_frames = 0;
};

std::string FixturePath() {
  return std::string(MW_STREAMER_LATENCY_TEST_DATA_DIR) + "/h265_aac.mp4";
}

DecodeFixture NativeFixture(AVMediaType type) {
  AVFormatContext* native = nullptr;
  const int opened =
      avformat_open_input(&native, FixturePath().c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatCloser> format(native);
  REQUIRE(opened >= 0);
  REQUIRE(avformat_find_stream_info(native, nullptr) >= 0);
  const int index = av_find_best_stream(native, type, -1, -1, nullptr, 0);
  REQUIRE(index >= 0);
  const auto* track = native->streams[index];
  DecodeFixture result;
  result.stream = {index, ffmpeg::CodecParameters(*track->codecpar),
                   track->time_base};
  result.stream.frame_rate =
      av_guess_frame_rate(native, native->streams[index], nullptr);
  for (;;) {
    ffmpeg::Packet packet;
    const int code = av_read_frame(native, packet.get());
    if (code == AVERROR_EOF) break;
    REQUIRE(code >= 0);
    if (packet->stream_index == index) {
      result.packets.push_back(std::move(packet));
    }
  }
  const auto* codec = avcodec_find_decoder(track->codecpar->codec_id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecCloser> decoder(
      avcodec_alloc_context3(codec));
  REQUIRE(decoder != nullptr);
  REQUIRE(avcodec_parameters_to_context(decoder.get(), track->codecpar) >= 0);
  decoder->thread_count = 1;
  REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  const auto receive = [&] {
    for (;;) {
      ffmpeg::Frame frame;
      const int code = avcodec_receive_frame(decoder.get(), frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE(code >= 0);
      ++result.frames;
    }
  };
  for (const auto& packet : result.packets) {
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    REQUIRE(receive() == AVERROR(EAGAIN));
  }
  const auto before_eof = result.frames;
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  REQUIRE(receive() == AVERROR_EOF);
  result.delayed_frames = result.frames - before_eof;
  return result;
}

std::vector<std::string> Summaries(const std::string& log,
                                   const std::string& stage) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < log.size()) {
    const auto end = log.find('\n', start);
    const auto line = log.substr(start, end - start);
    if (line.find("stage=" + stage + " ") != std::string::npos &&
        line.find("report=summary") != std::string::npos) {
      result.push_back(line);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

std::size_t Counter(const std::string& line, const char* field) {
  std::smatch match;
  REQUIRE(std::regex_search(line, match,
                            std::regex(std::string(field) + "=([0-9]+)")));
  return static_cast<std::size_t>(std::stoull(match[1].str()));
}

TEST_CASE("Input latency counts successful reads and finalizes once per open",
          "[latency][av-input]") {
  const auto video = NativeFixture(AVMEDIA_TYPE_VIDEO);
  const auto audio = NativeFixture(AVMEDIA_TYPE_AUDIO);
  const auto expected = video.packets.size() + audio.packets.size();
  InputDecoderLog logging(mw::log::LogLevel::kInfo);
  {
    mw::streamer::AvInput input;
    for (int session = 0; session < 2; ++session) {
      input.Open(FixturePath());
      std::size_t packets = 0;
      for (;;) {
        const auto packet = input.ReadPacket();
        if (packet.status == mw::streamer::AvInputReadStatus::kEof) break;
        REQUIRE(packet.status == mw::streamer::AvInputReadStatus::kPacket);
        ++packets;
      }
      CHECK(packets == expected);
      input.Close();
      input.Close();
    }
  }
  const auto reports = Summaries(logging.Read(), "read.packet");
  REQUIRE(reports.size() == 2);
  for (const auto& report : reports) {
    CHECK(Counter(report, "samples") == expected);
    CHECK(Counter(report, "unavailable") == 0);
  }
}

TEST_CASE(
    "Decode latency accounts for drained frames and preserves user opaque",
    "[latency][av-decoder][opaque][eof]") {
  AVMediaType type = AVMEDIA_TYPE_VIDEO;
  SECTION("video B frame drain") {}
  SECTION("audio drain") { type = AVMEDIA_TYPE_AUDIO; }
  auto fixture = NativeFixture(type);
  REQUIRE(fixture.frames > 0);
  if (type == AVMEDIA_TYPE_VIDEO) REQUIRE(fixture.delayed_frames > 0);
  for (auto& packet : fixture.packets) {
    packet->opaque_ref = av_buffer_alloc(1);
    REQUIRE(packet->opaque_ref != nullptr);
    packet->opaque_ref->data[0] = 73;
  }
  InputDecoderLog logging(mw::log::LogLevel::kInfo);
  {
    mw::streamer::AvDecoderConfig config;
    config.thread_count = 1;
    config.frame_capacity = 2;
    mw::streamer::AvDecoder decoder(fixture.stream, config);
    for (const auto& packet : fixture.packets) {
      REQUIRE(decoder.PushPacket(packet, 0));
      CHECK(packet->opaque_ref->size == 1);
      CHECK(packet->opaque_ref->data[0] == 73);
    }
    REQUIRE(decoder.PushEof(0));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::size_t frames = 0;
    for (;;) {
      auto result = decoder.TryReadFrame();
      REQUIRE(decoder.error() == nullptr);
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      if (result.status == mw::streamer::AvDecoderReadStatus::kEof) break;
      if (result.status == mw::streamer::AvDecoderReadStatus::kNull) {
        std::this_thread::sleep_for(1ms);
        continue;
      }
      REQUIRE(result.frame.has_value());
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
      REQUIRE(result.frame->get()->opaque_ref != nullptr);
      CHECK(result.frame->get()->opaque_ref->size == 1);
      CHECK(result.frame->get()->opaque_ref->data[0] == 73);
#endif
      ++frames;
    }
    CHECK(frames == fixture.frames);
    decoder.Stop();
    decoder.Stop();
    for (const auto& packet : fixture.packets) {
      CHECK(packet->opaque_ref->size == 1);
      CHECK(packet->opaque_ref->data[0] == 73);
    }
  }
  const auto stage =
      type == AVMEDIA_TYPE_VIDEO ? "decode.video" : "decode.audio";
  const auto reports = Summaries(logging.Read(), stage);
  REQUIRE(reports.size() == 1);
  CHECK(Counter(reports.front(), "samples") == fixture.frames);
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
  CHECK(Counter(reports.front(), "unavailable") == 0);
#else
  CHECK(Counter(reports.front(), "unavailable") == fixture.frames);
#endif
}

TEST_CASE("Disabled input and decode latency leave packets and logs unchanged",
          "[latency][av-input][av-decoder][disabled]") {
  auto fixture = NativeFixture(AVMEDIA_TYPE_AUDIO);
  InputDecoderLog logging(mw::log::LogLevel::kOff);
  {
    mw::streamer::AvInput input;
    input.Open(FixturePath());
    while (input.ReadPacket().status ==
           mw::streamer::AvInputReadStatus::kPacket) {
    }
    input.Close();
    mw::streamer::AvDecoderConfig config;
    config.thread_count = 1;
    config.frame_capacity = fixture.frames + 1;
    mw::streamer::AvDecoder decoder(fixture.stream, config);
    auto& packet = fixture.packets.front();
    packet->opaque_ref = av_buffer_alloc(1);
    REQUIRE(packet->opaque_ref != nullptr);
    const auto* original = packet->opaque_ref->data;
    REQUIRE(decoder.PushPacket(packet, 0));
    for (std::size_t index = 1; index < fixture.packets.size(); ++index) {
      REQUIRE(decoder.PushPacket(fixture.packets[index], 0));
    }
    REQUIRE(decoder.PushEof(0));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::size_t frames = 0;
    for (;;) {
      const auto result = decoder.TryReadFrame();
      REQUIRE(decoder.error() == nullptr);
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      if (result.status == mw::streamer::AvDecoderReadStatus::kEof) break;
      if (result.status == mw::streamer::AvDecoderReadStatus::kFrame) {
        ++frames;
      } else {
        std::this_thread::sleep_for(1ms);
      }
    }
    CHECK(frames == fixture.frames);
    decoder.Stop();
    CHECK(packet->opaque_ref->data == original);
    CHECK(packet->opaque_ref->size == 1);
  }
  const auto log = logging.Read();
  CHECK(log.find("stage=read.packet") == std::string::npos);
  CHECK(log.find("stage=decode.audio") == std::string::npos);
}

}  // namespace
