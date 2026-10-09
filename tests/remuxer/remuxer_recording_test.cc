#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "../encoder/encoder_test_support.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
constexpr AVRational kNanoseconds{1, 1000000000};
constexpr AVRational kVideoTimeBase{1001, 30000};
constexpr AVRational kAudioTimeBase{1, 48000};
constexpr int kVideoFrames = 360;
constexpr std::int64_t kSourceOrigin = 12345678900;
constexpr std::int64_t kSourceSamples = 576576;
constexpr std::int64_t kDecodedSamples = 578560;

mw::streamer::InitConfig RuntimeConfig() {
  mw::streamer::InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console_enabled = 0;
  return config;
}

class OutputDirectory final {
 public:
  OutputDirectory()
      : root_(fs::absolute(fs::current_path()).lexically_normal()),
        path_(root_ / ("remuxer-recording-" +
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

bool MarkerFrame(int index) {
  return (index >= 90 && index < 93) || (index >= 180 && index < 183) ||
         (index >= 270 && index < 273);
}

void EncodeMarkers(encoder_test::Capture& capture, int b_frames = 2) {
  mw::streamer::Encoder encoder;
  capture.Bind(encoder);
  auto config = encoder_test::Config();
  config.fps = {30000, 1001};
  config.max_b_frames = b_frames;
  config.gop_size = 60;
  config.video_options["preset"] = "medium";
  config.video_options.erase("tune");
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=5:sync-lookahead=0";
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(
      config, {encoder_test::VideoStream(), encoder_test::AudioStream()}, cpu);
  std::int64_t sample_start = 0;
  for (int index = 0; index < kVideoFrames; ++index) {
    const auto sample_end =
        av_rescale_q(index + 1, kVideoTimeBase, kAudioTimeBase);
    auto audio = encoder_test::AudioFrame(
        static_cast<int>(sample_end - sample_start),
        kSourceOrigin +
            av_rescale_q(sample_start, kAudioTimeBase, kNanoseconds));
    for (int channel = 0; channel < 2; ++channel) {
      auto* samples = reinterpret_cast<float*>(audio->extended_data[channel]);
      for (int sample = 0; sample < audio->nb_samples; ++sample) {
        const auto position = sample_start + sample;
        bool high = false;
        for (int marker = 1; marker <= 3; ++marker) {
          const auto first =
              av_rescale_q(marker * 90, kVideoTimeBase, kAudioTimeBase);
          high = high || (position >= first && position < first + 4800);
        }
        samples[sample] = high ? 0.6F : 0.0F;
      }
    }
    REQUIRE(encoder.SubmitAudio(audio));
    auto video = encoder_test::VideoFrame(0);
    video->pts =
        kSourceOrigin + av_rescale_q(index, kVideoTimeBase, kNanoseconds);
    video->duration = av_rescale_q(1, kVideoTimeBase, kNanoseconds);
    for (int row = 0; row < 64; ++row) {
      std::memset(video->data[0] + row * video->linesize[0],
                  MarkerFrame(index) ? 210 : 30, 64);
    }
    REQUIRE(encoder.SubmitVideo(video));
    sample_start = sample_end;
    // Keep the real Encoder's six-picture queue below its repetition policy.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.callback_error.empty());
  REQUIRE(capture.ended == 1);
  REQUIRE(capture.streams.size() == 2);
  REQUIRE(sample_start == kSourceSamples);
}

struct Notifications final {
  std::mutex mutex;
  std::condition_variable wake;
  int ended = 0;
  std::vector<std::string> errors;
  bool callback_failed = false;

  void Bind(mw::streamer::Remuxer& remuxer) {
    remuxer.SetOnError([this](std::string_view target, int,
                              std::string_view message) noexcept {
      try {
        std::lock_guard<std::mutex> lock(mutex);
        errors.push_back(std::string(target) + ": " + std::string(message));
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        callback_failed = true;
      }
      wake.notify_all();
    });
    remuxer.SetOnEnded([this]() noexcept {
      std::lock_guard<std::mutex> lock(mutex);
      ++ended;
      wake.notify_all();
    });
  }
  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return wake.wait_for(lock, std::chrono::seconds(20),
                         [&] { return ended != 0; });
  }
};

std::string Read(const std::string& path) {
  std::ifstream input(fs::u8path(path), std::ios::binary);
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool HasMoof(const std::string& path) {
  const auto data = Read(path);
  std::size_t offset = 0;
  while (data.size() - offset >= 8) {
    const auto number = [&](std::size_t start, std::size_t count) {
      std::uint64_t result = 0;
      for (std::size_t index = 0; index < count; ++index) {
        result =
            (result << 8) | static_cast<unsigned char>(data[start + index]);
      }
      return result;
    };
    auto length = number(offset, 4);
    if (length == 1) {
      if (data.size() - offset < 16) return false;
      length = number(offset + 8, 8);
    } else if (length == 0) {
      length = data.size() - offset;
    }
    if (length < 8 || length > data.size() - offset) return false;
    if (data.compare(offset + 4, 4, "moof") == 0) return true;
    offset += static_cast<std::size_t>(length);
  }
  return false;
}

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

struct Decoded final {
  std::vector<std::int64_t> video_pts;
  std::vector<bool> video_high;
  std::vector<std::int64_t> video_markers;
  std::vector<std::int64_t> audio_markers;
  std::int64_t samples = 0;
  int audio_frames = 0;
  int b_frames = 0;
};

Decoded DecodeFile(const std::string& path, unsigned int stream_count = 2) {
  INFO(path);
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开Remuxer录制成品");
  const std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(input.get(), nullptr), "探测录制轨道");
  REQUIRE(input->nb_streams == stream_count);
  std::vector<std::unique_ptr<AVCodecContext, CodecCloser>> codecs;
  for (unsigned int index = 0; index < input->nb_streams; ++index) {
    const auto* stream = input->streams[index];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    REQUIRE(decoder != nullptr);
    std::unique_ptr<AVCodecContext, CodecCloser> context(
        avcodec_alloc_context3(decoder));
    REQUIRE(context != nullptr);
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context.get(), stream->codecpar),
        "导入录制解码参数");
    context->pkt_timebase = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(context.get(), decoder, nullptr), "打开录制解码器");
    codecs.push_back(std::move(context));
  }
  Decoded result;
  bool video_high = false;
  bool audio_high = false;
  ffmpeg::Frame frame;
  const auto receive = [&](std::size_t index) {
    for (;;) {
      const auto status =
          avcodec_receive_frame(codecs[index].get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
      ffmpeg::FfmpegException::throwIfError(status, "解码录制媒体");
      REQUIRE(frame->best_effort_timestamp != AV_NOPTS_VALUE);
      const auto timestamp =
          av_rescale_q(frame->best_effort_timestamp,
                       input->streams[index]->time_base, kNanoseconds);
      if (codecs[index]->codec_type == AVMEDIA_TYPE_VIDEO) {
        REQUIRE(frame->width == 64);
        REQUIRE(frame->height == 64);
        result.b_frames =
            std::max(result.b_frames, codecs[index]->has_b_frames);
        const bool high = frame->data[0][0] > 150;
        if (high && !video_high) result.video_markers.push_back(timestamp);
        video_high = high;
        result.video_pts.push_back(timestamp);
        result.video_high.push_back(high);
      } else {
        REQUIRE(codecs[index]->codec_type == AVMEDIA_TYPE_AUDIO);
        REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
        REQUIRE(frame->sample_rate == 48000);
        const auto* samples =
            reinterpret_cast<const float*>(frame->extended_data[0]);
        for (int sample = 0; sample < frame->nb_samples; ++sample) {
          const bool high = std::abs(samples[sample]) > 0.25F;
          if (high && !audio_high) {
            const auto onset =
                timestamp + av_rescale_q(sample, kAudioTimeBase, kNanoseconds);
            if (result.audio_markers.empty() ||
                onset - result.audio_markers.back() > 500000000) {
              result.audio_markers.push_back(onset);
            }
          }
          audio_high = high;
        }
        result.samples += frame->nb_samples;
        ++result.audio_frames;
      }
      frame.Unref();
    }
  };
  ffmpeg::Packet packet;
  int status = 0;
  while ((status = av_read_frame(input.get(), packet.get())) >= 0) {
    REQUIRE(packet->stream_index >= 0);
    const auto index = static_cast<std::size_t>(packet->stream_index);
    REQUIRE(index < codecs.size());
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(codecs[index].get(), packet.get()), "提交录制包");
    receive(index);
    packet.Unref();
  }
  REQUIRE(status == AVERROR_EOF);
  for (std::size_t index = 0; index < codecs.size(); ++index) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(codecs[index].get(), nullptr), "排空录制解码器");
    receive(index);
  }
  return result;
}

void CheckMarkers(const Decoded& result) {
  REQUIRE(result.video_markers.size() == result.audio_markers.size());
  REQUIRE_FALSE(result.video_markers.empty());
  for (std::size_t index = 0; index < result.video_markers.size(); ++index) {
    CHECK(std::abs(result.video_markers[index] - result.audio_markers[index]) <=
          1000000);
  }
}

void CheckVideo(const Decoded& decoded, int b_frames = 2) {
  REQUIRE(decoded.video_pts.size() == kVideoFrames);
  CHECK(decoded.b_frames == b_frames);
  REQUIRE(decoded.video_markers.size() == 3);
  const auto shift = b_frames
                         ? av_rescale_q(2, kVideoTimeBase, kNanoseconds)
                         : av_rescale_q(1024, kAudioTimeBase, kNanoseconds);
  for (int index = 0; index < kVideoFrames; ++index) {
    const auto position = static_cast<std::size_t>(index);
    const auto expected =
        shift + av_rescale_q(index, kVideoTimeBase, kNanoseconds);
    CHECK(std::abs(decoded.video_pts[position] - expected) <= 1000000);
    CHECK(decoded.video_high[position] == MarkerFrame(index));
  }
}

void CheckFullRecording(const std::string& path, int b_frames = 2) {
  const auto decoded = DecodeFile(path);
  CheckVideo(decoded, b_frames);
  CHECK(decoded.audio_frames == 565);
  CHECK(decoded.samples == kDecodedSamples);
  CheckMarkers(decoded);
}

std::string InitSegment(const std::string& playlist) {
  const auto index = Read(playlist);
  REQUIRE(index.find("#EXT-X-ENDLIST") != std::string::npos);
  REQUIRE(index.find("#EXT-X-PLAYLIST-TYPE:EVENT") != std::string::npos);
  REQUIRE(index.find("#EXT-X-MEDIA-SEQUENCE:0") != std::string::npos);
  std::smatch match;
  REQUIRE(std::regex_search(index, match,
                            std::regex("#EXT-X-MAP:URI=\"([^\"]+)\"")));
  const auto uri = match[1].str();
  const auto path = fs::u8path(playlist);
  REQUIRE(uri.rfind(path.stem().u8string() + "/", 0) == 0);
  const auto init = path.parent_path() / fs::u8path(uri);
  REQUIRE(fs::exists(init));
  std::istringstream lines(index);
  std::string line;
  int segments = 0;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() == '#') continue;
    REQUIRE(line.rfind(path.stem().u8string() + "/", 0) == 0);
    const auto segment = path.parent_path() / fs::u8path(line);
    REQUIRE(fs::exists(segment));
    REQUIRE(HasMoof(segment.generic_u8string()));
    ++segments;
  }
  REQUIRE(segments >= 2);
  return init.generic_u8string();
}

void CheckName(const std::string& path, const std::string& stem,
               const std::string& extension) {
  const auto filename = fs::u8path(path).filename().u8string();
  CHECK(std::regex_match(filename,
                         std::regex(stem +
                                    "_[0-9]{4}_[0-9]{2}_[0-9]{2}_[0-9]{2}_"
                                    "[0-9]{2}_[0-9]{2}\\." +
                                    extension)));
}

}  // namespace

TEST_CASE("公开Remuxer多目标录制保留B帧AAC同步并完成动态录制尾部",
          "[remuxer][recording][integration]") {
  const std::unique_ptr<mw::streamer::MwStreamerContext,
                        decltype(&mw::streamer::Shutdown)>
      context(mw::streamer::Init(RuntimeConfig()), &mw::streamer::Shutdown);
  OutputDirectory directory;
  encoder_test::Capture capture;
  EncodeMarkers(capture);
  Notifications notifications;
  mw::streamer::Remuxer remuxer;
  notifications.Bind(remuxer);
  remuxer.Start(capture.streams);
  const auto first = remuxer.AddPushUrl(directory.File("single_a.mp4"));
  const auto second = remuxer.AddPushUrl(directory.File("single_b.mp4"));
  const auto hls_a = remuxer.AddPushUrl(directory.File("playlist_a.m3u8"));
  const auto hls_b = remuxer.AddPushUrl(directory.File("playlist_b.m3u8"));
  CheckName(first, "single_a", "mp4");
  CheckName(second, "single_b", "mp4");
  CheckName(hls_a, "playlist_a", "m3u8");
  CheckName(hls_b, "playlist_b", "m3u8");
  REQUIRE(capture.packets.size() == capture.dts_ns.size());
  std::vector<std::size_t> order(capture.packets.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
    return capture.dts_ns[left] < capture.dts_ns[right];
  });
  std::string late;
  for (std::size_t position = 0; position < order.size(); ++position) {
    if (position == order.size() / 2) {
      late = remuxer.AddPushUrl(directory.File("late.mp4"));
      CheckName(late, "late", "mp4");
    }
    const auto index = order[position];
    REQUIRE(
        remuxer.SubmitPacket(capture.packets[index], capture.dts_ns[index]));
  }
  remuxer.Drain();
  REQUIRE(notifications.Wait());
  remuxer.Stop();
  remuxer.Drain();
  remuxer.Stop();
  CHECK(notifications.ended == 1);
  CHECK_FALSE(notifications.callback_failed);
  const auto error_text = std::accumulate(
      notifications.errors.begin(), notifications.errors.end(), std::string{},
      [](const auto& text, const auto& error) { return text + error + "\n"; });
  INFO(error_text);
  REQUIRE(notifications.errors.empty());
  CHECK_FALSE(
      remuxer.SubmitPacket(capture.packets.front(), capture.dts_ns.front()));
  REQUIRE(HasMoof(first));
  REQUIRE(HasMoof(second));
  REQUIRE(HasMoof(late));
  CheckFullRecording(first);
  CheckFullRecording(second);
  CheckFullRecording(hls_a);
  CheckFullRecording(hls_b);
  const auto init_a = InitSegment(hls_a);
  const auto init_b = InitSegment(hls_b);
  CHECK(init_a != init_b);
  const auto partial = DecodeFile(late);
  REQUIRE(partial.video_pts.size() < kVideoFrames);
  REQUIRE(partial.video_pts.size() >= 90);
  REQUIRE(partial.video_markers.size() >= 1);
  CheckMarkers(partial);
  const auto start = kVideoFrames - static_cast<int>(partial.video_pts.size());
  for (std::size_t index = 0; index < partial.video_high.size(); ++index) {
    CHECK(partial.video_high[index] ==
          MarkerFrame(start + static_cast<int>(index)));
  }
}

TEST_CASE("公开Remuxer控制调用校验与空会话结束只通知一次",
          "[remuxer][recording][lifecycle]") {
  const std::unique_ptr<mw::streamer::MwStreamerContext,
                        decltype(&mw::streamer::Shutdown)>
      context(mw::streamer::Init(RuntimeConfig()), &mw::streamer::Shutdown);
  OutputDirectory directory;
  Notifications notifications;
  mw::streamer::Remuxer remuxer;
  notifications.Bind(remuxer);
  SECTION("Start前不能添加录制目标") {
    CHECK_THROWS_AS(remuxer.AddPushUrl(directory.File("early.mp4")),
                    std::logic_error);
  }
  SECTION("空会话支持DrainStop并拒绝无效输出扩展名") {
    encoder_test::Capture capture;
    mw::streamer::Encoder encoder;
    capture.Bind(encoder);
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    encoder.Start(encoder_test::Config(), {encoder_test::VideoStream()}, cpu);
    encoder.Drain();
    REQUIRE(capture.Wait());
    encoder.Stop();
    REQUIRE(capture.errors == 0);
    remuxer.Start(capture.streams);
    CHECK_THROWS_AS(remuxer.AddPushUrl(directory.File("invalid.ts")),
                    std::invalid_argument);
    remuxer.Drain();
    REQUIRE(notifications.Wait());
    remuxer.Stop();
    remuxer.Drain();
    remuxer.Stop();
    CHECK(notifications.ended == 1);
    CHECK(notifications.errors.empty());
    CHECK_FALSE(notifications.callback_failed);
    CHECK_THROWS_AS(remuxer.AddPushUrl(directory.File("ended.mp4")),
                    std::logic_error);
  }
}

TEST_CASE("声明双轨但EOF仅收到一路时录制保留超过启动缓存上限的包",
          "[remuxer][recording][eof][integration]") {
  const std::unique_ptr<mw::streamer::MwStreamerContext,
                        decltype(&mw::streamer::Shutdown)>
      context(mw::streamer::Init(RuntimeConfig()), &mw::streamer::Shutdown);
  OutputDirectory directory;
  encoder_test::Capture capture;
  EncodeMarkers(capture);
  AVMediaType selected = AVMEDIA_TYPE_UNKNOWN;
  SECTION("仅收到360个视频包") { selected = AVMEDIA_TYPE_VIDEO; }
  SECTION("仅收到565个AAC包且保留priming") { selected = AVMEDIA_TYPE_AUDIO; }
  REQUIRE(selected != AVMEDIA_TYPE_UNKNOWN);
  const auto stream = std::find_if(
      capture.streams.begin(), capture.streams.end(), [&](const auto& value) {
        return value.codec_parameters.get()->codec_type == selected;
      });
  REQUIRE(stream != capture.streams.end());
  Notifications notifications;
  mw::streamer::Remuxer remuxer;
  notifications.Bind(remuxer);
  remuxer.Start(capture.streams);
  const auto path = remuxer.AddPushUrl(directory.File("partial_track.mp4"));
  std::size_t submitted = 0;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    if (capture.packets[index]->stream_index != stream->stream_index) continue;
    REQUIRE(
        remuxer.SubmitPacket(capture.packets[index], capture.dts_ns[index]));
    ++submitted;
  }
  REQUIRE(submitted > 100);
  remuxer.Drain();
  REQUIRE(notifications.Wait());
  remuxer.Stop();
  CHECK(notifications.ended == 1);
  CHECK_FALSE(notifications.callback_failed);
  const auto error_text = std::accumulate(
      notifications.errors.begin(), notifications.errors.end(), std::string{},
      [](const auto& text, const auto& error) { return text + error + "\n"; });
  INFO(error_text);
  REQUIRE(notifications.errors.empty());
  REQUIRE(HasMoof(path));
  const auto decoded = DecodeFile(path, 1);
  if (selected == AVMEDIA_TYPE_VIDEO) {
    CHECK(submitted == kVideoFrames);
    CheckVideo(decoded);
    CHECK(decoded.audio_frames == 0);
    CHECK(decoded.samples == 0);
  } else {
    CHECK(submitted == 565);
    CHECK(decoded.video_pts.empty());
    CHECK(decoded.audio_frames == 565);
    CHECK(decoded.samples == kDecodedSamples);
    REQUIRE(decoded.audio_markers.size() == 3);
    for (std::size_t index = 0; index < decoded.audio_markers.size(); ++index) {
      const auto expected =
          av_rescale_q(static_cast<std::int64_t>(index + 1) * 90,
                       kVideoTimeBase, kNanoseconds) +
          av_rescale_q(1024, kAudioTimeBase, kNanoseconds);
      CHECK(std::abs(decoded.audio_markers[index] - expected) <= 1000000);
    }
  }
}

TEST_CASE("零B帧与AAC负priming启动回放使用共同媒体时间起点",
          "[remuxer][recording][startup][integration]") {
  const std::unique_ptr<mw::streamer::MwStreamerContext,
                        decltype(&mw::streamer::Shutdown)>
      context(mw::streamer::Init(RuntimeConfig()), &mw::streamer::Shutdown);
  OutputDirectory directory;
  encoder_test::Capture capture;
  EncodeMarkers(capture, 0);
  Notifications notifications;
  mw::streamer::Remuxer remuxer;
  notifications.Bind(remuxer);
  remuxer.Start(capture.streams);
  const auto path = remuxer.AddPushUrl(directory.File("zero_b_frames.mp4"));
  const auto playlist =
      remuxer.AddPushUrl(directory.File("zero_b_frames.m3u8"));
  std::vector<std::size_t> order(capture.packets.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
    return capture.dts_ns[left] < capture.dts_ns[right];
  });
  for (const auto index : order) {
    REQUIRE(
        remuxer.SubmitPacket(capture.packets[index], capture.dts_ns[index]));
  }
  remuxer.Drain();
  REQUIRE(notifications.Wait());
  remuxer.Stop();
  CHECK(notifications.ended == 1);
  CHECK_FALSE(notifications.callback_failed);
  const auto error_text = std::accumulate(
      notifications.errors.begin(), notifications.errors.end(), std::string{},
      [](const auto& text, const auto& error) { return text + error + "\n"; });
  INFO(error_text);
  REQUIRE(notifications.errors.empty());
  REQUIRE(HasMoof(path));
  CheckFullRecording(path, 0);
  REQUIRE_FALSE(InitSegment(playlist).empty());
  CheckFullRecording(playlist, 0);
}
