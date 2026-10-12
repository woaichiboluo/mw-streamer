#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mw/streamer/input/ffmpeg_input.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::DecodedInputSource;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputMode;
using mw::streamer::InputState;
namespace ffmpeg = mw::streamer::ffmpeg;

void RequireMedia(int result, const char* operation) {
  ffmpeg::FfmpegException::throwIfError(result, operation);
}

// Copy real H.264/AAC packets and add PCM. A forced AAC decoder must fail if
// Input initializes the unselected PCM track, even if its frames are discarded.
class MultiTrackFile final {
 public:
  explicit MultiTrackFile(int pcm_disposition = 0,
                          const char* source = "h264_aac.mp4",
                          bool competing_defaults = false)
      : path_(
            std::filesystem::temp_directory_path() /
            ("mw-input-selection-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".mkv")) {
    Build(pcm_disposition, source, competing_defaults);
  }
  ~MultiTrackFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  std::string path() const { return path_.string(); }

 private:
  void Build(int pcm_disposition, const char* sample, bool competing_defaults) {
    AVFormatContext* raw = nullptr;
    const auto source =
        std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/" + sample;
    RequireMedia(avformat_open_input(&raw, source.c_str(), nullptr, nullptr),
                 "打开多轨参考源");
    const auto close_input = [](AVFormatContext* value) {
      avformat_close_input(&value);
    };
    std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw,
                                                                  close_input);
    RequireMedia(avformat_find_stream_info(raw, nullptr), "读取多轨参考信息");
    AVFormatContext* target = nullptr;
    RequireMedia(avformat_alloc_output_context2(&target, nullptr, "matroska",
                                                path().c_str()),
                 "创建多轨容器");
    const auto close_output = [](AVFormatContext* value) {
      avio_closep(&value->pb);
      avformat_free_context(value);
    };
    std::unique_ptr<AVFormatContext, decltype(close_output)> output(
        target, close_output);
    for (unsigned index = 0; index < raw->nb_streams; ++index) {
      auto* stream = avformat_new_stream(target, nullptr);
      if (!stream) throw std::bad_alloc();
      RequireMedia(avcodec_parameters_copy(stream->codecpar,
                                           raw->streams[index]->codecpar),
                   "复制多轨参数");
      stream->codecpar->codec_tag = 0;
      stream->time_base = raw->streams[index]->time_base;
      stream->disposition =
          competing_defaults ? AV_DISPOSITION_FORCED : AV_DISPOSITION_DEFAULT;
    }
    AVStream* extra_video = nullptr;
    if (competing_defaults) {
      extra_video = avformat_new_stream(target, nullptr);
      if (!extra_video) throw std::bad_alloc();
      RequireMedia(avcodec_parameters_copy(extra_video->codecpar,
                                           raw->streams[0]->codecpar),
                   "复制额外视频参数");
      extra_video->codecpar->codec_tag = 0;
      extra_video->time_base = raw->streams[0]->time_base;
      extra_video->disposition = AV_DISPOSITION_DEFAULT;
    }
    auto* pcm = avformat_new_stream(target, nullptr);
    if (!pcm) throw std::bad_alloc();
    pcm->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    pcm->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    pcm->codecpar->format = AV_SAMPLE_FMT_S16;
    pcm->codecpar->sample_rate = 8000;
    pcm->codecpar->bits_per_coded_sample = 16;
    pcm->codecpar->block_align = 2;
    av_channel_layout_default(&pcm->codecpar->ch_layout, 1);
    pcm->time_base = {1, 8000};
    pcm->disposition =
        competing_defaults ? AV_DISPOSITION_DEFAULT : pcm_disposition;
    RequireMedia(av_dict_set(&pcm->metadata, "language", "zho", 0),
                 "设置非默认轨道信息");
    RequireMedia(avio_open(&target->pb, path().c_str(), AVIO_FLAG_WRITE),
                 "打开多轨容器");
    RequireMedia(avformat_write_header(target, nullptr), "写入多轨容器头");
    for (;;) {
      ffmpeg::Packet packet;
      const int status = av_read_frame(raw, packet.get());
      if (status == AVERROR_EOF) break;
      RequireMedia(status, "读取多轨源包");
      const int index = packet->stream_index;
      const auto* source_stream = raw->streams[index];
      if (extra_video && index == 0) {
        auto video = packet.Ref();
        video->stream_index = extra_video->index;
        video->pos = -1;
        av_packet_rescale_ts(video.get(), source_stream->time_base,
                             extra_video->time_base);
        RequireMedia(av_interleaved_write_frame(target, video.get()),
                     "写入额外视频包");
      }
      if (source_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
          packet->pts >= 0) {
        ffmpeg::Packet audio;
        const auto samples =
            av_rescale_q(packet->duration, source_stream->time_base, {1, 8000});
        if (samples > 0) {
          RequireMedia(
              av_new_packet(audio.get(), static_cast<int>(samples * 2)),
              "创建额外 PCM 包");
          std::memset(audio->data, 0, static_cast<std::size_t>(audio->size));
          audio->stream_index = pcm->index;
          audio->pts = audio->dts =
              av_rescale_q(packet->pts, source_stream->time_base, {1, 8000});
          audio->duration = samples;
          av_packet_rescale_ts(audio.get(), {1, 8000}, pcm->time_base);
          RequireMedia(av_interleaved_write_frame(target, audio.get()),
                       "写入额外 PCM 包");
        }
      }
      packet->pos = -1;
      av_packet_rescale_ts(packet.get(), source_stream->time_base,
                           target->streams[index]->time_base);
      RequireMedia(av_interleaved_write_frame(target, packet.get()),
                   "写入原始音视频包");
    }
    RequireMedia(av_write_trailer(target), "写入多轨容器尾");
  }

  std::filesystem::path path_;
};

struct SelectionCapture {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ffmpeg::StreamInfo> discovered;
  std::vector<std::vector<ffmpeg::StreamInfo>> ready;
  std::vector<DecodedInputSource> sources;
  std::map<int, int> frames;
  std::set<int> packets;
  std::set<int> ends;
  int selections = 0;
  bool ended = false;
  bool failed = false;
  std::string error;

  void Attach(FfmpegInput& input, bool pull = false) {
    input.SetOnReady([this](const auto& streams) { ready.push_back(streams); });
    input.SetOnPacket([this](std::uint64_t, const auto& packet) {
      packets.insert(packet->stream_index);
    });
    input.SetOnTrackEnded(
        [this](int index, std::int64_t) { ends.insert(index); });
    if (pull) {
      input.SetOnDecodedReady([this](const auto& source) {
        std::lock_guard<std::mutex> lock(mutex);
        sources.push_back(source);
        changed.notify_all();
      });
    } else {
      input.SetOnFrame([this](int index, const auto&) {
        std::lock_guard<std::mutex> lock(mutex);
        ++frames[index];
        changed.notify_all();
      });
    }
    input.SetOnStateChanged(
        [this](InputState state, int, std::string_view reason) {
          std::lock_guard<std::mutex> lock(mutex);
          ended |= state == InputState::kEnded;
          failed |= state == InputState::kFailed;
          if (state == InputState::kFailed) error = reason;
          changed.notify_all();
        });
  }
  bool WaitTerminal() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 8s, [&] { return ended || failed; });
  }
  bool WaitSources(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 8s,
                            [&] { return sources.size() >= count || failed; });
  }
  int FindAudio(AVCodecID codec) const {
    const auto found = std::find_if(
        discovered.begin(), discovered.end(), [codec](const auto& stream) {
          return stream.codec_parameters.get()->codec_id == codec;
        });
    if (found == discovered.end()) throw std::runtime_error("缺少所选音轨");
    return found->stream_index;
  }
};

TEST_CASE("Input 只创建下游选中轨道的解码器", "[input][selection][decode]") {
  const auto mode = GENERATE(InputMode::kLive, InputMode::kBatch);
  MultiTrackFile file;
  SelectionCapture capture;
  FfmpegInputConfig config;
  config.mode = mode;
  config.audio_decoder_name = "aac";
  FfmpegInput input(config);
  capture.Attach(input);
  int audio = -1;
  input.SetOnSelectStreams([&](const auto& streams) {
    capture.discovered = streams;
    ++capture.selections;
    audio = capture.FindAudio(AV_CODEC_ID_AAC);
    return std::vector<int>{0, audio};
  });
  input.Start(file.path());
  REQUIRE(capture.WaitTerminal());
  input.Stop();
  INFO(capture.error);
  REQUIRE_FALSE(capture.failed);
  REQUIRE(capture.ended);
  CHECK(capture.selections == 1);
  REQUIRE(capture.discovered.size() == 3);
  REQUIRE(capture.ready.size() == 1);
  CHECK(capture.ready.front().size() == 2);
  CHECK(capture.frames.size() == 2);
  CHECK(capture.frames.at(0) > 0);
  CHECK(capture.frames.at(audio) > 0);
  CHECK(capture.packets == std::set<int>{0, audio});
  CHECK(capture.ends == std::set<int>{0, audio});
}

TEST_CASE("Input 显式选择非 DEFAULT 轨道并只提供对应按轨源",
          "[input][selection][pull]") {
  MultiTrackFile file(AV_DISPOSITION_FORCED);
  SelectionCapture capture;
  FfmpegInputConfig config;
  config.audio_decoder_name = "pcm_s16le";
  FfmpegInput input(config);
  capture.Attach(input, true);
  int audio = -1;
  input.SetOnSelectStreams([&](const auto& streams) {
    capture.discovered = streams;
    audio = capture.FindAudio(AV_CODEC_ID_PCM_S16LE);
    return std::vector<int>{audio};
  });
  input.Start(file.path());
  REQUIRE(capture.WaitSources(1));
  DecodedInputSource source;
  {
    std::lock_guard<std::mutex> lock(capture.mutex);
    REQUIRE_FALSE(capture.sources.empty());
    source = capture.sources.front();
  }
  REQUIRE(source.tracks.size() == 1);
  const auto& track = source.tracks.front();
  CHECK(track.stream.stream_index == audio);
  CHECK(track.stream.disposition == AV_DISPOSITION_FORCED);
  CHECK(track.stream.metadata.at("language") == "zho");
  int samples = 0;
  int frames = 0;
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!track.finished()) {
    ffmpeg::Frame frame;
    if (track.try_receive(frame)) {
      samples += frame->nb_samples;
      ++frames;
      CHECK(frame->sample_rate == 8000);
    } else {
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
  }
  REQUIRE(source.complete);
  source.complete(frames);
  REQUIRE(capture.WaitTerminal());
  input.Stop();
  INFO(capture.error);
  CHECK_FALSE(capture.failed);
  CHECK(samples > 0);
  REQUIRE(capture.ready.size() == 1);
  REQUIRE(capture.ready.front().size() == 1);
  CHECK(capture.ready.front().front().stream_index == audio);
  CHECK(capture.packets == std::set<int>{audio});
  CHECK(capture.ends == std::set<int>{audio});
}

TEST_CASE("Input 默认按轨道顺序选择第一条视频和音频且忽略 DEFAULT 标记",
          "[input][selection][default]") {
  const auto mode =
      GENERATE(InputMode::kLive, InputMode::kBatch, InputMode::kRemux);
  const bool clear_callback = GENERATE(false, true);
  CAPTURE(mode, clear_callback);
  MultiTrackFile file(0, "h264_aac.mp4", true);
  SelectionCapture capture;
  FfmpegInputConfig config;
  config.mode = mode;
  config.audio_decoder_name = "aac";
  FfmpegInput input(config);
  capture.Attach(input);
  if (clear_callback) {
    input.SetOnSelectStreams([&](const auto&) -> std::vector<int> {
      ++capture.selections;
      throw std::runtime_error("已移除的选轨回调不得执行");
    });
    input.SetOnSelectStreams({});
  }
  input.Start(file.path());
  REQUIRE(capture.WaitTerminal());
  input.Stop();
  INFO(capture.error);
  REQUIRE_FALSE(capture.failed);
  CHECK(capture.ended);
  CHECK(capture.selections == 0);
  REQUIRE(capture.ready.size() == 1);
  REQUIRE(capture.ready.front().size() == 2);
  CHECK(capture.ready.front()[0].stream_index == 0);
  CHECK(capture.ready.front()[1].stream_index == 1);
  CHECK(capture.ready.front()[0].disposition == AV_DISPOSITION_FORCED);
  CHECK(capture.ready.front()[1].disposition == AV_DISPOSITION_FORCED);
  CHECK(capture.packets == std::set<int>{0, 1});
  if (mode == InputMode::kRemux) {
    CHECK(capture.frames.empty());
    CHECK(capture.ends.empty());
  } else {
    REQUIRE(capture.frames.size() == 2);
    CHECK(capture.frames.at(0) > 0);
    CHECK(capture.frames.at(1) > 0);
    CHECK(capture.ends == std::set<int>{0, 1});
  }
}

TEST_CASE("Input 无效选轨在包帧交付前失败", "[input][selection][validation]") {
  const int invalid = GENERATE(0, 1, 2, 3, 4);
  CAPTURE(invalid);
  MultiTrackFile file;
  SelectionCapture capture;
  FfmpegInput input;
  capture.Attach(input);
  input.SetOnSelectStreams([&](const auto& streams) -> std::vector<int> {
    capture.discovered = streams;
    if (invalid == 0) return {};
    if (invalid == 1) return {0, 0};
    if (invalid == 2) return {-1};
    if (invalid == 3) return {9999};
    throw std::runtime_error("选轨回调拒绝当前源");
  });
  input.Start(file.path());
  REQUIRE(capture.WaitTerminal());
  input.Stop();
  CHECK(capture.failed);
  CHECK_FALSE(capture.error.empty());
  CHECK(capture.ready.empty());
  CHECK(capture.packets.empty());
  CHECK(capture.frames.empty());
  CHECK(capture.ends.empty());
}

TEST_CASE("Input Remux 选轨不创建解码器", "[input][selection][remux]") {
  MultiTrackFile file;
  SelectionCapture capture;
  FfmpegInputConfig config;
  config.mode = InputMode::kRemux;
  config.video_decoder_name = "invalid_decoder";
  config.audio_decoder_name = "invalid_decoder";
  FfmpegInput input(config);
  capture.Attach(input);
  int audio = -1;
  input.SetOnSelectStreams([&](const auto& streams) {
    capture.discovered = streams;
    ++capture.selections;
    audio = capture.FindAudio(AV_CODEC_ID_AAC);
    return std::vector<int>{audio};
  });
  input.Start(file.path());
  REQUIRE(capture.WaitTerminal());
  input.Stop();
  CHECK_FALSE(capture.failed);
  CHECK(capture.ended);
  CHECK(capture.selections == 1);
  CHECK(capture.packets == std::set<int>{audio});
  CHECK(capture.frames.empty());
  CHECK(capture.ends.empty());
  REQUIRE(capture.ready.size() == 1);
  CHECK(capture.ready.front().size() == 1);
}

TEST_CASE("Input 循环和 Seek 重建解码器前重新选轨",
          "[input][selection][lifecycle]") {
  const bool loop = GENERATE(false, true);
  CAPTURE(loop);
  MultiTrackFile file(0, "seek_h264_aac.mp4");
  SelectionCapture capture;
  FfmpegInputConfig config;
  config.loop = loop;
  FfmpegInput input(config);
  capture.Attach(input);
  int first_audio = -1;
  int second_audio = -1;
  input.SetOnSelectStreams([&](const auto& streams) {
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.discovered = streams;
    ++capture.selections;
    first_audio = capture.FindAudio(AV_CODEC_ID_AAC);
    second_audio = capture.FindAudio(AV_CODEC_ID_PCM_S16LE);
    return std::vector<int>{capture.selections == 1 ? first_audio
                                                    : second_audio};
  });
  bool seek_requested = false;
  input.SetOnFrame([&](int index, const auto&) {
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      ++capture.frames[index];
      capture.changed.notify_all();
    }
    if (!loop && !seek_requested) {
      seek_requested = true;
      input.Seek(6s);
    }
  });
  input.Start(file.path());
  if (loop) {
    std::unique_lock<std::mutex> lock(capture.mutex);
    REQUIRE(capture.changed.wait_for(lock, 8s, [&] {
      return capture.frames.count(second_audio) > 0 || capture.failed;
    }));
  } else {
    REQUIRE(capture.WaitTerminal());
  }
  input.Stop();
  INFO(capture.error);
  CHECK_FALSE(capture.failed);
  CHECK(capture.selections >= 2);
  CHECK(capture.frames.at(first_audio) > 0);
  CHECK(capture.frames.at(second_audio) > 0);
  REQUIRE(capture.ready.size() >= 2);
  CHECK(capture.ready[0].front().stream_index == first_audio);
  CHECK(capture.ready[1].front().stream_index == second_audio);
  if (loop) CHECK_FALSE(capture.ended);
}

TEST_CASE("按轨 Seek 在解码前重新选轨且丢弃旧队列不伪装完整 EOF",
          "[input][selection][pull][seek]") {
  MultiTrackFile file(0, "seek_h264_aac.mp4");
  SelectionCapture capture;
  FfmpegInput input;
  capture.Attach(input, true);
  input.SetOnSelectStreams([&](const auto& streams) {
    capture.discovered = streams;
    ++capture.selections;
    return std::vector<int>{capture.FindAudio(
        capture.selections == 1 ? AV_CODEC_ID_AAC : AV_CODEC_ID_PCM_S16LE)};
  });
  const auto source_at = [&](std::size_t index) {
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.sources.at(index);
  };
  const auto receive = [](const auto& track, ffmpeg::Frame& frame) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (track.try_receive(frame)) return true;
      if (track.finished()) return false;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  };
  input.Start(file.path());
  REQUIRE(capture.WaitSources(1));
  const auto first = source_at(0);
  REQUIRE(first.tracks.size() == 1);
  CHECK(first.tracks.front().stream.codec_parameters.get()->codec_id ==
        AV_CODEC_ID_AAC);
  CHECK_FALSE(first.flush);
  ffmpeg::Frame initial;
  REQUIRE(receive(first.tracks.front(), initial));
  input.Seek(405ms);
  REQUIRE(capture.WaitSources(2));
  const auto second = source_at(1);
  REQUIRE(second.tracks.size() == 1);
  const auto& track = second.tracks.front();
  CHECK(track.stream.codec_parameters.get()->codec_id == AV_CODEC_ID_PCM_S16LE);
  CHECK(second.media_origin_us == first.media_origin_us);
  CHECK(second.start_time_us == first.media_origin_us + 405000);
  CHECK(second.start_time_us + second.duration_us ==
        first.start_time_us + first.duration_us);
  CHECK(second.flush);
  CHECK_FALSE(second.loop);
  CHECK(first.tracks.front().finished());
  CHECK(first.tracks.front().end_pts() == AV_NOPTS_VALUE);
  ffmpeg::Frame discarded;
  CHECK_FALSE(first.tracks.front().try_receive(discarded));
  ffmpeg::Frame decoded;
  REQUIRE(receive(track, decoded));
  CHECK(decoded->sample_rate == 8000);
  CHECK(decoded->pts == decoded->best_effort_timestamp);
  CHECK(av_cmp_q(decoded->time_base, track.stream.time_base) == 0);
  int consumed = 1;
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!track.finished()) {
    ffmpeg::Frame frame;
    if (track.try_receive(frame)) {
      ++consumed;
    } else {
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
  }
  REQUIRE(consumed > 2);
  REQUIRE(second.complete);
  second.complete(consumed);
  REQUIRE(capture.WaitTerminal());
  input.Stop();
  INFO(capture.error);
  CHECK_FALSE(capture.failed);
  CHECK(capture.ended);
  CHECK(capture.selections == 2);
  REQUIRE(capture.ready.size() == 2);
  CHECK(capture.ready[0].size() == 1);
  CHECK(capture.ready[1].size() == 1);
  CHECK(capture.ready[0].front().stream_index !=
        capture.ready[1].front().stream_index);
  CHECK_NOTHROW(first.complete(0));
  CHECK_NOTHROW(second.complete(consumed));
}

}  // namespace
