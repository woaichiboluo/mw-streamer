#include "mw/output/remux_sink.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Record/MP4.h"
#include "Record/MP4Demuxer.h"
#include "ext-codec/H264.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/input/zlm_input.h"
#include "mw/pipeline/pipeline.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::input::ZlmInput;
using mw::streamer::input::ZlmInputConfig;
using mw::streamer::media::StreamEndReason;
using mw::streamer::media::TimelineResetReason;
using mw::streamer::output::RemuxSink;
using mw::streamer::output::RemuxSinkConfig;
using mw::streamer::sink::PacketSinkState;
using namespace mw::streamer::pipeline;
using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::ffmpeg::InputFormatContext;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::performance::PerformanceType;
using mw::streamer::performance::PerformanceUnit;

class TestDirectory final {
 public:
  TestDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("mw-remux-sink-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string SamplePath() {
  return std::string(MW_REMUX_SINK_TEST_DATA_DIR) + "/h264_aac.mp4";
}

RemuxSinkConfig Config(const std::filesystem::path& target) {
  RemuxSinkConfig config;
  config.target = target.string();
  return config;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

bool WaitForEnd(const RemuxSink& sink) {
  return WaitUntil([&] {
    return sink.state() == PacketSinkState::kEnded ||
           sink.state() == PacketSinkState::kFailed;
  });
}

std::vector<std::filesystem::path> FindFiles(
    const std::filesystem::path& directory, const std::string& extension) {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == extension) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

// Use the same Annex B packet format as ZlmInput. FFmpeg's MP4 demuxer would
// instead provide length-prefixed AVC packets, outside this input contract.
struct Sample {
  std::vector<StreamInfo> streams;
  std::vector<Packet> packets;
};

Sample ReadSample() {
  mediakit::MP4Demuxer input;
  input.openMP4(SamplePath());
  Sample sample;
  std::unordered_map<int, std::unique_ptr<ZlmPacketConverter>> converters;
  for (const auto& track : input.getTracks(true)) {
    const auto index = static_cast<int>(sample.streams.size());
    ZlmCodecParametersConverter parameters(track);
    sample.streams.push_back(
        {index, parameters.codec_parameters(), parameters.time_base()});
    auto converter = std::make_unique<ZlmPacketConverter>(track, index);
    converter->SetOnPacket([&sample](const Packet& packet) {
      sample.packets.push_back(packet);
      return true;
    });
    converters.emplace(track->getIndex(), std::move(converter));
  }
  bool eof = false;
  while (!eof) {
    bool key = false;
    int error = 0;
    auto frame = input.readFrame(key, eof, &error);
    REQUIRE(error == 0);
    if (!frame) {
      continue;
    }
    if (key && !frame->keyFrame()) {
      frame = std::make_shared<mediakit::FrameCacheAble>(frame, true);
    }
    REQUIRE(converters.at(frame->getIndex())->InputFrame(frame));
  }
  for (const auto& [index, converter] : converters) {
    REQUIRE(converter->Flush());
  }
  REQUIRE_FALSE(sample.packets.empty());
  return sample;
}

void FeedPackets(RemuxSink& sink, const Sample& sample,
                 std::uint64_t generation, std::int64_t offset_ms = 0) {
  for (const auto& packet : sample.packets) {
    auto adjusted = packet.Clone();
    const auto& stream = sample.streams.at(packet->stream_index);
    const auto offset =
        av_rescale_q(offset_ms, AVRational{1, 1000}, stream.time_base);
    adjusted->dts += offset;
    adjusted->pts += offset;
    sink.OnPacket({generation, std::move(adjusted)});
  }
}

void CheckRecordedMedia(const std::filesystem::path& path,
                        std::int64_t minimum_duration_ms = 1800) {
  InputFormatContext input(path.string());
  input.FindStreamInfo();
  REQUIRE(input->nb_streams == 2);
  bool video = false;
  bool audio = false;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    const auto& parameters = *input->streams[i]->codecpar;
    if (parameters.codec_type == AVMEDIA_TYPE_VIDEO) {
      CHECK(parameters.codec_id == AV_CODEC_ID_H264);
      CHECK(parameters.width == 64);
      CHECK(parameters.height == 64);
      video = true;
    } else if (parameters.codec_type == AVMEDIA_TYPE_AUDIO) {
      CHECK(parameters.codec_id == AV_CODEC_ID_AAC);
      audio = true;
    }
  }
  CHECK(video);
  CHECK(audio);
  CHECK(input->duration >= minimum_duration_ms * 1000);
  std::vector<std::int64_t> last_dts(input->nb_streams, AV_NOPTS_VALUE);
  std::vector<int> counts(input->nb_streams, 0);
  Packet packet;
  while (input.ReadPacket(packet)) {
    const auto index = packet->stream_index;
    if (last_dts.at(index) != AV_NOPTS_VALUE) {
      CHECK(packet->dts >= last_dts[index]);
    }
    last_dts[index] = packet->dts;
    ++counts[index];
    packet.Unref();
  }
  for (const auto count : counts) {
    CHECK(count > 0);
  }
}

// Comparing encoded slice payloads proves stream copy, beyond codec/dimension
// checks that a decode/re-encode implementation could also satisfy.
std::vector<std::string> VideoSlices(const std::filesystem::path& path) {
  mediakit::MP4Demuxer input;
  input.openMP4(path.string());
  std::vector<std::string> slices;
  bool eof = false;
  while (!eof) {
    bool key = false;
    int error = 0;
    const auto frame = input.readFrame(key, eof, &error);
    REQUIRE(error == 0);
    if (!frame || frame->getCodecId() != mediakit::CodecH264 ||
        frame->size() <= frame->prefixSize()) {
      continue;
    }
    // A demuxed access unit can start with SEI and contain an IDR after it.
    // Inspect every NAL so that the first source frame is not skipped.
    mediakit::splitH264(
        frame->data(), frame->size(), frame->prefixSize(),
        [&](const char* data, std::size_t size, std::size_t prefix) {
          REQUIRE(size > prefix);
          const auto nal_type = static_cast<unsigned char>(data[prefix]) & 0x1f;
          if (nal_type == 1 || nal_type == 5) {
            slices.emplace_back(data + prefix, size - prefix);
          }
        });
  }
  return slices;
}

std::vector<std::string> VideoSlices(const Sample& sample) {
  std::vector<std::string> slices;
  for (const auto& packet : sample.packets) {
    if (sample.streams.at(packet->stream_index)
            .codec_parameters.get()
            ->codec_type != AVMEDIA_TYPE_VIDEO) {
      continue;
    }
    const auto* data = reinterpret_cast<const char*>(packet->data);
    REQUIRE(packet->size >= 4);
    const std::size_t prefix = packet->data[2] == 1 ? 3 : 4;
    mediakit::splitH264(
        data, packet->size, prefix,
        [&](const char* nal, std::size_t size, std::size_t nal_prefix) {
          REQUIRE(size > nal_prefix);
          const auto type = static_cast<unsigned char>(nal[nal_prefix]) & 0x1f;
          if (type == 1 || type == 5) {
            slices.emplace_back(nal + nal_prefix, size - nal_prefix);
          }
        });
  }
  return slices;
}

std::string AudioPayload(const Packet& packet) {
  const auto* data = packet->data;
  auto size = static_cast<std::size_t>(packet->size);
  // ZlmInput supplies ADTS while the fMP4 demuxer returns raw AAC.
  if (size >= 7 && data[0] == 0xff && (data[1] & 0xf6) == 0xf0) {
    const std::size_t prefix = (data[1] & 1) ? 7 : 9;
    REQUIRE(size >= prefix);
    data += prefix;
    size -= prefix;
  }
  return {reinterpret_cast<const char*>(data), size};
}

std::vector<std::string> AudioPayloads(const std::filesystem::path& path) {
  InputFormatContext input(path.string());
  input.FindStreamInfo();
  std::vector<std::string> payloads;
  Packet packet;
  while (input.ReadPacket(packet)) {
    if (input->streams[packet->stream_index]->codecpar->codec_type ==
        AVMEDIA_TYPE_AUDIO) {
      payloads.push_back(AudioPayload(packet));
    }
    packet.Unref();
  }
  return payloads;
}

using Timestamps = std::vector<std::pair<std::int64_t, std::int64_t>>;

void CheckRecordedTimestamps(const std::filesystem::path& path,
                             const Sample& sample, std::int64_t offset_ms = 0,
                             bool exact_timestamps = false) {
  std::unordered_map<int, Timestamps> expected;
  for (const auto& packet : sample.packets) {
    const auto& stream = sample.streams.at(packet->stream_index);
    expected[stream.codec_parameters.get()->codec_type].emplace_back(
        av_rescale_q(packet->dts, stream.time_base, AVRational{1, 1000}),
        av_rescale_q(packet->pts, stream.time_base, AVRational{1, 1000}));
  }
  for (auto& [type, timestamps] : expected) {
    if (offset_ms == 0) continue;
    const auto second_pass = timestamps;
    for (const auto& [dts, pts] : second_pass) {
      timestamps.emplace_back(dts + offset_ms, pts + offset_ms);
    }
  }
  InputFormatContext input(path.string());
  input.FindStreamInfo();
  std::unordered_map<int, Timestamps> actual;
  Packet packet;
  while (input.ReadPacket(packet)) {
    const auto& stream = *input->streams[packet->stream_index];
    actual[stream.codecpar->codec_type].emplace_back(
        av_rescale_q(packet->dts, stream.time_base, AVRational{1, 1000}),
        av_rescale_q(packet->pts, stream.time_base, AVRational{1, 1000}));
    packet.Unref();
  }
  REQUIRE(actual.size() == expected.size());
  const auto reference_type = expected.begin()->first;
  const auto common_offset = actual.at(reference_type).front().first -
                             expected.at(reference_type).front().first;
  if (exact_timestamps) CHECK(common_offset == 0);
  for (const auto& [type, timestamps] : expected) {
    INFO(type);
    const auto& recorded = actual.at(type);
    REQUIRE(recorded.size() == timestamps.size());
    for (std::size_t i = 0; i < timestamps.size(); ++i) {
      INFO(i);
      CHECK(recorded[i].first - timestamps[i].first == common_offset);
      CHECK(recorded[i].second - timestamps[i].second == common_offset);
    }
  }
}

}  // namespace

TEST_CASE("RemuxSink由Pipeline分发原始Packet并独立生成fMP4和HLS") {
  TestDirectory directory;
  auto mp4 = std::make_unique<RemuxSink>(
      "mp4", Config(directory.path() / "source.mp4"));
  auto hls = std::make_unique<RemuxSink>(
      "hls", Config(directory.path() / "source.m3u8"));
  const auto* mp4_sink = mp4.get();
  const auto* hls_sink = hls.get();
  auto input = std::make_unique<ZlmInput>(ZlmInputConfig{SamplePath()});
  const auto* input_node = input.get();
  Pipeline pipeline(std::move(input));
  pipeline.AddSink(std::move(mp4));
  pipeline.AddSink(std::move(hls));
  pipeline.Start();
  REQUIRE(WaitForEnd(*mp4_sink));
  REQUIRE(WaitForEnd(*hls_sink));
  INFO(mp4_sink->error());
  INFO(hls_sink->error());
  CHECK(mp4_sink->state() == PacketSinkState::kEnded);
  CHECK(hls_sink->state() == PacketSinkState::kEnded);
  CHECK(mp4_sink->queue_depth() == 0);
  CHECK(hls_sink->queue_depth() == 0);
  const auto input_snapshot = input_node->GetPerformance();
  REQUIRE(input_snapshot.operations.size() == 1);
  const auto& input_operation = input_snapshot.operations.front();
  CHECK(input_operation.type == PerformanceType::kInput);
  CHECK(input_operation.output_unit == PerformanceUnit::kPacket);
  REQUIRE(input_operation.output_count > 0);
  REQUIRE(input_operation.output_bytes > 0);
  CHECK(input_operation.started_calls == 0);
  for (const auto* sink : {mp4_sink, hls_sink}) {
    const auto snapshot = sink->GetPerformance();
    REQUIRE(snapshot.operations.size() == 1);
    const auto& operation = snapshot.operations.front();
    CHECK(operation.type == PerformanceType::kRemux);
    CHECK(operation.input_unit == PerformanceUnit::kPacket);
    CHECK(operation.output_unit == PerformanceUnit::kPacket);
    CHECK(operation.input_count == input_operation.output_count);
    CHECK(operation.output_count == input_operation.output_count);
    CHECK(operation.input_bytes == input_operation.output_bytes);
    CHECK(operation.output_bytes == input_operation.output_bytes);
    CHECK(operation.failed_calls == 0);
    CHECK(operation.in_flight == 0);
    CHECK(sink->GetPerformance().operations.front().output_count ==
          operation.output_count);
  }
  CHECK(input_node->GetPerformance().operations.front().output_count ==
        input_operation.output_count);
  const auto mp4_files = FindFiles(directory.path(), ".mp4");
  auto playlists = FindFiles(directory.path(), ".m3u8");
  // ZLM also writes index_delay.m3u8 for delayed playback of the same target.
  playlists.erase(std::remove_if(playlists.begin(), playlists.end(),
                                 [](const auto& path) {
                                   return path.filename() != "index.m3u8";
                                 }),
                  playlists.end());
  // HLS initialization is named init.mp4; isolate the standalone recording.
  std::vector<std::filesystem::path> recordings;
  for (const auto& path : mp4_files) {
    if (path.parent_path() == directory.path()) {
      recordings.push_back(path);
    }
  }
  REQUIRE(recordings.size() == 1);
  REQUIRE(playlists.size() == 1);
  CheckRecordedMedia(recordings.front());
  CheckRecordedMedia(playlists.front());
  const auto source_slices = VideoSlices(SamplePath());
  REQUIRE(source_slices.size() == 20);
  CHECK(VideoSlices(recordings.front()) == source_slices);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("RemuxSink保留关键帧之前的音频和非关键视频直到EOF") {
  TestDirectory directory;
  auto sample = ReadSample();
  const auto is_audio = [&](const Packet& packet) {
    return sample.streams.at(packet->stream_index)
               .codec_parameters.get()
               ->codec_type == AVMEDIA_TYPE_AUDIO;
  };
  SECTION("全部音频先于首个视频关键帧") {
    std::stable_partition(sample.packets.begin(), sample.packets.end(),
                          is_audio);
  }
  SECTION("声明音视频但直到EOF只有音频") {
    sample.packets.erase(
        std::remove_if(sample.packets.begin(), sample.packets.end(),
                       [&](const Packet& packet) { return !is_audio(packet); }),
        sample.packets.end());
  }
  SECTION("视频从非关键帧开始且直到EOF没有关键帧") {
    sample.packets.erase(
        std::remove_if(sample.packets.begin(), sample.packets.end(),
                       [&](const Packet& packet) {
                         return !is_audio(packet) &&
                                (packet->flags & AV_PKT_FLAG_KEY);
                       }),
        sample.packets.end());
  }
  const auto expected_video = VideoSlices(sample);
  std::vector<std::string> expected_audio;
  for (const auto& packet : sample.packets) {
    if (is_audio(packet)) expected_audio.push_back(AudioPayload(packet));
  }
  REQUIRE_FALSE(expected_audio.empty());
  RemuxSink sink("sink", Config(directory.path() / "complete.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample, 1);
  if (std::all_of(sample.packets.begin(), sample.packets.end(), is_audio)) {
    REQUIRE(WaitUntil([&] {
      return sink.GetPerformance().operations.front().input_count ==
             sample.packets.size();
    }));
    // Startup accepts audio while waiting for video. These packets have not
    // reached the local muxer yet; EOF flushes them exactly once.
    CHECK(sink.GetPerformance().operations.front().output_count == 0);
  }
  sink.OnInputEnded({1, StreamEndReason::kEof});
  REQUIRE(WaitForEnd(sink));
  INFO(sink.error());
  REQUIRE(sink.state() == PacketSinkState::kEnded);
  const auto snapshot = sink.GetPerformance();
  REQUIRE(snapshot.operations.size() == 1);
  const auto& operation = snapshot.operations.front();
  CHECK(operation.input_count == sample.packets.size());
  CHECK(operation.output_count == sample.packets.size());
  CHECK(operation.completed_calls == sample.packets.size());
  std::uint64_t bytes = 0;
  for (const auto& packet : sample.packets) bytes += packet->size;
  CHECK(operation.input_bytes == bytes);
  CHECK(operation.output_bytes == bytes);
  CHECK(sink.GetPerformance().operations.front().input_count ==
        operation.input_count);
  const auto files = FindFiles(directory.path(), ".mp4");
  REQUIRE(files.size() == 1);
  CHECK((AudioPayloads(files.front()) == expected_audio));
  CHECK((VideoSlices(files.front()) == expected_video));
  sink.Stop();
}

TEST_CASE("RemuxSink跨代次时间戳递增时继续同一录像") {
  TestDirectory directory;
  const auto sample = ReadSample();
  RemuxSink sink("sink", Config(directory.path() / "continuous.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample, 1);
  sink.OnInputEnded({1, StreamEndReason::kInterrupted});
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  sink.OnStreamsReady({2, sample.streams});
  // Leave a real reconnect gap, including any AAC priming duration exposed
  // by ZLM's MP4 demuxer, so every track is strictly beyond its old DTS.
  FeedPackets(sink, sample, 2, 3000);
  sink.OnInputEnded({2, StreamEndReason::kEof});
  REQUIRE(WaitForEnd(sink));
  INFO(sink.error());
  REQUIRE(sink.state() == PacketSinkState::kEnded);
  const auto files = FindFiles(directory.path(), ".mp4");
  REQUIRE(files.size() == 1);
  CheckRecordedMedia(files.front(), 4900);
  // A common start offset may change absolute timestamps; every per-track
  // DTS/PTS interval, including the reconnect gap, must remain unchanged.
  CheckRecordedTimestamps(files.front(), sample, 3000);
  auto expected_slices = VideoSlices(SamplePath());
  const auto second_pass = expected_slices;
  expected_slices.insert(expected_slices.end(), second_pass.begin(),
                         second_pass.end());
  CHECK(VideoSlices(files.front()) == expected_slices);
  sink.Stop();
}

TEST_CASE("fMP4时间戳保留模式支持提前初始化和单轨且默认行为不变") {
  TestDirectory directory;
  AVMediaType selected_type = AVMEDIA_TYPE_UNKNOWN;
  SECTION("音视频") {}
  SECTION("只有音频") { selected_type = AVMEDIA_TYPE_AUDIO; }
  SECTION("只有视频") { selected_type = AVMEDIA_TYPE_VIDEO; }
  for (const bool preserve : {false, true}) {
    INFO(preserve);
    InputFormatContext input(SamplePath());
    input.FindStreamInfo();
    const auto path = directory.path() / "writer.mp4";
    auto file = std::make_shared<mediakit::MP4FileDisk>();
    file->openFile(path.c_str(), "wb+");
    auto writer = file->createWriter(
        MOV_FLAG_SEGMENT | (preserve ? MOV_FLAG_PRESERVE_TIMESTAMPS : 0), true);
    Sample expected;
    std::unordered_map<int, int> tracks;
    std::unordered_map<int, std::int64_t> first_dts;
    for (unsigned int i = 0; i < input->nb_streams; ++i) {
      const auto& stream = *input->streams[i];
      const auto& parameters = *stream.codecpar;
      expected.streams.push_back(
          {static_cast<int>(i),
           mw::streamer::ffmpeg::CodecParameters(parameters),
           {1, 1000}});
      if (selected_type != AVMEDIA_TYPE_UNKNOWN &&
          parameters.codec_type != selected_type) {
        continue;
      }
      const auto track =
          parameters.codec_type == AVMEDIA_TYPE_VIDEO
              ? mp4_writer_add_video(writer.get(), MOV_OBJECT_H264,
                                     parameters.width, parameters.height,
                                     parameters.extradata,
                                     parameters.extradata_size)
              : mp4_writer_add_audio(writer.get(), MOV_OBJECT_AAC,
                                     parameters.ch_layout.nb_channels, 16,
                                     parameters.sample_rate,
                                     parameters.extradata,
                                     parameters.extradata_size);
      REQUIRE(track >= 0);
      tracks.emplace(i, track);
    }
    REQUIRE(mp4_writer_init_segment(writer.get()) == 0);
    Packet packet;
    while (input.ReadPacket(packet)) {
      const auto index = packet->stream_index;
      if (tracks.count(index) == 0) {
        packet.Unref();
        continue;
      }
      const auto& stream = *input->streams[index];
      const auto original_dts =
          av_rescale_q(packet->dts, stream.time_base, AVRational{1, 1000});
      const auto original_pts =
          av_rescale_q(packet->pts, stream.time_base, AVRational{1, 1000});
      const auto origin = first_dts.emplace(index, original_dts).first->second;
      const auto start =
          stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? 837 : 700;
      const auto dts = original_dts - origin + start;
      const auto pts = original_pts - origin + start;
      REQUIRE(mp4_writer_write(writer.get(), tracks.at(index), packet->data,
                               packet->size, pts, dts,
                               packet->flags & AV_PKT_FLAG_KEY
                                   ? MOV_AV_FLAG_KEYFREAME
                                   : 0) == 0);
      auto recorded = packet.Clone();
      recorded->dts = preserve ? dts : dts - start;
      recorded->pts = preserve ? pts : pts - start;
      expected.packets.push_back(std::move(recorded));
      if (expected.packets.size() % 7 == 0) {
        REQUIRE(mp4_writer_save_segment(writer.get()) == 0);
      }
      packet.Unref();
    }
    writer.reset();
    file.reset();
    CheckRecordedTimestamps(path, expected, 0, true);
  }
}

TEST_CASE("RemuxSink录像和HLS跨分片保留音视频共同时间原点") {
  TestDirectory directory;
  auto sample = ReadSample();
  AVMediaType delayed_type = AVMEDIA_TYPE_VIDEO;
  SECTION("音频先开始") {}
  SECTION("视频先开始") { delayed_type = AVMEDIA_TYPE_AUDIO; }
  for (auto& packet : sample.packets) {
    const auto& stream = sample.streams.at(packet->stream_index);
    if (stream.codec_parameters.get()->codec_type == delayed_type) {
      const auto offset =
          av_rescale_q(137, AVRational{1, 1000}, stream.time_base);
      packet->dts += offset;
      packet->pts += offset;
    }
  }
  for (const auto* extension : {".mp4", ".m3u8"}) {
    INFO(extension);
    RemuxSink sink("sink", Config(directory.path() /
                                  (std::string("timeline") + extension)));
    sink.OnStreamsReady({1, sample.streams});
    FeedPackets(sink, sample, 1);
    FeedPackets(sink, sample, 1, 3000);
    sink.OnInputEnded({1, StreamEndReason::kEof});
    REQUIRE(WaitForEnd(sink));
    INFO(sink.error());
    REQUIRE(sink.state() == PacketSinkState::kEnded);
    auto files = FindFiles(directory.path(), extension);
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const auto& path) {
                                 return path.extension() == ".m3u8" &&
                                        path.filename() != "index.m3u8";
                               }),
                files.end());
    REQUIRE(files.size() == 1);
    // Every packet in every fragment must retain the same cross-track shift.
    CheckRecordedTimestamps(files.front(), sample, 3000);
    if (std::string(extension) == ".mp4") {
      auto expected_video = VideoSlices(sample);
      const auto second_video = expected_video;
      expected_video.insert(expected_video.end(), second_video.begin(),
                            second_video.end());
      CHECK(VideoSlices(files.front()) == expected_video);
      std::vector<std::string> expected_audio;
      for (const auto& packet : sample.packets) {
        if (sample.streams.at(packet->stream_index)
                .codec_parameters.get()
                ->codec_type == AVMEDIA_TYPE_AUDIO) {
          expected_audio.push_back(AudioPayload(packet));
        }
      }
      const auto second_audio = expected_audio;
      expected_audio.insert(expected_audio.end(), second_audio.begin(),
                            second_audio.end());
      CHECK(AudioPayloads(files.front()) == expected_audio);
    }
    sink.Stop();
  }
}

TEST_CASE("RemuxSink任意轨道DTS回退重建输出并保留两份有效录像") {
  TestDirectory directory;
  auto sample = ReadSample();
  bool reset = false;
  AVMediaType first_track = AVMEDIA_TYPE_VIDEO;
  SECTION("同代次视频DTS先回退") {}
  SECTION("同代次音频DTS先回退") { first_track = AVMEDIA_TYPE_AUDIO; }
  SECTION("跨代次DTS回退") { reset = true; }
  RemuxSink sink("sink", Config(directory.path() / "restart.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample, 1);
  const auto first = std::find_if(
      sample.packets.begin(), sample.packets.end(), [&](const auto& packet) {
        return sample.streams.at(packet->stream_index)
                   .codec_parameters.get()
                   ->codec_type == first_track;
      });
  REQUIRE(first != sample.packets.end());
  std::rotate(sample.packets.begin(), first, first + 1);
  const std::uint64_t generation = reset ? 2 : 1;
  if (reset) {
    sink.OnInputEnded({1, StreamEndReason::kInterrupted});
    sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
    sink.OnStreamsReady({2, sample.streams});
  }
  FeedPackets(sink, sample, generation);
  sink.OnInputEnded({generation, StreamEndReason::kEof});
  REQUIRE(WaitForEnd(sink));
  INFO(sink.error());
  REQUIRE(sink.state() == PacketSinkState::kEnded);
  const auto files = FindFiles(directory.path(), ".mp4");
  REQUIRE(files.size() == 2);
  const auto source_slices = VideoSlices(SamplePath());
  for (const auto& file : files) {
    CheckRecordedMedia(file);
    CHECK(VideoSlices(file) == source_slices);
  }
  sink.Stop();
}

TEST_CASE("RemuxSink仅PTS回退不重建输出") {
  TestDirectory directory;
  auto sample = ReadSample();
  bool changed = false;
  for (auto& packet : sample.packets) {
    const auto& stream = sample.streams.at(packet->stream_index);
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO &&
        packet->pts >= 500) {
      packet->pts = 0;
      changed = true;
      break;
    }
  }
  REQUIRE(changed);
  RemuxSink sink("sink", Config(directory.path() / "pts.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample, 1);
  sink.OnInputEnded({1, StreamEndReason::kEof});
  REQUIRE(WaitForEnd(sink));
  INFO(sink.error());
  CHECK(sink.state() == PacketSinkState::kEnded);
  const auto files = FindFiles(directory.path(), ".mp4");
  REQUIRE(files.size() == 1);
  CHECK(VideoSlices(files.front()) == VideoSlices(SamplePath()));
  sink.Stop();
}

TEST_CASE("RemuxSink永久录像失败只影响自身") {
  TestDirectory directory;
  const auto blocked = directory.path() / "blocked";
  std::ofstream(blocked).put('x');
  auto failed =
      std::make_unique<RemuxSink>("failed", Config(blocked / "failed.mp4"));
  auto healthy = std::make_unique<RemuxSink>(
      "healthy", Config(directory.path() / "healthy.mp4"));
  const auto* failed_sink = failed.get();
  const auto* healthy_sink = healthy.get();
  Pipeline pipeline(std::make_unique<ZlmInput>(ZlmInputConfig{SamplePath()}));
  pipeline.AddSink(std::move(failed));
  pipeline.AddSink(std::move(healthy));
  pipeline.Start();
  REQUIRE(WaitForEnd(*failed_sink));
  REQUIRE(WaitForEnd(*healthy_sink));
  CHECK(failed_sink->state() == PacketSinkState::kFailed);
  CHECK_FALSE(failed_sink->error().empty());
  CHECK(healthy_sink->state() == PacketSinkState::kEnded);
  CHECK(pipeline.state() != PipelineState::kFailed);
  pipeline.Stop();
  CHECK(failed_sink->state() == PacketSinkState::kFailed);
  CHECK(pipeline.state() == PipelineState::kStopped);
  const auto files = FindFiles(directory.path(), ".mp4");
  REQUIRE(files.size() == 1);
  CheckRecordedMedia(files.front());
}

TEST_CASE("RemuxSink停止排空已接收Packet且停止后拒绝继续写入") {
  TestDirectory directory;
  const auto sample = ReadSample();
  RemuxSink sink("sink", Config(directory.path() / "stop.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample, 1);
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kStopped);
  CHECK(sink.queue_depth() == 0);
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  sink.OnStreamsReady({2, sample.streams});
  FeedPackets(sink, sample, 2);
  sink.OnInputEnded({2, StreamEndReason::kEof});
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kStopped);
  CHECK(sink.queue_depth() == 0);
  const auto files = FindFiles(directory.path(), ".mp4");
  REQUIRE(files.size() == 1);
  CheckRecordedMedia(files.front());
  CHECK(VideoSlices(files.front()) == VideoSlices(SamplePath()));
}

TEST_CASE("RemuxSink拒绝轨道元数据变化") {
  TestDirectory directory;
  const auto sample = ReadSample();
  RemuxSink sink("sink", Config(directory.path() / "metadata.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  FeedPackets(sink, sample, 1);
  sink.OnInputEnded({1, StreamEndReason::kInterrupted});
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  auto changed = sample.streams;
  for (auto& stream : changed) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      stream.codec_parameters.get()->width *= 2;
    }
  }
  sink.OnStreamsReady({2, std::move(changed)});
  REQUIRE(WaitUntil([&] { return sink.state() == PacketSinkState::kFailed; }));
  CHECK_FALSE(sink.error().empty());
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kFailed);
}

TEST_CASE("RemuxSink同步拒绝无效单目标配置") {
  RemuxSinkConfig config;
  CHECK_THROWS_AS(RemuxSink("remux-5", config), std::invalid_argument);
  config.target = "udp://127.0.0.1:9000/live";
  CHECK_THROWS_AS(RemuxSink("remux-6", config), std::invalid_argument);
  config.target = "camera.mp4";
  config.packet_queue_capacity = 0;
  CHECK_THROWS_AS(RemuxSink("remux-7", config), std::invalid_argument);
  config.packet_queue_capacity = 1;
  RemuxSink sink("sink", config);
  CHECK(sink.state() == PacketSinkState::kIdle);
  sink.Stop();
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kStopped);
}

TEST_CASE("RemuxSink启动缓存满明确失败而不静默丢包") {
  TestDirectory directory;
  const auto sample = ReadSample();
  auto config = Config(directory.path() / "startup.mp4");
  config.packet_queue_capacity = 2;
  RemuxSink sink("sink", config);
  sink.OnStreamsReady({1, sample.streams});
  REQUIRE(WaitUntil([&] { return sink.state() == PacketSinkState::kRunning; }));
  std::size_t submitted = 0;
  for (const auto& packet : sample.packets) {
    if (sample.streams.at(packet->stream_index)
            .codec_parameters.get()
            ->codec_type != AVMEDIA_TYPE_AUDIO) {
      continue;
    }
    sink.OnPacket({1, packet});
    // Drain the dispatch queue between submissions so this exercises only
    // the startup cache waiting for the declared video track.
    REQUIRE(WaitUntil([&] { return sink.queue_depth() == 0; }));
    if (++submitted == 3) break;
  }
  REQUIRE(submitted == 3);
  REQUIRE(WaitUntil([&] { return sink.state() == PacketSinkState::kFailed; }));
  CHECK(sink.error().find("启动包缓存已满") != std::string::npos);
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kFailed);
}

TEST_CASE("RemuxSink无法转换编码Packet时明确失败") {
  TestDirectory directory;
  const auto sample = ReadSample();
  RemuxSink sink("sink", Config(directory.path() / "invalid.mp4"));
  sink.OnStreamsReady({1, sample.streams});
  const auto video = std::find_if(
      sample.packets.begin(), sample.packets.end(), [&](const Packet& packet) {
        return sample.streams.at(packet->stream_index)
                   .codec_parameters.get()
                   ->codec_type == AVMEDIA_TYPE_VIDEO;
      });
  REQUIRE(video != sample.packets.end());
  auto invalid = video->Clone();
  REQUIRE(av_packet_make_writable(invalid.get()) == 0);
  std::fill(invalid->data, invalid->data + invalid->size, 0xff);
  std::uint64_t successful_packets = 0;
  SECTION("启动元数据发现失败") {}
  SECTION("启动完成后的封装调用失败") {
    FeedPackets(sink, sample, 1);
    successful_packets = sample.packets.size();
    const auto& stream = sample.streams.at(invalid->stream_index);
    // Keep this packet in the existing output generation so the invalid
    // payload fails actual muxing, rather than another startup discovery.
    const auto offset =
        av_rescale_q(10000, AVRational{1, 1000}, stream.time_base);
    invalid->pts += offset;
    invalid->dts += offset;
  }
  sink.OnPacket({1, std::move(invalid)});
  REQUIRE(WaitUntil([&] { return sink.state() == PacketSinkState::kFailed; }));
  CHECK(sink.error().find("无法转换编码包") != std::string::npos);
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kFailed);
  const auto snapshot = sink.GetPerformance();
  REQUIRE(snapshot.operations.size() == 1);
  const auto& operation = snapshot.operations.front();
  CHECK(operation.input_count == successful_packets + 1);
  CHECK(operation.output_count == successful_packets);
  // Invalid startup metadata fails before an actual muxing call begins.
  const std::uint64_t failed_calls = successful_packets == 0 ? 0 : 1;
  CHECK(operation.completed_calls == successful_packets + failed_calls);
  CHECK(operation.failed_calls == failed_calls);
  CHECK(operation.in_flight == 0);
}
