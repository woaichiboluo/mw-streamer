#include "mw/streamer/recorder/recorder.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::Recorder;
using mw::streamer::RecorderConfig;
using mw::streamer::RecorderState;

class OutputDirectory final {
 public:
  OutputDirectory()
      : path_(fs::temp_directory_path() /
              ("mw-recorder-" + std::to_string(std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count()))) {
    fs::create_directories(path_);
  }
  ~OutputDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  std::string File(const std::string& name) const {
    return (path_ / name).generic_u8string();
  }

 private:
  fs::path path_;
};

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, FormatCloser> OpenMedia(
    const std::string& path) {
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0);
  std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
  return input;
}

struct Media final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;

  explicit Media(bool video = true, bool audio = false,
                 const std::string& filename = "h264_aac.mp4") {
    const auto path = fs::u8path(MW_STREAMER_TEST_DATA_DIR) / filename;
    const auto input = OpenMedia(path.generic_u8string());
    std::vector<int> selected(input->nb_streams, -1);
    for (unsigned index = 0; index < input->nb_streams; ++index) {
      const auto* stream = input->streams[index];
      const auto type = stream->codecpar->codec_type;
      if ((video && type == AVMEDIA_TYPE_VIDEO) ||
          (audio && type == AVMEDIA_TYPE_AUDIO)) {
        ffmpeg::StreamInfo info;
        info.stream_index = type == AVMEDIA_TYPE_VIDEO ? 4 : 7;
        info.time_base = stream->time_base;
        info.codec_parameters = ffmpeg::CodecParameters(*stream->codecpar);
        selected[index] = static_cast<int>(streams.size());
        streams.push_back(std::move(info));
      }
    }
    REQUIRE(streams.size() == static_cast<std::size_t>(video + audio));
    ffmpeg::Packet packet;
    int result = 0;
    while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
      const int selected_index =
          selected[static_cast<std::size_t>(packet->stream_index)];
      if (selected_index >= 0) {
        const auto& info = streams[static_cast<std::size_t>(selected_index)];
        packet->stream_index = info.stream_index;
        packet->time_base = info.time_base;
        const auto offset = av_rescale_q(10, AVRational{1, 1}, info.time_base);
        if (packet->pts != AV_NOPTS_VALUE) packet->pts += offset;
        if (packet->dts != AV_NOPTS_VALUE) packet->dts += offset;
        packets.push_back(packet.Ref());
      }
      packet.Unref();
    }
    REQUIRE(result == AVERROR_EOF);
    REQUIRE_FALSE(packets.empty());
  }
};

struct CodecCloser {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

std::size_t DecodeCount(const ffmpeg::StreamInfo& stream,
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
  std::size_t count = 0;
  const auto receive = [&] {
    while (true) {
      ffmpeg::Frame frame;
      const int result = avcodec_receive_frame(decoder.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      ++count;
    }
  };
  for (const auto& packet : packets) {
    if (packet->stream_index != stream.stream_index) continue;
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    receive();
  }
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  receive();
  return count;
}

void CheckRecording(const std::string& path, const Media& source) {
  const auto input = OpenMedia(path);
  REQUIRE(input->nb_streams == source.streams.size());
  std::vector<ffmpeg::Packet> packets;
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
    packets.push_back(packet.Ref());
    packet.Unref();
  }
  REQUIRE(result == AVERROR_EOF);
  REQUIRE(packets.size() == source.packets.size());
  std::vector<std::int64_t> original_start;
  std::vector<std::int64_t> recorded_start;
  for (const auto& original : source.streams) {
    const int index = av_find_best_stream(
        input.get(), original.codec_parameters.get()->codec_type, -1, -1,
        nullptr, 0);
    REQUIRE(index >= 0);
    const auto* stream = input->streams[index];
    ffmpeg::StreamInfo recorded;
    recorded.stream_index = index;
    recorded.time_base = stream->time_base;
    recorded.codec_parameters = ffmpeg::CodecParameters(*stream->codecpar);
    CHECK(recorded.codec_parameters.get()->codec_id ==
          original.codec_parameters.get()->codec_id);
    const auto original_count = DecodeCount(original, source.packets);
    REQUIRE(original_count > 0);
    CHECK(DecodeCount(recorded, packets) == original_count);
    const auto first_original = std::find_if(
        source.packets.begin(), source.packets.end(), [&](const auto& item) {
          return item->stream_index == original.stream_index;
        });
    const auto first_recorded = std::find_if(
        packets.begin(), packets.end(),
        [&](const auto& item) { return item->stream_index == index; });
    REQUIRE(first_original != source.packets.end());
    REQUIRE(first_recorded != packets.end());
    original_start.push_back(av_rescale_q((*first_original)->pts,
                                          original.time_base, AV_TIME_BASE_Q));
    recorded_start.push_back(av_rescale_q((*first_recorded)->pts,
                                          recorded.time_base, AV_TIME_BASE_Q));
    std::int64_t last_dts = AV_NOPTS_VALUE;
    for (const auto& recorded_packet : packets) {
      if (recorded_packet->stream_index != index) continue;
      REQUIRE(recorded_packet->dts != AV_NOPTS_VALUE);
      if (last_dts != AV_NOPTS_VALUE) CHECK(recorded_packet->dts > last_dts);
      last_dts = recorded_packet->dts;
    }
  }
  if (source.streams.size() == 2) {
    // Both tracks must use a common origin; independently zeroing each track
    // would erase the source's audio/video presentation offset.
    const auto expected_offset = original_start[0] - original_start[1];
    const auto actual_offset = recorded_start[0] - recorded_start[1];
    CHECK(actual_offset >= expected_offset - 1000);
    CHECK(actual_offset <= expected_offset + 1000);
  }
}

bool WaitForState(Recorder& recorder, RecorderState state) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  do {
    if (recorder.status().state == state) return true;
    std::this_thread::sleep_for(2ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return recorder.status().state == state;
}

struct OutputFormatCloser {
  void operator()(AVFormatContext* context) const {
    if (context->pb) avio_closep(&context->pb);
    avformat_free_context(context);
  }
};

void WriteAdts(const std::string& path, const Media& source) {
  REQUIRE(source.streams.size() == 1);
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_alloc_output_context2(&raw, nullptr, "adts", path.c_str()) >=
          0);
  std::unique_ptr<AVFormatContext, OutputFormatCloser> output(raw);
  REQUIRE(output != nullptr);
  auto* stream = avformat_new_stream(output.get(), nullptr);
  REQUIRE(stream != nullptr);
  REQUIRE(avcodec_parameters_copy(
              stream->codecpar,
              source.streams.front().codec_parameters.get()) >= 0);
  stream->time_base = source.streams.front().time_base;
  REQUIRE(avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0);
  REQUIRE(avformat_write_header(output.get(), nullptr) >= 0);
  for (const auto& original : source.packets) {
    auto packet = original.Ref();
    av_packet_rescale_ts(packet.get(), source.streams.front().time_base,
                         stream->time_base);
    packet->stream_index = stream->index;
    REQUIRE(av_interleaved_write_frame(output.get(), packet.get()) >= 0);
  }
  REQUIRE(av_write_trailer(output.get()) >= 0);
}

}  // namespace

TEST_CASE("Recorder writes independently decodable fragmented MP4",
          "[recorder][integration]") {
  bool video = true;
  bool audio = false;
  std::string filename = "h264_aac.mp4";
  SECTION("video") {}
  SECTION("audio") {
    video = false;
    audio = true;
  }
  SECTION("audio and video") { audio = true; }
  SECTION("H265 video") { filename = "h265_video.mp4"; }
  const Media media(video, audio, filename);
  const OutputDirectory output;
  Recorder recorder;
  CHECK(recorder.status().state == RecorderState::kStopped);
  CHECK_FALSE(recorder.TrySubmitPacket(media.packets.front()));
  const auto path = output.File("record.mp4");
  REQUIRE(recorder.Start(media.streams, RecorderConfig{path}));
  CHECK_FALSE(
      recorder.Start(media.streams, RecorderConfig{output.File("other.mp4")}));
  const auto original = media.packets.front().Clone();
  for (const auto& packet : media.packets) {
    REQUIRE(recorder.TrySubmitPacket(packet));
  }
  recorder.Stop();
  recorder.Stop();
  const auto status = recorder.status();
  CHECK(status.state == RecorderState::kStopped);
  CHECK(status.error.empty());
  CHECK(status.queue_bytes == 0);
  CHECK(status.written_packets == media.packets.size());
  CHECK(status.dropped_packets == 0);
  CHECK(media.packets.front()->pts == original->pts);
  CHECK(media.packets.front()->dts == original->dts);
  CHECK(media.packets.front()->duration == original->duration);
  CHECK(media.packets.front()->stream_index == original->stream_index);
  CHECK(av_cmp_q(media.packets.front()->time_base, original->time_base) == 0);
  REQUIRE(media.packets.front()->size == original->size);
  CHECK(std::equal(original->data, original->data + original->size,
                   media.packets.front()->data));
  CheckRecording(path, media);
}

TEST_CASE("Recorder preserves raw AAC without AudioSpecificConfig",
          "[recorder][integration][raw-aac]") {
  bool video = false;
  SECTION("audio only") {}
  SECTION("audio and video") { video = true; }
  const Media original(video, true);
  auto streams = original.streams;
  for (auto& stream : streams) {
    auto* parameters = stream.codec_parameters.get();
    if (parameters->codec_id != AV_CODEC_ID_AAC) continue;
    REQUIRE(parameters->extradata_size > 0);
    av_freep(&parameters->extradata);
    parameters->extradata_size = 0;
    parameters->profile = AV_PROFILE_UNKNOWN;
  }
  std::vector<ffmpeg::Packet> snapshots;
  for (const auto& packet : original.packets)
    snapshots.push_back(packet.Clone());
  const OutputDirectory output;
  const auto path = output.File("raw-aac.mp4");
  Recorder recorder;
  REQUIRE(recorder.Start(streams, RecorderConfig{path}));
  bool accepted = true;
  for (const auto& packet : original.packets) {
    if (!recorder.TrySubmitPacket(packet)) {
      accepted = false;
      break;
    }
  }
  recorder.Stop();
  const auto status = recorder.status();
  INFO(status.error);
  REQUIRE(status.state == RecorderState::kStopped);
  CHECK(accepted);
  CHECK(status.error.empty());
  CHECK(status.written_packets == original.packets.size());
  CHECK(status.dropped_packets == 0);
  CHECK(status.queue_bytes == 0);
  for (std::size_t index = 0; index < original.packets.size(); ++index) {
    const auto& packet = original.packets[index];
    const auto& snapshot = snapshots[index];
    CHECK(packet->pts == snapshot->pts);
    CHECK(packet->dts == snapshot->dts);
    CHECK(packet->duration == snapshot->duration);
    CHECK(packet->stream_index == snapshot->stream_index);
    CHECK(av_cmp_q(packet->time_base, snapshot->time_base) == 0);
    REQUIRE(packet->size == snapshot->size);
    CHECK(std::equal(snapshot->data, snapshot->data + snapshot->size,
                     packet->data));
  }

  const auto recorded = OpenMedia(path);
  const int audio_index = av_find_best_stream(
      recorded.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  REQUIRE(audio_index >= 0);
  std::vector<ffmpeg::Packet> audio_packets;
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(recorded.get(), packet.get())) >= 0) {
    if (packet->stream_index == audio_index)
      audio_packets.push_back(packet.Ref());
    packet.Unref();
  }
  REQUIRE(result == AVERROR_EOF);
  std::size_t audio_count = 0;
  for (const auto& item : original.packets) {
    const auto source =
        std::find_if(original.streams.begin(), original.streams.end(),
                     [&](const auto& stream) {
                       return stream.stream_index == item->stream_index;
                     });
    REQUIRE(source != original.streams.end());
    if (source->codec_parameters.get()->codec_id != AV_CODEC_ID_AAC) continue;
    REQUIRE(audio_count < audio_packets.size());
    REQUIRE(item->size > 2);
    REQUIRE_FALSE((item->data[0] == 0xff && (item->data[1] & 0xf6) == 0xf0));
    const auto& actual = audio_packets[audio_count++];
    REQUIRE(actual->size == item->size);
    CHECK(std::equal(item->data, item->data + item->size, actual->data));
  }
  CHECK(audio_packets.size() == audio_count);
  REQUIRE(audio_count > 0);
  CheckRecording(path, original);
}

TEST_CASE("Recorder converts ADTS AAC into decodable fragmented MP4",
          "[recorder][integration][adts]") {
  const Media original(false, true);
  const OutputDirectory output;
  const auto adts_path = output.File("source.aac");
  WriteAdts(adts_path, original);
  const auto input = OpenMedia(adts_path);
  REQUIRE(input->nb_streams == 1);
  auto stream = original.streams.front();
  stream.time_base = input->streams[0]->time_base;
  SECTION("source retains AudioSpecificConfig") {
    REQUIRE(stream.codec_parameters.get()->extradata_size > 0);
  }
  SECTION("source has no AudioSpecificConfig") {
    av_freep(&stream.codec_parameters.get()->extradata);
    stream.codec_parameters.get()->extradata_size = 0;
    stream.codec_parameters.get()->profile = AV_PROFILE_UNKNOWN;
  }
  std::vector<ffmpeg::Packet> packets;
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
    packet->stream_index = stream.stream_index;
    packet->time_base = stream.time_base;
    REQUIRE(packet->size > 7);
    REQUIRE(packet->data[0] == 0xff);
    REQUIRE((packet->data[1] & 0xf6) == 0xf0);
    packets.push_back(packet.Clone());
    packet.Unref();
  }
  REQUIRE(result == AVERROR_EOF);
  REQUIRE(packets.size() == original.packets.size());
  std::vector<ffmpeg::Packet> snapshots;
  for (const auto& item : packets) snapshots.push_back(item.Clone());
  const auto path = output.File("adts.mp4");
  Recorder recorder;
  REQUIRE(recorder.Start({stream}, RecorderConfig{path}));
  for (const auto& item : packets) REQUIRE(recorder.TrySubmitPacket(item));
  recorder.Stop();
  const auto status = recorder.status();
  INFO(status.error);
  REQUIRE(status.state == RecorderState::kStopped);
  CHECK(status.error.empty());
  CHECK(status.written_packets == packets.size());
  CHECK(status.dropped_packets == 0);
  CHECK(status.queue_bytes == 0);
  const auto recorded = OpenMedia(path);
  REQUIRE(recorded->nb_streams == 1);
  CHECK(recorded->streams[0]->codecpar->extradata_size > 0);
  for (std::size_t index = 0; index < packets.size(); ++index) {
    const auto& actual = packets[index];
    const auto& snapshot = snapshots[index];
    CHECK(actual->pts == snapshot->pts);
    CHECK(actual->dts == snapshot->dts);
    CHECK(actual->duration == snapshot->duration);
    CHECK(actual->stream_index == snapshot->stream_index);
    CHECK(av_cmp_q(actual->time_base, snapshot->time_base) == 0);
    REQUIRE(actual->size == snapshot->size);
    CHECK(std::equal(snapshot->data, snapshot->data + snapshot->size,
                     actual->data));
  }
  // ADTS does not retain MP4's SKIP_SAMPLES packet metadata. Compare against
  // the actual demuxed ADTS packets, using ASC for standalone decoding even
  // when the Recorder was intentionally given no extradata.
  auto adts_source = original;
  adts_source.streams.front().time_base = stream.time_base;
  adts_source.packets = std::move(snapshots);
  CheckRecording(path, adts_source);
}

TEST_CASE("Recorder Drain finalizes asynchronously before Stop joins",
          "[recorder][integration][drain]") {
  const Media media(true, true);
  const OutputDirectory output;
  const auto path = output.File("drained.mp4");
  Recorder recorder;
  REQUIRE(recorder.Start(media.streams, RecorderConfig{path}));
  for (const auto& packet : media.packets) {
    REQUIRE(recorder.TrySubmitPacket(packet));
  }
  recorder.Drain();
  CHECK_FALSE(recorder.TrySubmitPacket(media.packets.front()));
  // Finalization must finish without Stop joining or driving the worker.
  REQUIRE(WaitForState(recorder, RecorderState::kStopped));
  const auto status = recorder.status();
  CHECK(status.error.empty());
  CHECK(status.queue_bytes == 0);
  CHECK(status.written_packets == media.packets.size());
  CHECK(status.dropped_packets == 0);
  recorder.Stop();
  CheckRecording(path, media);
}

TEST_CASE("Recorder validates parameters without starting a session",
          "[recorder]") {
  const Media media;
  const OutputDirectory output;
  Recorder recorder;
  CHECK_THROWS_AS(recorder.Start({}, RecorderConfig{output.File("empty.mp4")}),
                  std::invalid_argument);
  CHECK_THROWS_AS(recorder.Start(media.streams, RecorderConfig{}),
                  std::invalid_argument);
  CHECK_THROWS_AS(
      recorder.Start(media.streams,
                     RecorderConfig{output.File("unsupported.m3u8")}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      recorder.Start(media.streams, RecorderConfig{output.File("zero.mp4"), 0}),
      std::invalid_argument);
  auto invalid = media.streams;
  invalid.front().time_base = {0, 1};
  CHECK_THROWS_AS(
      recorder.Start(invalid, RecorderConfig{output.File("invalid.mp4")}),
      std::invalid_argument);
  CHECK(recorder.status().state == RecorderState::kStopped);
}

TEST_CASE("Recorder owns submitted packets and drains on destruction",
          "[recorder]") {
  const OutputDirectory output;
  const auto path = output.File("owned.mp4");
  const Media expected;
  {
    Recorder recorder;
    REQUIRE(recorder.Start(expected.streams, RecorderConfig{path}));
    // Release every caller-owned packet before the recorder is destroyed.
    Media submitted;
    for (auto& packet : submitted.packets) {
      REQUIRE(recorder.TrySubmitPacket(packet));
      packet.Unref();
    }
  }
  CheckRecording(path, expected);
}

TEST_CASE("Recorder accepts concurrent audio and video producers",
          "[recorder]") {
  const Media media(true, true);
  const OutputDirectory output;
  const auto path = output.File("concurrent.mp4");
  Recorder recorder;
  REQUIRE(recorder.Start(media.streams, RecorderConfig{path}));
  std::atomic<bool> accepted{true};
  const auto submit = [&](int index) {
    for (const auto& packet : media.packets) {
      if (packet->stream_index == index && !recorder.TrySubmitPacket(packet))
        accepted = false;
    }
  };
  std::thread video([&] { submit(4); });
  std::thread audio([&] { submit(7); });
  video.join();
  audio.join();
  recorder.Stop();
  REQUIRE(accepted.load());
  CHECK(recorder.status().written_packets == media.packets.size());
  CHECK(recorder.status().dropped_packets == 0);
  CheckRecording(path, media);
}

TEST_CASE("Recorder rejects invalid packets without failing its session",
          "[recorder]") {
  const Media media;
  const OutputDirectory output;
  Recorder recorder;
  REQUIRE(
      recorder.Start(media.streams, RecorderConfig{output.File("valid.mp4")}));
  CHECK_THROWS_AS(recorder.TrySubmitPacket(ffmpeg::Packet{}),
                  std::invalid_argument);
  auto unknown = media.packets.front().Clone();
  unknown->stream_index = 100;
  CHECK_THROWS_AS(recorder.TrySubmitPacket(unknown), std::invalid_argument);
  auto untimed = media.packets.front().Clone();
  untimed->pts = AV_NOPTS_VALUE;
  untimed->dts = AV_NOPTS_VALUE;
  CHECK_THROWS_AS(recorder.TrySubmitPacket(untimed), std::invalid_argument);
  CHECK(recorder.status().state == RecorderState::kRunning);
  for (const auto& packet : media.packets) {
    REQUIRE(recorder.TrySubmitPacket(packet));
  }
  recorder.Stop();
  CHECK(recorder.status().dropped_packets == 0);
  CheckRecording(output.File("valid.mp4"), media);
}

TEST_CASE("Recorder bounds its packet queue and can restart", "[recorder]") {
  const Media media;
  const OutputDirectory output;
  Recorder recorder;
  int errors = 0;
  recorder.SetOnError([&](std::exception_ptr) { ++errors; });
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("bounded.mp4"), 1}));
  for (const auto& packet : media.packets) {
    REQUIRE(packet->size > 1);
    CHECK_FALSE(recorder.TrySubmitPacket(packet));
  }
  CHECK(recorder.status().queue_bytes == 0);
  CHECK(recorder.status().dropped_packets == media.packets.size());
  recorder.Stop();
  CHECK(recorder.status().written_packets == 0);
  CHECK(errors == 0);
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("restart.mp4")}));
  CHECK(recorder.status().dropped_packets == 0);
  for (const auto& packet : media.packets) {
    REQUIRE(recorder.TrySubmitPacket(packet));
  }
  recorder.Stop();
  CHECK(errors == 0);
  CheckRecording(output.File("restart.mp4"), media);
}

TEST_CASE("Recorder notifies once per failed session outside locks",
          "[recorder]") {
  const Media media;
  const OutputDirectory output;
  Recorder recorder;
  int notifications = 0;
  std::exception_ptr failure;
  mw::streamer::RecorderStatus observed;
  recorder.SetOnError([&](std::exception_ptr error) {
    ++notifications;
    failure = error;
    observed = recorder.status();
    // Callback configuration is safe from within a notification.
    recorder.SetOnError({});
  });
  CHECK_THROWS_AS(
      recorder.Start(media.streams, RecorderConfig{output.File("bad.m3u8")}),
      std::invalid_argument);
  CHECK(notifications == 0);
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("missing/failed.mp4")}));
  recorder.Stop();
  CHECK(notifications == 1);
  CHECK(observed.state == RecorderState::kFailed);
  CHECK(observed.queue_bytes == 0);
  CHECK_FALSE(observed.error.empty());
  REQUIRE(failure != nullptr);
  try {
    std::rethrow_exception(failure);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR(ENOENT));
    CHECK(observed.error == error.what());
  }
  recorder.Stop();
  CHECK(notifications == 1);
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("missing/again.mp4")}));
  recorder.Stop();
  CHECK(recorder.status().state == RecorderState::kFailed);
  CHECK(notifications == 1);
}

TEST_CASE("Recorder contains observer exceptions and retains registration",
          "[recorder]") {
  bool unknown = false;
  SECTION("standard exception") {}
  SECTION("unknown exception") { unknown = true; }
  const Media media;
  const OutputDirectory output;
  Recorder recorder;
  int notifications = 0;
  recorder.SetOnError([&](std::exception_ptr) {
    ++notifications;
    if (unknown) throw 42;
    throw std::runtime_error("observer failure");
  });
  for (int session = 0; session < 2; ++session) {
    REQUIRE(recorder.Start(media.streams,
                           RecorderConfig{output.File("missing/failed.mp4")}));
    recorder.Stop();
    CHECK(recorder.status().state == RecorderState::kFailed);
    CHECK(recorder.status().error.find("observer failure") ==
          std::string::npos);
    CHECK(notifications == session + 1);
  }
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("recovered.mp4")}));
  for (const auto& packet : media.packets) {
    REQUIRE(recorder.TrySubmitPacket(packet));
  }
  recorder.Stop();
  CHECK(notifications == 2);
  CHECK(recorder.status().state == RecorderState::kStopped);
  CheckRecording(output.File("recovered.mp4"), media);
}

TEST_CASE("Recorder reports asynchronous file failure and recovers on restart",
          "[recorder]") {
  const Media media;
  const OutputDirectory output;
  Recorder recorder;
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("missing/failed.mp4")}));
  REQUIRE(WaitForState(recorder, RecorderState::kFailed));
  CHECK_FALSE(recorder.status().error.empty());
  CHECK(recorder.status().queue_bytes == 0);
  CHECK_FALSE(recorder.TrySubmitPacket(media.packets.front()));
  CHECK_FALSE(recorder.Start(media.streams,
                             RecorderConfig{output.File("before_stop.mp4")}));
  recorder.Stop();
  CHECK(recorder.status().state == RecorderState::kFailed);
  CHECK_FALSE(recorder.status().error.empty());
  REQUIRE(recorder.Start(media.streams,
                         RecorderConfig{output.File("recovered.mp4")}));
  for (const auto& packet : media.packets) {
    REQUIRE(recorder.TrySubmitPacket(packet));
  }
  recorder.Stop();
  CHECK(recorder.status().state == RecorderState::kStopped);
  CHECK(recorder.status().error.empty());
  CheckRecording(output.File("recovered.mp4"), media);
}

#if defined(__linux__)
TEST_CASE("Recorder exposes write failures without a callback", "[recorder]") {
  const Media media;
  const OutputDirectory output;
  const auto path = output.File("full.mp4");
  fs::create_symlink("/dev/full", fs::u8path(path));
  Recorder recorder;
  REQUIRE(recorder.Start(media.streams, RecorderConfig{path}));
  for (const auto& packet : media.packets) {
    recorder.TrySubmitPacket(packet);
  }
  recorder.Stop();
  CHECK(recorder.status().state == RecorderState::kFailed);
  CHECK_FALSE(recorder.status().error.empty());
  CHECK(recorder.status().queue_bytes == 0);
}
#endif
