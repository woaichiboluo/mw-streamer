#include "streamer/clip/clip_output.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::ClipConfig;
using mw::streamer::ClipRequest;
using mw::streamer::internal::ClipOutput;

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

class Directory final {
 public:
  Directory()
      : path_(fs::temp_directory_path() /
              ("mw-clip-" + std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()))) {
    fs::create_directories(path_);
  }
  ~Directory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  std::string File(const std::string& name) const {
    return (path_ / name).generic_u8string();
  }

 private:
  fs::path path_;
};

struct Media final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;

  explicit Media(const std::string& path) {
    AVFormatContext* raw = nullptr;
    REQUIRE(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0);
    std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
    REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
    for (unsigned index = 0; index < input->nb_streams; ++index) {
      const auto* stream = input->streams[index];
      ffmpeg::StreamInfo info;
      info.stream_index = stream->index;
      info.time_base = stream->time_base;
      info.codec_parameters = ffmpeg::CodecParameters(*stream->codecpar);
      streams.push_back(std::move(info));
    }
    ffmpeg::Packet packet;
    int result = 0;
    while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
      packet->time_base = input->streams[packet->stream_index]->time_base;
      packets.push_back(packet.Ref());
      packet.Unref();
    }
    REQUIRE(result == AVERROR_EOF);
  }
};

Media Source(const std::string& filename = "h264_aac.mp4") {
  Media media(
      (fs::u8path(MW_STREAMER_TEST_DATA_DIR) / filename).generic_u8string());
  for (auto& packet : media.packets) {
    const auto offset = av_rescale_q(30, AVRational{1, 1}, packet->time_base);
    packet->pts += offset;
    packet->dts += offset;
  }
  return media;
}

std::int64_t PtsUs(const ffmpeg::Packet& packet) {
  return av_rescale_q(packet->pts, packet->time_base, AV_TIME_BASE_Q);
}

void Feed(ClipOutput& output, const Media& media, std::uint64_t generation) {
  for (const auto& packet : media.packets) {
    const auto& track =
        media.streams[static_cast<std::size_t>(packet->stream_index)];
    if (track.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      ffmpeg::Frame source;
      ffmpeg::Frame encoded;
      encoded->pts = packet->pts;
      encoded->time_base = track.time_base;
      // The source jumps within one generation. Historical events must use
      // their actual correspondence rather than the latest source offset.
      source->pts =
          packet->pts + av_rescale_q(PtsUs(packet) < 31000000 ? -20 : 20,
                                     AVRational{1, 1}, track.time_base);
      source->time_base = track.time_base;
      output.MapVideo(generation, source, encoded);
    } else {
      ffmpeg::Frame encoded;
      encoded->pts = packet->pts;
      encoded->time_base = track.time_base;
      output.MapAudio(generation, encoded);
    }
    output.SubmitPacket(packet);
  }
}

ClipRequest Request(const std::string& path, std::uint64_t generation = 1) {
  ClipRequest request;
  request.event_id = "goal";
  request.time = {generation, 10500, {1, 1000}};
  request.before = 200ms;
  request.after = 200ms;
  request.path = path;
  return request;
}

std::size_t Decode(const ffmpeg::StreamInfo& stream,
                   const std::vector<ffmpeg::Packet>& packets) {
  const auto* codec =
      avcodec_find_decoder(stream.codec_parameters.get()->codec_id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecCloser> decoder(
      avcodec_alloc_context3(codec));
  REQUIRE(decoder != nullptr);
  REQUIRE(avcodec_parameters_to_context(decoder.get(),
                                        stream.codec_parameters.get()) >= 0);
  decoder->pkt_timebase = stream.time_base;
  decoder->thread_count = 1;
  REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  std::size_t frames = 0;
  const auto receive = [&] {
    ffmpeg::Frame frame;
    int result = 0;
    while ((result = avcodec_receive_frame(decoder.get(), frame.get())) >= 0) {
      ++frames;
      frame.Unref();
    }
    REQUIRE((result == AVERROR(EAGAIN) || result == AVERROR_EOF));
  };
  for (const auto& packet : packets) {
    if (packet->stream_index != stream.stream_index) continue;
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    receive();
  }
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  receive();
  return frames;
}

void CheckClip(const std::string& path, const Media& original,
               std::int64_t start_us = 30000000, std::int64_t end_us = 31000000,
               std::int64_t audio_start_us = 0) {
  REQUIRE(fs::exists(fs::u8path(path)));
  Media clip(path);
  REQUIRE(clip.streams.size() == 2);
  std::vector<std::int64_t> origins;
  for (const auto& stream : clip.streams) {
    std::vector<const ffmpeg::Packet*> selected;
    for (const auto& packet : clip.packets) {
      if (packet->stream_index == stream.stream_index)
        selected.push_back(&packet);
    }
    REQUIRE_FALSE(selected.empty());
    const auto decoded_frames = Decode(stream, clip.packets);
    CHECK(decoded_frames > 0);
    const auto& source_stream =
        original.streams[static_cast<std::size_t>(stream.stream_index)];
    CHECK(stream.codec_parameters.get()->codec_id ==
          source_stream.codec_parameters.get()->codec_id);
    std::vector<const ffmpeg::Packet*> expected;
    const auto track_start =
        stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO
            ? std::max(start_us, audio_start_us)
            : start_us;
    for (const auto& packet : original.packets) {
      if (packet->stream_index == stream.stream_index &&
          PtsUs(packet) >= track_start && PtsUs(packet) < end_us)
        expected.push_back(&packet);
    }
    REQUIRE(selected.size() == expected.size());
    std::int64_t last_dts = AV_NOPTS_VALUE;
    std::vector<std::int64_t> track_origins;
    for (std::size_t index = 0; index < selected.size(); ++index) {
      const auto& actual = *selected[index];
      const auto& source = *expected[index];
      REQUIRE(actual->size == source->size);
      CHECK(
          std::equal(source->data, source->data + source->size, actual->data));
      const auto origin = PtsUs(source) - PtsUs(actual);
      origins.push_back(origin);
      track_origins.push_back(origin);
      if (last_dts != AV_NOPTS_VALUE) CHECK(actual->dts > last_dts);
      last_dts = actual->dts;
    }
    const auto [track_minimum, track_maximum] =
        std::minmax_element(track_origins.begin(), track_origins.end());
    CAPTURE(stream.stream_index);
    CHECK(*track_maximum - *track_minimum <= 1);
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      CHECK(((*selected.front())->flags & AV_PKT_FLAG_KEY) != 0);
      CHECK(decoded_frames == expected.size());
    }
  }
  // All packets, across both tracks, retain one common presentation origin.
  const auto [minimum, maximum] =
      std::minmax_element(origins.begin(), origins.end());
  CHECK(*maximum - *minimum <= 2);
}

}  // namespace

TEST_CASE("ClipOutput maps delayed events to historical output GOPs",
          "[clip][integration][mapping]") {
  const Directory directory;
  const auto media = Source();
  ClipConfig config;
  config.enabled = true;
  ClipOutput output(config);
  output.Start(media.streams);
  output.SetGeneration(1);
  Feed(output, media, 1);
  output.SubmitClip(Request(directory.File("history.mp4")));
  output.Stop();
  CheckClip(directory.File("history.mp4"), media);
}

TEST_CASE("ClipOutput waits for an event frame to reach output",
          "[clip][integration][pending]") {
  const Directory directory;
  const auto media = Source();
  ClipConfig config;
  config.enabled = true;
  ClipOutput output(config);
  output.Start(media.streams);
  output.SetGeneration(1);
  output.SubmitClip(Request(directory.File("pending.mp4")));
  auto invalid = Request(directory.File("invalid.mp4"));
  invalid.time.pts = AV_NOPTS_VALUE;
  output.SubmitClip(invalid);
  Feed(output, media, 1);
  output.Stop();
  CheckClip(directory.File("pending.mp4"), media);
  CHECK_FALSE(fs::exists(fs::u8path(directory.File("invalid.mp4"))));
}

TEST_CASE("ClipOutput discards old-generation events and pending work",
          "[clip][generation]") {
  const Directory directory;
  const auto media = Source();
  ClipConfig config;
  config.enabled = true;
  ClipOutput output(config);
  output.Start(media.streams);
  output.SetGeneration(1);
  output.SubmitClip(Request(directory.File("discarded.mp4")));
  output.SetGeneration(2);
  CHECK(output.generation() == 2);
  Feed(output, media, 2);
  output.SubmitClip(Request(directory.File("stale.mp4")));
  output.SubmitClip(Request(directory.File("current.mp4"), 2));
  output.Stop();
  CHECK_FALSE(fs::exists(fs::u8path(directory.File("discarded.mp4"))));
  CHECK_FALSE(fs::exists(fs::u8path(directory.File("stale.mp4"))));
  CheckClip(directory.File("current.mp4"), media);
}

TEST_CASE("ClipOutput refuses clips after its byte budget evicts history",
          "[clip][capacity]") {
  const Directory directory;
  const auto media = Source();
  ClipConfig config;
  config.enabled = true;
  config.max_cache_bytes = 1;
  ClipOutput output(config);
  output.Start(media.streams);
  output.SetGeneration(1);
  Feed(output, media, 1);
  output.SubmitClip(Request(directory.File("evicted.mp4")));
  output.Stop();
  CHECK_FALSE(fs::exists(fs::u8path(directory.File("evicted.mp4"))));
}

TEST_CASE("ClipOutput starts HEVC CRA clips without external leading pictures",
          "[clip][integration][hevc][b-frames]") {
  const bool near_boundary = GENERATE(false, true);
  const Directory directory;
  const auto media = Source("h265_aac.mp4");
  ClipConfig config;
  config.enabled = true;
  ClipOutput output(config);
  output.Start(media.streams);
  output.SetGeneration(1);
  std::vector<const ffmpeg::Packet*> video;
  for (const auto& packet : media.packets) {
    if (media.streams[static_cast<std::size_t>(packet->stream_index)]
            .codec_parameters.get()
            ->codec_type == AVMEDIA_TYPE_VIDEO)
      video.push_back(&packet);
  }
  // Syncer observes display-order frames before encoder reorders their packets.
  std::sort(video.begin(), video.end(),
            [](const auto* left, const auto* right) {
              return (*left)->pts < (*right)->pts;
            });
  for (const auto* packet : video) {
    ffmpeg::Frame source;
    ffmpeg::Frame encoded;
    encoded->pts = (*packet)->pts;
    encoded->time_base = (*packet)->time_base;
    source->pts = (*packet)->pts -
                  av_rescale_q(20, AVRational{1, 1}, (*packet)->time_base);
    source->time_base = (*packet)->time_base;
    output.MapVideo(1, source, encoded);
  }
  auto request = Request(directory.File("cra.mp4"));
  request.time.pts = near_boundary ? 10700 : 11500;
  request.after = near_boundary ? 290ms : 200ms;
  output.SubmitClip(request);
  for (const auto& packet : media.packets) {
    if (media.streams[static_cast<std::size_t>(packet->stream_index)]
            .codec_parameters.get()
            ->codec_type == AVMEDIA_TYPE_AUDIO) {
      ffmpeg::Frame encoded;
      encoded->pts = packet->pts;
      encoded->time_base = packet->time_base;
      output.MapAudio(1, encoded);
    }
    output.SubmitPacket(packet);
  }
  // A subsequent IDR closes the selected GOP. Its references and the boundary
  // audio packet only advance the watermark; neither belongs in the clip.
  auto boundary_video = video.front()->Ref();
  const auto video_shift =
      av_rescale_q(2, AVRational{1, 1}, boundary_video->time_base);
  boundary_video->pts += video_shift;
  boundary_video->dts += video_shift;
  output.SubmitPacket(boundary_video);
  const auto first_audio = std::find_if(
      media.packets.begin(), media.packets.end(), [&](const auto& packet) {
        return media.streams[static_cast<std::size_t>(packet->stream_index)]
                   .codec_parameters.get()
                   ->codec_type == AVMEDIA_TYPE_AUDIO;
      });
  REQUIRE(first_audio != media.packets.end());
  auto boundary_audio = first_audio->Ref();
  boundary_audio->pts =
      av_rescale_q(32, AVRational{1, 1}, boundary_audio->time_base);
  boundary_audio->dts = boundary_audio->pts;
  ffmpeg::Frame encoded_audio;
  encoded_audio->pts = boundary_audio->pts;
  encoded_audio->time_base = boundary_audio->time_base;
  output.MapAudio(1, encoded_audio);
  output.SubmitPacket(boundary_audio);
  output.Stop();
  // The 30.99s request needs the 30.9s leading picture that arrives after the
  // 31s CRA in decode order. Keep that GOP instead of ending the picture early.
  CheckClip(directory.File("cra.mp4"), media,
            near_boundary ? 30000000 : 31000000, 32000000);
}

TEST_CASE("ClipOutput excludes delayed audio packets from the previous epoch",
          "[clip][generation][audio]") {
  const Directory directory;
  const auto media = Source();
  ClipConfig config;
  config.enabled = true;
  ClipOutput output(config);
  output.Start(media.streams);
  output.SetGeneration(1);
  Feed(output, media, 1);
  output.SetGeneration(2);
  ffmpeg::Frame video_source;
  ffmpeg::Frame video_output;
  video_source->pts = 10000000;
  video_source->time_base = AV_TIME_BASE_Q;
  video_output->pts = 30000000;
  video_output->time_base = AV_TIME_BASE_Q;
  output.MapVideo(2, video_source, video_output);
  ffmpeg::Frame audio_output;
  audio_output->pts = 30100000;
  audio_output->time_base = AV_TIME_BASE_Q;
  output.MapAudio(2, audio_output);
  const auto stale = std::find_if(
      media.packets.begin(), media.packets.end(), [&](const auto& packet) {
        return media.streams[static_cast<std::size_t>(packet->stream_index)]
                       .codec_parameters.get()
                       ->codec_type == AVMEDIA_TYPE_AUDIO &&
               PtsUs(packet) > 30000000 && PtsUs(packet) < 30100000;
      });
  REQUIRE(stale != media.packets.end());
  output.SubmitPacket(*stale);
  Feed(output, media, 2);
  output.SubmitClip(Request(directory.File("new-audio.mp4"), 2));
  output.Stop();
  CheckClip(directory.File("new-audio.mp4"), media, 30000000, 31000000,
            30100000);
}
