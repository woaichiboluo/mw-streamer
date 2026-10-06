#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/input/ffmpeg_input.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputState;

std::string PacketSamplePath() {
  return std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/h264_aac.mp4";
}

struct PacketRecord {
  int index;
  int64_t pts;
  int64_t dts;
  int64_t duration;
  int flags;
  std::vector<uint8_t> bytes;
  std::vector<std::pair<int, std::vector<uint8_t>>> side_data;

  bool operator==(const PacketRecord& other) const {
    return index == other.index && pts == other.pts && dts == other.dts &&
           duration == other.duration && flags == other.flags &&
           bytes == other.bytes && side_data == other.side_data;
  }
};

PacketRecord RecordPacket(const AVPacket& packet) {
  PacketRecord result{
      packet.stream_index, packet.pts, packet.dts, packet.duration,
      packet.flags,        {},         {}};
  if (packet.size > 0) {
    result.bytes.assign(packet.data, packet.data + packet.size);
  }
  for (int i = 0; i < packet.side_data_elems; ++i) {
    const auto& data = packet.side_data[i];
    std::vector<uint8_t> bytes;
    if (data.size) bytes.assign(data.data, data.data + data.size);
    result.side_data.emplace_back(static_cast<int>(data.type),
                                  std::move(bytes));
  }
  return result;
}

struct RawMedia {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
};

struct ClosePacketInput {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

// The packet oracle uses av_read_frame directly, without Input or Decoder.
RawMedia ReadRawMedia(
    const std::string& path,
    std::optional<std::chrono::milliseconds> position = std::nullopt) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开原始包参考文件");
  std::unique_ptr<AVFormatContext, ClosePacketInput> input(raw);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "读取原始包参考轨道");
  RawMedia media;
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    const int index = av_find_best_stream(raw, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) continue;
    ffmpeg::FfmpegException::throwIfError(index, "选择原始包参考轨道");
    const auto* stream = raw->streams[index];
    media.streams.push_back(
        {index, ffmpeg::CodecParameters(*stream->codecpar), stream->time_base});
  }
  if (position) {
    const auto& stream = media.streams.at(0);
    auto target = av_rescale_q(position->count(), {1, 1000}, AV_TIME_BASE_Q);
    if (raw->start_time != AV_NOPTS_VALUE) target += raw->start_time;
    target = av_rescale_q(target, AV_TIME_BASE_Q, stream.time_base);
    ffmpeg::FfmpegException::throwIfError(
        av_seek_frame(raw, stream.stream_index, target, AVSEEK_FLAG_BACKWARD),
        "定位原始参考包");
  }
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(raw, packet.get());
    if (result == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(result, "读取原始参考包");
    if (std::any_of(media.streams.begin(), media.streams.end(),
                    [&](const auto& stream) {
                      return stream.stream_index == packet->stream_index;
                    })) {
      media.packets.push_back(std::move(packet));
    }
  }
  return media;
}

using PacketRecords = std::map<int, std::vector<PacketRecord>>;

PacketRecords GroupPackets(const std::vector<ffmpeg::Packet>& packets) {
  PacketRecords result;
  for (const auto& packet : packets) {
    result[packet->stream_index].push_back(RecordPacket(*packet.get()));
  }
  return result;
}

// A second independent native decoding loop proves the recording is usable.
std::map<AVMediaType, std::vector<int64_t>> DecodeRawMedia(
    const RawMedia& media) {
  std::map<AVMediaType, std::vector<int64_t>> result;
  for (const auto& stream : media.streams) {
    const auto* parameters = stream.codec_parameters.get();
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) throw std::runtime_error("缺少参考软件解码器");
    ffmpeg::CodecContext context(codec);
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context.get(), parameters),
        "设置录像参考解码器");
    context.get()->pkt_timebase = stream.time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(context.get(), codec, nullptr), "打开录像参考解码器");
    ffmpeg::Frame frame;
    const auto receive = [&] {
      for (;;) {
        const int received = avcodec_receive_frame(context.get(), frame.get());
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return;
        ffmpeg::FfmpegException::throwIfError(received, "解码录像参考帧");
        result[parameters->codec_type].push_back(av_rescale_q(
            frame->best_effort_timestamp, stream.time_base, {1, 1000000000}));
      }
    };
    for (const auto& packet : media.packets) {
      if (packet->stream_index != stream.stream_index) continue;
      ffmpeg::FfmpegException::throwIfError(
          avcodec_send_packet(context.get(), packet.get()), "发送录像参考包");
      receive();
    }
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(context.get(), nullptr), "刷新录像参考尾帧");
    receive();
  }
  return result;
}

class PacketCollector final {
 public:
  void Attach(FfmpegInput& input,
              std::function<void(const std::vector<ffmpeg::StreamInfo>&)>
                  ready_hook = {},
              std::function<void(const ffmpeg::Packet&)> packet_hook = {}) {
    input.SetOnReady([this, ready_hook](const auto& streams) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++ready;
      stream_info = streams;
      thread_ = std::this_thread::get_id();
      try {
        if (ready_hook) ready_hook(streams);
      } catch (...) {
        error = std::current_exception();
      }
    });
    input.SetOnPacket(
        [this, packet_hook](std::uint64_t generation, const auto& packet) {
          std::lock_guard<std::mutex> lock(mutex_);
          valid &= ready == 1 && std::this_thread::get_id() == thread_;
          packets.push_back(packet.Ref());
          generations.push_back(generation);
          try {
            if (packet_hook && !error) packet_hook(packet);
          } catch (...) {
            error = std::current_exception();
          }
        });
    input.SetOnFrame([this](int index, const auto& frame) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++frames;
      // A frame's originating packet must already have reached OnPacket.
      valid &= std::any_of(packets.begin(), packets.end(), [&](const auto& p) {
        return p->stream_index == index &&
               p->pts == frame->best_effort_timestamp;
      });
    });
    input.SetOnStateChanged([this](InputState state, int, std::string_view) {
      std::lock_guard<std::mutex> lock(mutex_);
      states.push_back(state);
      changed_.notify_all();
    });
  }

  bool WaitForEnd() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 5s, [&] {
      return !states.empty() && (states.back() == InputState::kEnded ||
                                 states.back() == InputState::kFailed);
    });
  }

  // Inspected only after Stop joins both workers (or Input is destroyed).
  std::vector<ffmpeg::StreamInfo> stream_info;
  std::vector<ffmpeg::Packet> packets;
  std::vector<std::uint64_t> generations;
  std::vector<InputState> states;
  std::exception_ptr error;
  size_t ready = 0;
  size_t frames = 0;
  bool valid = true;

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread::id thread_;
};

class PacketRecording final {
 public:
  PacketRecording()
      : path_(
            std::filesystem::temp_directory_path() /
            ("mw-streamer-packets-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".mp4")) {}

  ~PacketRecording() {
    if (output_) {
      avio_closep(&output_->pb);
      avformat_free_context(output_);
    }
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  void Open(const std::vector<ffmpeg::StreamInfo>& streams) {
    ffmpeg::FfmpegException::throwIfError(
        avformat_alloc_output_context2(&output_, nullptr, "mp4",
                                       path().c_str()),
        "创建包回调录像");
    for (const auto& stream : streams) {
      auto* target = avformat_new_stream(output_, nullptr);
      if (!target) throw std::bad_alloc();
      ffmpeg::FfmpegException::throwIfError(
          avcodec_parameters_copy(target->codecpar,
                                  stream.codec_parameters.get()),
          "复制录像轨道参数");
      target->codecpar->codec_tag = 0;
      target->time_base = stream.time_base;
      mapping_[stream.stream_index] = {target->index, stream.time_base};
    }
    ffmpeg::FfmpegException::throwIfError(
        avio_open(&output_->pb, path().c_str(), AVIO_FLAG_WRITE),
        "打开包回调录像");
    ffmpeg::FfmpegException::throwIfError(
        avformat_write_header(output_, nullptr), "写入录像头");
  }

  void Write(const ffmpeg::Packet& source) {
    auto copy = source.Ref();
    const auto& mapping = mapping_.at(source->stream_index);
    copy->stream_index = mapping.first;
    copy->pos = -1;
    av_packet_rescale_ts(copy.get(), mapping.second,
                         output_->streams[mapping.first]->time_base);
    ffmpeg::FfmpegException::throwIfError(
        av_interleaved_write_frame(output_, copy.get()), "写入录像包");
  }

  void Finish() {
    ffmpeg::FfmpegException::throwIfError(av_write_trailer(output_),
                                          "写入录像尾");
    ffmpeg::FfmpegException::throwIfError(avio_closep(&output_->pb),
                                          "关闭包回调录像");
  }

  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
  AVFormatContext* output_ = nullptr;
  std::map<int, std::pair<int, AVRational>> mapping_;
};

// Declare after Input so every assertion failure releases the callback before
// Input destruction joins its worker. The callback owns the gate's state.
class PacketPause final {
 public:
  explicit PacketPause(size_t packet_count = 1)
      : state_(std::make_shared<State>(packet_count)) {}
  ~PacketPause() { Release(); }

  std::function<void(const ffmpeg::Packet&)> hook() const {
    return [state = state_](const ffmpeg::Packet&) {
      std::unique_lock<std::mutex> lock(state->mutex);
      if (++state->packets_seen != state->pause_at) return;
      state->paused = true;
      state->changed.notify_all();
      state->changed.wait(lock, [&] { return state->released; });
    };
  }

  bool Wait() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(lock, 2s, [&] { return state_->paused; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->released = true;
    state_->changed.notify_all();
  }

 private:
  struct State {
    explicit State(size_t packet_count) : pause_at(packet_count) {}
    std::mutex mutex;
    std::condition_variable changed;
    size_t packets_seen = 0;
    size_t pause_at;
    bool paused = false;
    bool released = false;
  };
  std::shared_ptr<State> state_;
};

TEST_CASE("Input packet callbacks preserve native packets in both decode modes",
          "[input][packet]") {
  bool decode = true;
  SECTION("decode with packet and frame delivery") {}
  SECTION("packet only ignores decoder selection") { decode = false; }
  const auto reference = ReadRawMedia(PacketSamplePath());
  const auto expected = GroupPackets(reference.packets);
  REQUIRE(expected.size() == 2);
  REQUIRE(std::any_of(
      reference.packets.begin(), reference.packets.end(),
      [](const auto& packet) { return packet->side_data_elems > 0; }));
  PacketCollector collector;
  std::map<int, std::vector<PacketRecord>> before_destroy;
  const auto started = std::chrono::steady_clock::now();
  {
    FfmpegInputConfig config;
    config.decode = decode;
    if (!decode) {
      config.video_decoder_name = "missing_video_decoder";
      config.audio_decoder_name = "missing_audio_decoder";
    }
    FfmpegInput input(config);
    collector.Attach(input);
    input.Start(PacketSamplePath());
    REQUIRE(collector.WaitForEnd());
    input.Stop();
    before_destroy = GroupPackets(collector.packets);
  }
  if (collector.error) std::rethrow_exception(collector.error);
  CHECK(collector.valid);
  CHECK(collector.ready == 1);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kEnded) == 1);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kFailed) == 0);
  CHECK(before_destroy == expected);
  CHECK(GroupPackets(collector.packets) == before_destroy);
  CHECK(collector.generations.size() == collector.packets.size());
  CHECK(std::all_of(collector.generations.begin(), collector.generations.end(),
                    [](auto generation) { return generation == 0; }));
  size_t expected_frames = 0;
  if (decode) {
    for (const auto& stream : DecodeRawMedia(reference)) {
      expected_frames += stream.second.size();
    }
  }
  CHECK(collector.frames == expected_frames);
  if (!decode) {
    CHECK(std::chrono::steady_clock::now() - started < 1s);
  }
}

TEST_CASE("Input packet callbacks can record a playable MP4 without decoding",
          "[input][packet]") {
  const auto source = ReadRawMedia(PacketSamplePath());
  PacketCollector collector;
  PacketRecording recording;
  FfmpegInputConfig config;
  config.decode = false;
  FfmpegInput input(config);
  collector.Attach(
      input, [&](const auto& streams) { recording.Open(streams); },
      [&](const auto& packet) { recording.Write(packet); });
  input.Start(PacketSamplePath());
  REQUIRE(collector.WaitForEnd());
  input.Stop();
  if (collector.error) std::rethrow_exception(collector.error);
  CHECK(collector.valid);
  CHECK(collector.ready == 1);
  CHECK(collector.frames == 0);
  CHECK(std::all_of(collector.generations.begin(), collector.generations.end(),
                    [](auto generation) { return generation == 0; }));
  CHECK(GroupPackets(collector.packets) == GroupPackets(source.packets));
  recording.Finish();
  const auto recorded = ReadRawMedia(recording.path());
  REQUIRE(recorded.streams.size() == source.streams.size());
  CHECK(recorded.packets.size() == source.packets.size());
  CHECK(DecodeRawMedia(recorded) == DecodeRawMedia(source));
}

TEST_CASE("Packet-only input executes Seek before reading subsequent packets",
          "[input][packet]") {
  const auto path =
      std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/seek_h264_aac.mp4";
  const auto original = ReadRawMedia(path);
  const auto sought = ReadRawMedia(path, 6s);
  REQUIRE_FALSE(original.packets.empty());
  REQUIRE_FALSE(sought.packets.empty());
  PacketCollector collector;
  PacketCollector restarted;
  FfmpegInputConfig config;
  config.decode = false;
  FfmpegInput input(config);
  PacketPause pause;
  collector.Attach(input, {}, pause.hook());
  input.Start(path);
  REQUIRE(pause.Wait());
  input.Seek(6s);
  pause.Release();
  REQUIRE(collector.WaitForEnd());
  input.Stop();
  if (collector.error) std::rethrow_exception(collector.error);
  REQUIRE_FALSE(collector.packets.empty());
  REQUIRE(collector.generations.size() == collector.packets.size());
  CHECK(collector.generations.front() == 0);
  CHECK(std::all_of(collector.generations.begin() + 1,
                    collector.generations.end(),
                    [](auto generation) { return generation == 1; }));
  CHECK(RecordPacket(*collector.packets.front().get()) ==
        RecordPacket(*original.packets.front().get()));
  std::vector<ffmpeg::Packet> after_seek;
  for (size_t i = 1; i < collector.packets.size(); ++i) {
    after_seek.push_back(collector.packets[i].Ref());
  }
  CHECK(GroupPackets(after_seek) == GroupPackets(sought.packets));
  CHECK(collector.valid);
  CHECK(collector.ready == 1);
  CHECK(collector.frames == 0);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kEnded) == 1);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kFailed) == 0);
  // Reset must be observable after an earlier session actually reached 1.
  restarted.Attach(input);
  input.Start(path);
  REQUIRE(restarted.WaitForEnd());
  input.Stop();
  CHECK(restarted.valid);
  CHECK(restarted.ready == 1);
  CHECK(restarted.frames == 0);
  CHECK(GroupPackets(restarted.packets) == GroupPackets(original.packets));
  CHECK(std::all_of(restarted.generations.begin(), restarted.generations.end(),
                    [](auto generation) { return generation == 0; }));
}

TEST_CASE("Packet-only loops keep original packets and advance generation",
          "[input][packet]") {
  const auto source = ReadRawMedia(PacketSamplePath());
  REQUIRE_FALSE(source.packets.empty());
  PacketCollector collector;
  FfmpegInputConfig config;
  config.decode = false;
  config.loop = true;
  FfmpegInput input(config);
  PacketPause pause(source.packets.size() * 2);
  collector.Attach(input, {}, pause.hook());
  input.Start(PacketSamplePath());
  REQUIRE(pause.Wait());
  pause.Release();
  input.Stop();
  if (collector.error) std::rethrow_exception(collector.error);
  REQUIRE(collector.generations.size() == collector.packets.size());
  for (std::uint64_t generation : {0, 1}) {
    std::vector<ffmpeg::Packet> phase;
    for (size_t i = 0; i < collector.packets.size(); ++i) {
      if (collector.generations[i] == generation) {
        phase.push_back(collector.packets[i].Ref());
      }
    }
    CHECK(GroupPackets(phase) == GroupPackets(source.packets));
  }
  CHECK(collector.valid);
  CHECK(collector.ready == 1);
  CHECK(collector.frames == 0);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kConnecting) == 1);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kConnected) == 1);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kStopped) == 1);
  CHECK(std::count(collector.states.begin(), collector.states.end(),
                   InputState::kFailed) == 0);
}

}  // namespace
