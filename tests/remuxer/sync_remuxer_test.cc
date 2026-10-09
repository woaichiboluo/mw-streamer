#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include "../encoder/encoder_test_support.h"
#include "Poller/EventPoller.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/remuxer/sync_remuxer.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kMicroseconds{1, 1000000};

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_ = nullptr;
};

class OutputDirectory final {
 public:
  OutputDirectory()
      : path_(std::filesystem::current_path() /
              ("sync-remuxer-" + std::to_string(std::chrono::steady_clock::now()
                                                    .time_since_epoch()
                                                    .count()))) {
    std::filesystem::create_directories(path_);
  }
  ~OutputDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::string File() const { return (path_ / "record.mp4").string(); }

 private:
  std::filesystem::path path_;
};

std::string Fixture(const std::string& name) {
  return std::string(MW_STREAMER_SYNC_REMUXER_TEST_DATA_DIR) + "/" + name;
}

struct FormatCloser final {
  void operator()(AVFormatContext* format) const {
    avformat_close_input(&format);
  }
};

struct Media final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
};

Media ReadMedia(const std::string& path,
                AVMediaType selected = AVMEDIA_TYPE_UNKNOWN) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开测试媒体");
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "探测测试媒体");
  Media result;
  for (unsigned int index = 0; index < raw->nb_streams; ++index) {
    const auto* stream = raw->streams[index];
    const auto type = stream->codecpar->codec_type;
    if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) continue;
    if (selected != AVMEDIA_TYPE_UNKNOWN && selected != type) continue;
    result.streams.push_back({static_cast<int>(index),
                              ffmpeg::CodecParameters(*stream->codecpar),
                              stream->time_base});
  }
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "读取测试媒体");
    if (std::any_of(result.streams.begin(), result.streams.end(),
                    [&](const auto& stream) {
                      return packet->stream_index == stream.stream_index;
                    })) {
      result.packets.push_back(std::move(packet));
    }
  }
  return result;
}

const ffmpeg::StreamInfo& Track(const Media& media, int index) {
  const auto found = std::find_if(
      media.streams.begin(), media.streams.end(),
      [&](const auto& stream) { return stream.stream_index == index; });
  if (found == media.streams.end()) throw std::logic_error("测试轨道不存在");
  return *found;
}

struct PacketSnapshot final {
  std::int64_t pts;
  std::int64_t dts;
  std::int64_t duration;
  int stream_index;
  int flags;
  std::vector<std::uint8_t> bytes;
  std::vector<std::vector<std::uint8_t>> side_data;

  bool operator==(const PacketSnapshot& other) const {
    return pts == other.pts && dts == other.dts && duration == other.duration &&
           stream_index == other.stream_index && flags == other.flags &&
           bytes == other.bytes && side_data == other.side_data;
  }
};

PacketSnapshot Snapshot(const AVPacket& packet) {
  PacketSnapshot result{packet.pts,
                        packet.dts,
                        packet.duration,
                        packet.stream_index,
                        packet.flags,
                        {packet.data, packet.data + packet.size},
                        {}};
  for (int index = 0; index < packet.side_data_elems; ++index) {
    const auto& side = packet.side_data[index];
    result.side_data.emplace_back(side.data, side.data + side.size);
  }
  return result;
}

struct DecodedTrack final {
  std::vector<std::int64_t> pts;
  std::vector<std::uint64_t> hashes;
};

// Native FFmpeg decoding is independent of Input/Remuxer. Ignore container
// priming side data and discard flags on both sides to compare every coded
// AAC frame; these changes affect only decoder-owned packet copies.
std::map<AVMediaType, DecodedTrack> Decode(const Media& media) {
  std::map<AVMediaType, DecodedTrack> result;
  for (const auto& stream : media.streams) {
    const auto* parameters = stream.codec_parameters.get();
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    REQUIRE(codec != nullptr);
    ffmpeg::CodecContext context(codec);
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context.get(), parameters),
        "设置参考解码器");
    context.get()->pkt_timebase = stream.time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(context.get(), codec, nullptr), "打开参考解码器");
    auto& output = result[parameters->codec_type];
    ffmpeg::Frame frame;
    const auto receive = [&] {
      for (;;) {
        const int status = avcodec_receive_frame(context.get(), frame.get());
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
        ffmpeg::FfmpegException::throwIfError(status, "解码参考媒体");
        std::uint64_t hash = 14695981039346656037ULL;
        const auto append = [&](const std::uint8_t* bytes, std::size_t size) {
          for (std::size_t index = 0; index < size; ++index) {
            hash = (hash ^ bytes[index]) * 1099511628211ULL;
          }
        };
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
          const auto format = static_cast<AVPixelFormat>(frame->format);
          const int size =
              av_image_get_buffer_size(format, frame->width, frame->height, 1);
          ffmpeg::FfmpegException::throwIfError(size, "计算图像大小");
          std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
          ffmpeg::FfmpegException::throwIfError(
              av_image_copy_to_buffer(bytes.data(), size, frame->data,
                                      frame->linesize, format, frame->width,
                                      frame->height, 1),
              "复制参考图像");
          append(bytes.data(), bytes.size());
        } else {
          const auto format = static_cast<AVSampleFormat>(frame->format);
          const bool planar = av_sample_fmt_is_planar(format) != 0;
          const int channels = frame->ch_layout.nb_channels;
          const auto size =
              static_cast<std::size_t>(frame->nb_samples) *
              static_cast<std::size_t>(av_get_bytes_per_sample(format)) *
              static_cast<std::size_t>(planar ? 1 : channels);
          for (int plane = 0; plane < (planar ? channels : 1); ++plane) {
            append(frame->extended_data[plane], size);
          }
        }
        REQUIRE(frame->best_effort_timestamp != AV_NOPTS_VALUE);
        output.pts.push_back(av_rescale_q(frame->best_effort_timestamp,
                                          stream.time_base, kMicroseconds));
        output.hashes.push_back(hash);
        frame.Unref();
      }
    };
    for (const auto& packet : media.packets) {
      if (packet->stream_index != stream.stream_index) continue;
      auto coded = packet.Clone();
      av_packet_free_side_data(coded.get());
      coded->flags &= ~AV_PKT_FLAG_DISCARD;
      ffmpeg::FfmpegException::throwIfError(
          avcodec_send_packet(context.get(), coded.get()), "提交参考包");
      receive();
    }
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(context.get(), nullptr), "排空参考解码器");
    receive();
  }
  return result;
}

struct Notifications final {
  std::atomic<int> errors{0};
  std::atomic<int> ended{0};
  void Bind(mw::streamer::SyncRemuxer& remuxer) {
    remuxer.SetOnError(
        [this](std::string_view, int, std::string_view) noexcept { ++errors; });
    remuxer.SetOnEnded([this]() noexcept { ++ended; });
  }
};

std::string Record(mw::streamer::SyncRemuxer& remuxer, const Media& media,
                   const OutputDirectory& directory, int generations = 1) {
  remuxer.Start(media.streams);
  const auto path = remuxer.AddPushUrl(directory.File());
  for (int generation = 0; generation < generations; ++generation) {
    for (const auto& packet : media.packets) {
      const auto before = Snapshot(*packet.get());
      REQUIRE(remuxer.SubmitPacket(static_cast<std::uint64_t>(generation),
                                   *packet.get()));
      REQUIRE(Snapshot(*packet.get()) == before);
    }
  }
  remuxer.Drain();
  remuxer.Stop();
  return path;
}

}  // namespace

TEST_CASE("SyncRemuxer原包直录保持H264 H265 AAC和单轨的解码内容",
          "[remuxer][sync][recording]") {
  Runtime runtime;
  OutputDirectory directory;
  std::string name;
  AVMediaType selected = AVMEDIA_TYPE_UNKNOWN;
  SECTION("H264和AAC") { name = "h264_aac.mp4"; }
  SECTION("H265含B帧和AAC") { name = "h265_aac.mp4"; }
  SECTION("只有视频") { name = "h264_video.mp4"; }
  SECTION("只有音频") {
    name = "h264_aac.mp4";
    selected = AVMEDIA_TYPE_AUDIO;
  }
  const auto source = ReadMedia(Fixture(name), selected);
  Notifications notifications;
  mw::streamer::SyncRemuxer remuxer;
  notifications.Bind(remuxer);
  const auto path = Record(remuxer, source, directory);
  CHECK(notifications.errors == 0);
  CHECK(notifications.ended == 1);
  const auto output = ReadMedia(path);
  CHECK(output.streams.size() == source.streams.size());
  const auto reference = Decode(source);
  const auto actual = Decode(output);
  REQUIRE(actual.size() == reference.size());
  for (const auto& [type, frames] : reference) {
    REQUIRE(actual.count(type) == 1);
    CHECK(actual.at(type).hashes == frames.hashes);
  }
}

TEST_CASE("decode=false Input直接录制原包且不调用解码帧回调",
          "[remuxer][sync][input]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto source = ReadMedia(Fixture("h264_aac.mp4"));
  Notifications notifications;
  mw::streamer::SyncRemuxer remuxer;
  notifications.Bind(remuxer);
  mw::streamer::FfmpegInputConfig config;
  config.decode = false;
  mw::streamer::FfmpegInput input(config);
  std::mutex mutex;
  std::condition_variable changed;
  bool finished = false;
  bool failed = false;
  std::string path;
  std::vector<PacketSnapshot> received;
  int frames = 0;
  input.SetOnReady([&](const auto& streams) {
    remuxer.Start(streams);
    path = remuxer.AddPushUrl(directory.File());
  });
  input.SetOnPacket([&](std::uint64_t generation, const auto& packet) {
    const auto before = Snapshot(*packet.get());
    if (!remuxer.SubmitPacket(generation, packet))
      throw std::runtime_error("录制拒绝包");
    if (!(Snapshot(*packet.get()) == before))
      throw std::runtime_error("原包被修改");
    received.push_back(before);
  });
  input.SetOnFrame([&](int, const auto&) { ++frames; });
  input.SetOnStateChanged([&](auto state, int, std::string_view) {
    if (state != mw::streamer::InputState::kEnded &&
        state != mw::streamer::InputState::kFailed)
      return;
    std::lock_guard lock(mutex);
    failed = state == mw::streamer::InputState::kFailed;
    finished = true;
    changed.notify_all();
  });
  input.Start(Fixture("h264_aac.mp4"));
  {
    std::unique_lock lock(mutex);
    REQUIRE(changed.wait_for(lock, 10s, [&] { return finished; }));
  }
  input.Stop();
  remuxer.Stop();
  REQUIRE_FALSE(failed);
  CHECK(frames == 0);
  REQUIRE(received.size() == source.packets.size());
  for (std::size_t index = 0; index < received.size(); ++index) {
    CHECK(received[index] == Snapshot(*source.packets[index].get()));
  }
  const auto reference = Decode(source);
  const auto actual = Decode(ReadMedia(path));
  for (const auto& [type, value] : reference)
    CHECK(actual.at(type).hashes == value.hashes);
  CHECK(notifications.errors == 0);
  CHECK(notifications.ended == 1);
}

TEST_CASE("SyncRemuxer共同原点保留音频偏移和B帧负DTS",
          "[remuxer][sync][timeline]") {
  Runtime runtime;
  OutputDirectory directory;
  auto media = ReadMedia(Fixture("h265_aac.mp4"));
  for (auto& packet : media.packets) {
    const auto& stream = Track(media, packet->stream_index);
    const bool audio =
        stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO;
    const auto shift = av_rescale_q(audio ? 10300000 : 10000000, kMicroseconds,
                                    stream.time_base);
    packet->pts += shift;
    packet->dts += shift;
  }
  const auto expected = Decode(media);
  Notifications notifications;
  mw::streamer::SyncRemuxer remuxer;
  notifications.Bind(remuxer);
  const auto actual = Decode(ReadMedia(Record(remuxer, media, directory)));
  const auto expected_offset = expected.at(AVMEDIA_TYPE_AUDIO).pts.front() -
                               expected.at(AVMEDIA_TYPE_VIDEO).pts.front();
  const auto actual_offset = actual.at(AVMEDIA_TYPE_AUDIO).pts.front() -
                             actual.at(AVMEDIA_TYPE_VIDEO).pts.front();
  CHECK(std::abs(actual_offset - expected_offset) <= 2000);
  CHECK(actual.at(AVMEDIA_TYPE_VIDEO).pts.front() < 1000000);
  CHECK(actual.at(AVMEDIA_TYPE_AUDIO).pts.front() < 1000000);
  CHECK(notifications.errors == 0);
}

TEST_CASE("SyncRemuxer合法五秒音视频偏移不受SDK启动帧数限制",
          "[remuxer][sync][timeline][startup]") {
  Runtime runtime;
  OutputDirectory directory;
  auto source = ReadMedia(Fixture("seek_h264_aac.mp4"));
  for (auto& packet : source.packets) {
    const auto& stream = Track(source, packet->stream_index);
    if (stream.codec_parameters.get()->codec_type != AVMEDIA_TYPE_VIDEO)
      continue;
    const auto shift = av_rescale_q(5000000, kMicroseconds, stream.time_base);
    packet->pts += shift;
    packet->dts += shift;
  }
  std::stable_sort(
      source.packets.begin(), source.packets.end(),
      [&](const auto& first, const auto& second) {
        return av_compare_ts(first->dts,
                             Track(source, first->stream_index).time_base,
                             second->dts,
                             Track(source, second->stream_index).time_base) < 0;
      });
  const auto first_video = std::find_if(
      source.packets.begin(), source.packets.end(), [&](const auto& packet) {
        return Track(source, packet->stream_index)
                   .codec_parameters.get()
                   ->codec_type == AVMEDIA_TYPE_VIDEO;
      });
  REQUIRE(first_video != source.packets.end());
  // More than 100 valid audio packets precede the first video packet in actual
  // demux order; an SDK startup cache limit must not silently drop any of them.
  REQUIRE(std::distance(source.packets.begin(), first_video) > 200);
  const auto expected = Decode(source);
  Notifications notifications;
  mw::streamer::SyncRemuxer remuxer;
  notifications.Bind(remuxer);
  const auto actual = Decode(ReadMedia(Record(remuxer, source, directory)));
  REQUIRE(notifications.errors == 0);
  REQUIRE(notifications.ended == 1);
  REQUIRE(actual.size() == expected.size());
  for (const auto& [type, frames] : expected) {
    REQUIRE(actual.count(type) == 1);
    CHECK(actual.at(type).hashes == frames.hashes);
  }
  const auto expected_offset = expected.at(AVMEDIA_TYPE_VIDEO).pts.front() -
                               expected.at(AVMEDIA_TYPE_AUDIO).pts.front();
  const auto actual_offset = actual.at(AVMEDIA_TYPE_VIDEO).pts.front() -
                             actual.at(AVMEDIA_TYPE_AUDIO).pts.front();
  CHECK(expected_offset >= 5000000);
  CHECK(std::abs(actual_offset - expected_offset) <= 2000);
}

TEST_CASE("SyncRemuxer代际切换接续同文件且每轨DTS单调",
          "[remuxer][sync][generation]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto source = ReadMedia(Fixture("h265_aac.mp4"));
  Notifications notifications;
  mw::streamer::SyncRemuxer remuxer;
  notifications.Bind(remuxer);
  const auto output = ReadMedia(Record(remuxer, source, directory, 2));
  std::map<int, std::int64_t> previous;
  for (const auto& packet : output.packets) {
    REQUIRE(packet->dts != AV_NOPTS_VALUE);
    if (previous.count(packet->stream_index))
      CHECK(packet->dts > previous.at(packet->stream_index));
    previous[packet->stream_index] = packet->dts;
  }
  const auto expected = Decode(source);
  const auto actual = Decode(output);
  for (const auto& [type, value] : expected) {
    auto hashes = value.hashes;
    hashes.insert(hashes.end(), value.hashes.begin(), value.hashes.end());
    if (type == AVMEDIA_TYPE_VIDEO) CHECK(actual.at(type).hashes == hashes);
    const auto count = value.pts.size();
    REQUIRE(actual.at(type).pts.size() == count * 2);
    CHECK(actual.at(type).pts[count] - actual.at(type).pts[0] >= 1900000);
    CHECK(actual.at(type).pts[count] - actual.at(type).pts[0] <= 2400000);
  }
  CHECK(notifications.errors == 0);
}

TEST_CASE("SyncRemuxer缺轨启动时切换代际仍接收后来音频",
          "[remuxer][sync][generation][startup]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto source = ReadMedia(Fixture("h264_aac.mp4"));
  const auto first_video = std::find_if(
      source.packets.begin(), source.packets.end(), [&](const auto& packet) {
        return Track(source, packet->stream_index)
                   .codec_parameters.get()
                   ->codec_type == AVMEDIA_TYPE_VIDEO;
      });
  REQUIRE(first_video != source.packets.end());
  REQUIRE(((*first_video)->flags & AV_PKT_FLAG_KEY) != 0);
  Notifications notifications;
  mw::streamer::SyncRemuxer remuxer;
  notifications.Bind(remuxer);
  remuxer.Start(source.streams);
  const auto path = remuxer.AddPushUrl(directory.File());
  REQUIRE(remuxer.SubmitPacket(0, *first_video));
  for (const auto& packet : source.packets) {
    REQUIRE(remuxer.SubmitPacket(1, packet));
  }
  remuxer.Stop();
  REQUIRE(notifications.errors == 0);
  REQUIRE(notifications.ended == 1);
  const auto output = ReadMedia(path);
  REQUIRE(output.streams.size() == 2);
  std::map<int, std::int64_t> previous;
  for (const auto& packet : output.packets) {
    REQUIRE(packet->dts != AV_NOPTS_VALUE);
    if (previous.count(packet->stream_index)) {
      CHECK(packet->dts > previous.at(packet->stream_index));
    }
    previous[packet->stream_index] = packet->dts;
  }
  const auto expected = Decode(source);
  const auto actual = Decode(output);
  REQUIRE(actual.count(AVMEDIA_TYPE_VIDEO) == 1);
  REQUIRE(actual.count(AVMEDIA_TYPE_AUDIO) == 1);
  auto video_hashes = expected.at(AVMEDIA_TYPE_VIDEO).hashes;
  REQUIRE_FALSE(video_hashes.empty());
  video_hashes.insert(video_hashes.begin(), video_hashes.front());
  CHECK(actual.at(AVMEDIA_TYPE_VIDEO).hashes == video_hashes);
  CHECK(actual.at(AVMEDIA_TYPE_AUDIO).hashes ==
        expected.at(AVMEDIA_TYPE_AUDIO).hashes);
}

TEST_CASE("SyncRemuxer非法配置时间戳和超大包明确拒绝",
          "[remuxer][sync][validation]") {
  Runtime runtime;
  mw::streamer::SyncRemuxerConfig invalid;
  invalid.max_pending_bytes = 0;
  CHECK_THROWS_AS(mw::streamer::SyncRemuxer(invalid), std::invalid_argument);
  const auto media = ReadMedia(Fixture("h264_video.mp4"));
  mw::streamer::SyncRemuxerConfig config;
  config.max_pending_bytes =
      static_cast<std::size_t>(media.packets.front()->size);
  mw::streamer::SyncRemuxer remuxer(config);
  remuxer.Start(media.streams);
  auto packet = media.packets.front().Clone();
  packet->pts = AV_NOPTS_VALUE;
  CHECK_THROWS_AS(remuxer.SubmitPacket(0, packet), std::invalid_argument);
  packet = media.packets.front().Clone();
  packet->dts = AV_NOPTS_VALUE;
  CHECK_THROWS_AS(remuxer.SubmitPacket(0, packet), std::invalid_argument);
  packet = media.packets.front().Clone();
  packet->time_base = {1, 3};
  CHECK_THROWS_AS(remuxer.SubmitPacket(0, packet), std::invalid_argument);
  packet->time_base = {-1, 1};
  CHECK_THROWS_AS(remuxer.SubmitPacket(0, packet), std::invalid_argument);
  packet->time_base = {1, 0};
  CHECK_THROWS_AS(remuxer.SubmitPacket(0, packet), std::invalid_argument);
  ffmpeg::Packet oversized;
  ffmpeg::FfmpegException::throwIfError(
      av_new_packet(oversized.get(), media.packets.front()->size + 1),
      "分配超大测试包");
  oversized->stream_index = media.streams.front().stream_index;
  oversized->pts = oversized->dts = 0;
  CHECK_THROWS_AS(remuxer.SubmitPacket(0, oversized), std::length_error);
  packet = media.packets.front().Clone();
  packet->time_base = {0, 1};
  REQUIRE(remuxer.SubmitPacket(0, *packet.get()));
  CHECK(packet->time_base.num == 0);
  CHECK(packet->time_base.den == 1);
  remuxer.Stop();
  CHECK_FALSE(remuxer.SubmitPacket(0, media.packets.front()));
}

TEST_CASE("SyncRemuxer满队列等待且Drain和Stop解除提交阻塞",
          "[remuxer][sync][backpressure]") {
  Runtime runtime;
  const auto media = ReadMedia(Fixture("h264_video.mp4"));
  mw::streamer::SyncRemuxerConfig config;
  config.max_pending_bytes =
      static_cast<std::size_t>(media.packets.front()->size);
  REQUIRE(media.packets.size() >= 2);
  REQUIRE(media.packets.at(1)->size <= media.packets.front()->size);
  REQUIRE(media.packets.at(1)->dts > media.packets.front()->dts);
  mw::streamer::SyncRemuxer remuxer(config);
  remuxer.Start(media.streams);
  encoder_test::Gate gate;
  encoder_test::ReleaseGate release{gate};
  toolkit::EventPollerPool::Instance().getPoller()->async(
      [&] { gate.Enter(); });
  REQUIRE(gate.Wait());
  REQUIRE(remuxer.SubmitPacket(0, media.packets.front()));
  std::promise<void> started;
  auto entered = started.get_future();
  auto submit = std::async(std::launch::async, [&] {
    started.set_value();
    return remuxer.SubmitPacket(0, media.packets.at(1));
  });
  // This guard precedes future destruction even if an assertion fails.
  encoder_test::ReleaseGate release_submit{gate};
  entered.wait();
  CHECK(submit.wait_for(100ms) == std::future_status::timeout);
  SECTION("Drain解除阻塞") {
    remuxer.Drain();
    const auto status = submit.wait_for(2s);
    CHECK(status == std::future_status::ready);
    gate.Release();
    CHECK_FALSE(submit.get());
    remuxer.Stop();
  }
  SECTION("Stop解除阻塞") {
    auto stop = std::async(std::launch::async, [&] { remuxer.Stop(); });
    encoder_test::ReleaseGate release_stop{gate};
    const auto status = submit.wait_for(2s);
    CHECK(status == std::future_status::ready);
    gate.Release();
    CHECK_FALSE(submit.get());
    CHECK(stop.wait_for(5s) == std::future_status::ready);
    stop.get();
  }
}

TEST_CASE("SyncRemuxer声明双轨实际单轨EOF可排空而启动缓存耗尽明确失败",
          "[remuxer][sync][startup]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto source = ReadMedia(Fixture("h264_aac.mp4"));
  auto video = ReadMedia(Fixture("h264_aac.mp4"), AVMEDIA_TYPE_VIDEO);
  SECTION("EOF只收到视频仍可封装") {
    video.streams = source.streams;
    Notifications notifications;
    mw::streamer::SyncRemuxer remuxer;
    notifications.Bind(remuxer);
    const auto actual = Decode(ReadMedia(Record(remuxer, video, directory)));
    CHECK(actual.at(AVMEDIA_TYPE_VIDEO).hashes ==
          Decode(video).at(AVMEDIA_TYPE_VIDEO).hashes);
    CHECK(notifications.errors == 0);
  }
  SECTION("启动缺轨无法释放容量时报告错误而不自锁") {
    mw::streamer::SyncRemuxerConfig config;
    config.max_pending_bytes =
        static_cast<std::size_t>(video.packets.front()->size);
    Notifications notifications;
    mw::streamer::SyncRemuxer remuxer(config);
    notifications.Bind(remuxer);
    remuxer.Start(source.streams);
    REQUIRE(remuxer.SubmitPacket(0, video.packets.front()));
    auto submit = std::async(std::launch::async, [&] {
      return remuxer.SubmitPacket(0, video.packets.at(1));
    });
    const auto status = submit.wait_for(5s);
    CHECK(status == std::future_status::ready);
    if (status != std::future_status::ready) remuxer.Drain();
    CHECK_FALSE(submit.get());
    remuxer.Stop();
    CHECK(notifications.errors == 1);
  }
}
