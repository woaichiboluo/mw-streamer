#include "mw/streamer/input/av_frame_source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <fmt/format.h>

#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoder;
using mw::streamer::AvDecoderConfig;
using mw::streamer::AvEncoderConfig;
using mw::streamer::AvFrameSource;
using mw::streamer::AvFrameSourceReadResult;
using mw::streamer::AvFrameSourceReadStatus;
namespace ffmpeg = mw::streamer::ffmpeg;

struct FrameRecord {
  std::int64_t pts = AV_NOPTS_VALUE;
  std::int64_t duration = 0;
  int format = -1;
  int width = 0;
  int height = 0;
  int samples = 0;
  int sample_rate = 0;
  std::vector<AVChannel> channel_layout;
  AVRational aspect{0, 1};
  AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
  AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
  AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
  AVChromaLocation chroma_location = AVCHROMA_LOC_UNSPECIFIED;
  int flags = 0;
  std::vector<std::uint8_t> bytes;

  bool operator==(const FrameRecord& other) const {
    return pts == other.pts && duration == other.duration &&
           format == other.format && width == other.width &&
           height == other.height && samples == other.samples &&
           sample_rate == other.sample_rate &&
           channel_layout == other.channel_layout &&
           aspect.num == other.aspect.num && aspect.den == other.aspect.den &&
           colorspace == other.colorspace && color_range == other.color_range &&
           primaries == other.primaries && transfer == other.transfer &&
           chroma_location == other.chroma_location && flags == other.flags &&
           bytes == other.bytes;
  }
};

FrameRecord Record(const AVFrame& frame) {
  FrameRecord result;
  result.pts = frame.pts;
  result.duration = frame.duration;
  result.format = frame.format;
  result.width = frame.width;
  result.height = frame.height;
  result.samples = frame.nb_samples;
  result.sample_rate = frame.sample_rate;
  result.aspect = frame.sample_aspect_ratio;
  result.colorspace = frame.colorspace;
  result.color_range = frame.color_range;
  result.primaries = frame.color_primaries;
  result.transfer = frame.color_trc;
  result.chroma_location = frame.chroma_location;
  result.flags = frame.flags;
  if (frame.nb_samples > 0) {
    for (int channel = 0; channel < frame.ch_layout.nb_channels; ++channel) {
      result.channel_layout.push_back(av_channel_layout_channel_from_index(
          &frame.ch_layout, static_cast<unsigned int>(channel)));
    }
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int planes = planar ? frame.ch_layout.nb_channels : 1;
    const int bytes = frame.nb_samples * av_get_bytes_per_sample(format) *
                      (planar ? 1 : frame.ch_layout.nb_channels);
    for (int plane = 0; plane < planes; ++plane) {
      result.bytes.insert(result.bytes.end(), frame.extended_data[plane],
                          frame.extended_data[plane] + bytes);
    }
  } else {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const int size =
        av_image_get_buffer_size(format, frame.width, frame.height, 1);
    REQUIRE(size > 0);
    result.bytes.resize(static_cast<std::size_t>(size));
    REQUIRE(av_image_copy_to_buffer(result.bytes.data(), size, frame.data,
                                    frame.linesize, format, frame.width,
                                    frame.height, 1) == size);
  }
  return result;
}

void CheckRecord(const FrameRecord& actual, const FrameRecord& expected,
                 int flag_mask = ~0) {
  CHECK(actual.pts == expected.pts);
  CHECK(actual.duration == expected.duration);
  CHECK(actual.format == expected.format);
  CHECK(actual.width == expected.width);
  CHECK(actual.height == expected.height);
  CHECK(actual.samples == expected.samples);
  CHECK(actual.sample_rate == expected.sample_rate);
  CHECK(actual.channel_layout == expected.channel_layout);
  CHECK(actual.aspect.num == expected.aspect.num);
  CHECK(actual.aspect.den == expected.aspect.den);
  CHECK(actual.colorspace == expected.colorspace);
  CHECK(actual.color_range == expected.color_range);
  CHECK(actual.primaries == expected.primaries);
  CHECK(actual.transfer == expected.transfer);
  CHECK(actual.chroma_location == expected.chroma_location);
  const int actual_flags = actual.flags & flag_mask;
  const int expected_flags = expected.flags & flag_mask;
  CHECK(actual_flags == expected_flags);
  REQUIRE(actual.bytes.size() == expected.bytes.size());
  if (!actual.bytes.empty()) {
    CHECK(std::memcmp(actual.bytes.data(), expected.bytes.data(),
                      actual.bytes.size()) == 0);
  }
}

struct GraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

// Build and drive native filters directly; no converter internals are used.
class ReferenceGraph final {
 public:
  ReferenceGraph(const ffmpeg::StreamInfo& stream, const ffmpeg::Frame& first,
                 const AvEncoderConfig& config, std::int64_t start_us)
      : video_(stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO),
        graph_(avfilter_graph_alloc()) {
    REQUIRE(graph_ != nullptr);
    graph_->nb_threads = 1;
    source_ = avfilter_graph_alloc_filter(
        graph_.get(), avfilter_get_by_name(video_ ? "buffer" : "abuffer"),
        "in");
    REQUIRE(source_ != nullptr);
    const std::unique_ptr<AVBufferSrcParameters, decltype(&av_free)> parameters(
        av_buffersrc_parameters_alloc(), &av_free);
    REQUIRE(parameters != nullptr);
    parameters->format = first->format;
    parameters->time_base = stream.time_base;
    parameters->width = first->width;
    parameters->height = first->height;
    parameters->sample_aspect_ratio = first->sample_aspect_ratio;
    parameters->frame_rate = stream.frame_rate;
    parameters->color_space = first->colorspace;
    parameters->color_range = first->color_range;
    parameters->hw_frames_ctx = first->hw_frames_ctx;
    parameters->sample_rate = first->sample_rate;
    parameters->ch_layout = first->ch_layout;
    REQUIRE(av_buffersrc_parameters_set(source_, parameters.get()) >= 0);
    REQUIRE(avfilter_init_str(source_, nullptr) >= 0);
    REQUIRE(avfilter_graph_create_filter(
                &sink_,
                avfilter_get_by_name(video_ ? "buffersink" : "abuffersink"),
                "out", nullptr, nullptr, graph_.get()) >= 0);
    std::string specification;
    if (video_) {
      specification =
          fmt::format("fps=fps={}/{}", config.fps.num, config.fps.den);
      if (start_us != AV_NOPTS_VALUE) {
        specification += fmt::format(
            ":start_time={:.6f}", static_cast<double>(start_us) / AV_TIME_BASE);
      }
    } else {
      specification = fmt::format("aresample=async=1000:min_comp=0.01:osr={}",
                                  config.audio_sample_rate);
      if (start_us != AV_NOPTS_VALUE) {
        specification += fmt::format(
            ":first_pts={}", av_rescale_q(start_us, AVRational{1, AV_TIME_BASE},
                                          AVRational{1, first->sample_rate}));
      }
      char layout[128];
      REQUIRE(av_channel_layout_describe(&config.audio_channel_layout, layout,
                                         sizeof(layout)) >= 0);
      specification += fmt::format(
          ",asetnsamples=n=1024:p=0,aformat=sample_fmts={}:sample_rates={}:"
          "channel_layouts={}",
          av_get_sample_fmt_name(config.audio_sample_format),
          config.audio_sample_rate, layout);
    }
    AVFilterContext* tail = source_;
    std::vector<std::string> filters;
    std::size_t begin = 0;
    for (;;) {
      const auto end = specification.find(',', begin);
      filters.push_back(specification.substr(begin, end - begin));
      if (end == std::string::npos) break;
      begin = end + 1;
    }
    for (std::size_t index = 0; index < filters.size(); ++index) {
      const auto separator = filters[index].find('=');
      const auto name = filters[index].substr(0, separator);
      const auto options = filters[index].substr(separator + 1);
      AVFilterContext* filter = nullptr;
      REQUIRE(avfilter_graph_create_filter(
                  &filter, avfilter_get_by_name(name.c_str()),
                  fmt::format("f{}", index).c_str(), options.c_str(), nullptr,
                  graph_.get()) >= 0);
      REQUIRE(avfilter_link(tail, 0, filter, 0) >= 0);
      tail = filter;
    }
    REQUIRE(avfilter_link(tail, 0, sink_, 0) >= 0);
    REQUIRE(avfilter_graph_config(graph_.get(), nullptr) >= 0);
  }

  void Push(const ffmpeg::Frame& frame) {
    REQUIRE(av_buffersrc_write_frame(source_, frame.get()) >= 0);
  }
  void Eof(std::int64_t end_pts) {
    REQUIRE(av_buffersrc_close(source_, end_pts, 0) >= 0);
  }
  int Read(ffmpeg::Frame& frame, int samples = 0) {
    return video_ ? av_buffersink_get_frame(sink_, frame.get())
                  : av_buffersink_get_samples(sink_, frame.get(),
                                              samples == 0 ? 1024 : samples);
  }
  AVRational time_base() const { return av_buffersink_get_time_base(sink_); }

 private:
  bool video_;
  std::unique_ptr<AVFilterGraph, GraphDeleter> graph_;
  AVFilterContext* source_ = nullptr;
  AVFilterContext* sink_ = nullptr;
};

struct FormatDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct CodecDeleter {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

struct Track {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Packet> packets;
};

Track ReadTrack(std::string_view name, AVMediaType type = AVMEDIA_TYPE_VIDEO) {
  const auto path =
      fmt::format("{}/{}", MW_AV_FRAME_SOURCE_TEST_DATA_DIR, name);
  AVFormatContext* context = nullptr;
  const int code =
      avformat_open_input(&context, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE(code >= 0);
  REQUIRE(avformat_find_stream_info(context, nullptr) >= 0);
  const int index = av_find_best_stream(context, type, -1, -1, nullptr, 0);
  REQUIRE(index >= 0);
  const auto* stream = context->streams[index];
  Track track;
  track.stream = {index, ffmpeg::CodecParameters(*stream->codecpar),
                  stream->time_base};
  track.stream.frame_rate =
      av_guess_frame_rate(context, context->streams[index], nullptr);
  track.stream.sample_aspect_ratio =
      av_guess_sample_aspect_ratio(context, context->streams[index], nullptr);
  for (;;) {
    ffmpeg::Packet packet;
    const int read = av_read_frame(context, packet.get());
    if (read == AVERROR_EOF) break;
    REQUIRE(read >= 0);
    if (packet->stream_index == index)
      track.packets.push_back(std::move(packet));
  }
  REQUIRE_FALSE(track.packets.empty());
  return track;
}

// PCM packets exercise the real decoder while keeping duration/rate and mono
// sample values deterministic, independent of a lossy audio encoder.
Track PcmTrack(int total_samples = 2 * 44100) {
  Track track;
  track.stream.stream_index = 1;
  track.stream.time_base = {1, 44100};
  auto* parameters = track.stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
  parameters->format = AV_SAMPLE_FMT_S16;
  parameters->sample_rate = 44100;
  parameters->bits_per_coded_sample = 16;
  av_channel_layout_default(&parameters->ch_layout, 1);
  for (int start = 0; start < total_samples; start += 997) {
    const int samples = std::min(997, total_samples - start);
    ffmpeg::Packet packet;
    REQUIRE(av_new_packet(packet.get(), samples * 2) >= 0);
    packet->stream_index = track.stream.stream_index;
    packet->pts = start;
    packet->dts = start;
    packet->duration = samples;
    for (int sample = 0; sample < samples; ++sample) {
      const auto value = static_cast<std::uint16_t>(
          static_cast<std::int16_t>((start + sample) % 2047 - 1023));
      packet->data[sample * 2] = static_cast<std::uint8_t>(value & 255);
      packet->data[sample * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    track.packets.push_back(std::move(packet));
  }
  return track;
}

Track Prefix(const Track& source, std::size_t count) {
  Track result;
  result.stream = source.stream;
  REQUIRE(count <= source.packets.size());
  result.packets.assign(
      source.packets.begin(),
      source.packets.begin() + static_cast<std::ptrdiff_t>(count));
  return result;
}

Track Shifted(const Track& source, std::int64_t offset_us) {
  Track result;
  result.stream = source.stream;
  const auto offset = av_rescale_q(offset_us, AVRational{1, AV_TIME_BASE},
                                   source.stream.time_base);
  for (const auto& original : source.packets) {
    auto packet = original.Ref();
    if (packet->pts != AV_NOPTS_VALUE) packet->pts += offset;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts += offset;
    result.packets.push_back(std::move(packet));
  }
  return result;
}

struct DecodedReference {
  std::vector<ffmpeg::Frame> frames;
  std::int64_t end_pts = AV_NOPTS_VALUE;
};

AVPixelFormat CudaFormat(AVCodecContext*, const AVPixelFormat* formats) {
  for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    if (*format == AV_PIX_FMT_CUDA) return *format;
  }
  return AV_PIX_FMT_NONE;
}

DecodedReference DecodeReference(
    const Track& track, const ffmpeg::HwDeviceContext* device = nullptr,
    std::string_view decoder_name = {}) {
  const auto id = track.stream.codec_parameters.get()->codec_id;
  const AVCodec* codec = nullptr;
  if (!decoder_name.empty()) {
    const std::string name(decoder_name);
    codec = avcodec_find_decoder_by_name(name.c_str());
  } else if (!device && id == AV_CODEC_ID_VP8) {
    codec = avcodec_find_decoder_by_name("libvpx");
  } else if (!device && id == AV_CODEC_ID_VP9) {
    codec = avcodec_find_decoder_by_name("libvpx-vp9");
  }
  if (!codec) codec = avcodec_find_decoder(id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecDeleter> context(
      avcodec_alloc_context3(codec));
  REQUIRE(context != nullptr);
  REQUIRE(avcodec_parameters_to_context(
              context.get(), track.stream.codec_parameters.get()) >= 0);
  context->pkt_timebase = track.stream.time_base;
  context->framerate = track.stream.frame_rate;
  context->thread_count = device ? 2 : 1;
  if (device) {
    context->hw_device_ctx = av_buffer_ref(device->get());
    REQUIRE(context->hw_device_ctx != nullptr);
    context->get_format = CudaFormat;
  }
  REQUIRE(avcodec_open2(context.get(), codec, nullptr) >= 0);
  DecodedReference decoded;
  const auto receive = [&] {
    for (;;) {
      ffmpeg::Frame frame;
      const int code = avcodec_receive_frame(context.get(), frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE(code >= 0);
      if (device) REQUIRE(frame->format == AV_PIX_FMT_CUDA);
      frame->pts = frame->best_effort_timestamp;
      frame->time_base = track.stream.time_base;
      auto duration = frame->duration;
      if (duration <= 0 && frame->nb_samples > 0) {
        duration =
            av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate},
                         track.stream.time_base);
      } else if (duration <= 0 && track.stream.frame_rate.num > 0) {
        duration = av_rescale_q(1, av_inv_q(track.stream.frame_rate),
                                track.stream.time_base);
      }
      decoded.end_pts = frame->pts == AV_NOPTS_VALUE || duration <= 0
                            ? AV_NOPTS_VALUE
                            : frame->pts + duration;
      decoded.frames.push_back(std::move(frame));
    }
  };
  for (const auto& packet : track.packets) {
    REQUIRE(avcodec_send_packet(context.get(), packet.get()) >= 0);
    REQUIRE(receive() == AVERROR(EAGAIN));
  }
  REQUIRE(avcodec_send_packet(context.get(), nullptr) >= 0);
  REQUIRE(receive() == AVERROR_EOF);
  return decoded;
}

int Cadence(const AvEncoderConfig& config, std::size_t index) {
  if (config.audio_sample_rate == 48000 &&
      av_cmp_q(config.fps, {30000, 1001}) == 0) {
    constexpr std::array<int, 5> kSamples{1602, 1601, 1602, 1601, 1602};
    return kSamples[index % kSamples.size()];
  }
  if (config.audio_sample_rate == 48000 &&
      av_cmp_q(config.fps, {60000, 1001}) == 0) {
    constexpr std::array<int, 5> kSamples{801, 801, 801, 800, 801};
    return kSamples[index % kSamples.size()];
  }
  const AVRational period{config.fps.den, config.fps.num};
  const AVRational samples{1, config.audio_sample_rate};
  const auto before =
      av_rescale_q(static_cast<std::int64_t>(index), period, samples);
  const auto after =
      av_rescale_q(static_cast<std::int64_t>(index + 1), period, samples);
  return static_cast<int>(after - before);
}

struct FilteredReference {
  std::vector<FrameRecord> frames;
  AVRational time_base{0, 1};
};

FrameRecord DownloadVideo(const ffmpeg::Frame& gpu);

FilteredReference FilterReference(
    const Track& track, const AvEncoderConfig& config,
    std::int64_t start_us = 0, const ffmpeg::HwDeviceContext* device = nullptr,
    std::string_view decoder_name = {}) {
  const auto decoded = DecodeReference(track, device, decoder_name);
  FilteredReference filtered;
  if (decoded.frames.empty()) return filtered;
  ReferenceGraph graph(track.stream, decoded.frames.front(), config, start_us);
  for (const auto& frame : decoded.frames) graph.Push(frame);
  graph.Eof(decoded.end_pts);
  filtered.time_base = graph.time_base();
  for (std::size_t index = 0;; ++index) {
    ffmpeg::Frame frame;
    const int code = graph.Read(frame, Cadence(config, index));
    if (code == AVERROR_EOF) break;
    REQUIRE(code >= 0);
    filtered.frames.push_back(frame->format == AV_PIX_FMT_CUDA
                                  ? DownloadVideo(frame)
                                  : Record(*frame.get()));
  }
  return filtered;
}

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

AvFrameSourceReadResult ReadAvailable(AvFrameSource& source) {
  AvFrameSourceReadResult result;
  REQUIRE(WaitFor([&] {
    result = source.TryReadFrame();
    return result.status != AvFrameSourceReadStatus::kNull || source.error();
  }));
  REQUIRE(source.error() == nullptr);
  return result;
}

void Push(AvDecoder& decoder, const Track& track, std::uint64_t generation,
          bool eof = true, std::size_t begin = 0) {
  for (std::size_t index = begin; index < track.packets.size(); ++index) {
    REQUIRE(decoder.PushPacket(track.packets[index], generation));
  }
  if (eof) REQUIRE(decoder.PushEof(generation));
}

void CheckGeneration(AvFrameSource& source, const FilteredReference& video,
                     const FilteredReference& audio, std::uint64_t generation) {
  const auto count = std::max(video.frames.size(), audio.frames.size());
  for (std::size_t index = 0; index < count; ++index) {
    const auto result = ReadAvailable(source);
    REQUIRE(result.status == AvFrameSourceReadStatus::kFrame);
    CHECK(result.generation == generation);
    CHECK(result.frame_index == index);
    CHECK(result.video.has_value() == !video.frames.empty());
    CHECK(result.audio.has_value() == !audio.frames.empty());
    if (!video.frames.empty()) {
      const auto& expected =
          video.frames[std::min(index, video.frames.size() - 1)];
      REQUIRE(result.video.has_value());
      CheckRecord(Record(*result.video->get()), expected);
      CHECK(result.video->get()->time_base.num == video.time_base.num);
      CHECK(result.video->get()->time_base.den == video.time_base.den);
    }
    if (!audio.frames.empty()) {
      const auto& expected =
          audio.frames[std::min(index, audio.frames.size() - 1)];
      REQUIRE(result.audio.has_value());
      CheckRecord(Record(*result.audio->get()), expected);
      CHECK(result.audio->get()->format == AV_SAMPLE_FMT_FLTP);
      CHECK(result.audio->get()->sample_rate == 48000);
      CHECK(result.audio->get()->ch_layout.nb_channels == 2);
    }
    const bool new_audio = index < audio.frames.size();
    const auto& timing = new_audio ? audio.frames[index] : video.frames[index];
    const auto time_base = new_audio ? audio.time_base : video.time_base;
    CHECK(result.pts_us ==
          av_rescale_q(timing.pts, time_base, AVRational{1, AV_TIME_BASE}));
    const auto duration = new_audio ? timing.samples : timing.duration;
    CHECK(result.duration_us ==
          av_rescale_q(duration, time_base, AVRational{1, AV_TIME_BASE}));
  }
  const auto eof = ReadAvailable(source);
  CHECK(eof.status == AvFrameSourceReadStatus::kEof);
  CHECK(eof.generation == generation);
  CHECK(eof.frame_index == count);
  CHECK_FALSE(eof.video.has_value());
  CHECK_FALSE(eof.audio.has_value());
}

AvDecoderConfig SingleThreadConfig() {
  AvDecoderConfig config;
  config.thread_count = 1;
  return config;
}

TEST_CASE("AvFrameSource starts every generation at received content",
          "[av-frame-source][origin][generation]") {
  std::int64_t next_origin = 105000000;
  SECTION("loop repeats the same content") {}
  SECTION("live reconnection advances timestamps") { next_origin = 106000000; }
  SECTION("network file reconnection restarts timestamps") { next_origin = 0; }
  const auto original = PcmTrack(44100);
  AvEncoderConfig config;
  AvDecoder audio(original.stream, SingleThreadConfig());
  AvFrameSource source(nullptr, &audio, config);
  for (std::uint64_t generation = 0; generation < 2; ++generation) {
    const auto origin = generation == 0 ? 105000000 : next_origin;
    const auto track = Shifted(original, origin);
    const auto reference = FilterReference(track, config, origin);
    REQUIRE(reference.frames.size() == 30);
    Push(audio, track, generation);
    for (std::size_t index = 0; index < reference.frames.size(); ++index) {
      const auto result = ReadAvailable(source);
      REQUIRE(result.status == AvFrameSourceReadStatus::kFrame);
      REQUIRE(result.pts_us >= origin);
      CHECK(result.generation == generation);
      CHECK(result.frame_index == index);
      REQUIRE(result.audio.has_value());
      CheckRecord(Record(*result.audio->get()), reference.frames[index]);
    }
    const auto eof = ReadAvailable(source);
    CHECK(eof.status == AvFrameSourceReadStatus::kEof);
    CHECK(eof.generation == generation);
    CHECK(eof.frame_index == 30);
    CHECK(eof.pts_us == origin + 1000000);
  }
}

TEST_CASE("AvFrameSource waits for both tracks before choosing a shared origin",
          "[av-frame-source][origin][audio-video]") {
  bool video_first = true;
  bool audio_earlier = true;
  SECTION("later video arrives before earlier audio") {}
  SECTION("earlier video arrives before later audio") { audio_earlier = false; }
  SECTION("earlier audio arrives before later video") { video_first = false; }
  SECTION("later audio arrives before earlier video") {
    video_first = false;
    audio_earlier = false;
  }
  const auto video_track =
      Shifted(ReadTrack("h264_aac.mp4"), audio_earlier ? 105200000 : 105000000);
  const auto audio_track =
      Shifted(PcmTrack(), audio_earlier ? 105000000 : 105200000);
  AvEncoderConfig config;
  const auto video_reference = FilterReference(video_track, config, 105000000);
  const auto audio_reference = FilterReference(audio_track, config, 105000000);
  AvDecoder video(video_track.stream, SingleThreadConfig());
  AvDecoder audio(audio_track.stream, SingleThreadConfig());
  AvFrameSource source(&video, &audio, config);
  auto& first = video_first ? video : audio;
  Push(first, video_first ? video_track : audio_track, 0);
  // At least two decoded frames prove the first track is ready; the other
  // track has not received any packets and must still gate initialization.
  REQUIRE(WaitFor([&] { return first.buffered_frames() >= 2; }));
  CHECK(source.buffered_frames() == 0);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  Push(video_first ? audio : video, video_first ? audio_track : video_track, 0);
  CheckGeneration(source, video_reference, audio_reference, 0);
}

TEST_CASE("AvFrameSource video-only generations start at received content",
          "[av-frame-source][origin][single-track][generation]") {
  std::int64_t next_origin = 105000000;
  SECTION("loop repeats the same content") {}
  SECTION("live reconnection advances timestamps") { next_origin = 106000000; }
  SECTION("network file reconnection restarts timestamps") { next_origin = 0; }
  const auto original = ReadTrack("h264_aac.mp4");
  AvEncoderConfig config;
  AvDecoder video(original.stream, SingleThreadConfig());
  AvFrameSource source(&video, nullptr, config);
  for (std::uint64_t generation = 0; generation < 2; ++generation) {
    const auto origin = generation == 0 ? 105000000 : next_origin;
    const auto track = Shifted(original, origin);
    const auto reference = FilterReference(track, config, origin);
    REQUIRE_FALSE(reference.frames.empty());
    Push(video, track, generation);
    CheckGeneration(source, reference, {}, generation);
  }
}

TEST_CASE("AvFrameSource empty track EOF releases shared origin initialization",
          "[av-frame-source][origin][empty][eof]") {
  bool empty_video = true;
  SECTION("video has no decoded frames") {}
  SECTION("audio has no decoded frames") { empty_video = false; }
  const auto video_track = Shifted(ReadTrack("h264_aac.mp4"), 105000000);
  const auto audio_track = Shifted(PcmTrack(44100), 105000000);
  AvEncoderConfig config;
  const auto video_reference =
      empty_video ? FilteredReference{}
                  : FilterReference(video_track, config, 105000000);
  const auto audio_reference =
      empty_video ? FilterReference(audio_track, config, 105000000)
                  : FilteredReference{};
  AvDecoder video(video_track.stream, SingleThreadConfig());
  AvDecoder audio(audio_track.stream, SingleThreadConfig());
  AvFrameSource source(&video, &audio, config);
  auto& present = empty_video ? audio : video;
  Push(present, empty_video ? audio_track : video_track, 0);
  REQUIRE(WaitFor([&] { return present.buffered_frames() >= 2; }));
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  REQUIRE((empty_video ? video : audio).PushEof(0));
  CheckGeneration(source, video_reference, audio_reference, 0);
}

TEST_CASE(
    "AvFrameSource bundles native FPS video and resampled audio into cached "
    "periods",
    "[av-frame-source][audio-video][oracle]") {
  const auto video_track = ReadTrack("h264_aac.mp4");
  const auto audio_track = PcmTrack();
  AvEncoderConfig config;
  const auto video_reference = FilterReference(video_track, config);
  const auto audio_reference = FilterReference(audio_track, config);
  AvDecoder video(video_track.stream, SingleThreadConfig());
  AvDecoder audio(audio_track.stream, SingleThreadConfig());
  AvFrameSource source(&video, &audio, config);
  Push(video, video_track, 0);
  Push(audio, audio_track, 0);
  CheckGeneration(source, video_reference, audio_reference, 0);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
}

TEST_CASE(
    "AvFrameSource supports individual tracks and exact fractional sample "
    "cadences",
    "[av-frame-source][single-track][cadence]") {
  bool audio_only = false;
  AvEncoderConfig config;
  SECTION("video only") {}
  SECTION("audio only at 30 fps") { audio_only = true; }
  SECTION("29.97 fps NTSC audio") {
    audio_only = true;
    config.fps = {30000, 1001};
  }
  SECTION("59.94 fps NTSC audio") {
    audio_only = true;
    config.fps = {60000, 1001};
  }
  SECTION("arbitrary rational audio cadence") {
    audio_only = true;
    config.fps = {24000, 1001};
  }
  const auto track = audio_only ? PcmTrack() : ReadTrack("h264_aac.mp4");
  const auto reference = FilterReference(track, config);
  AvDecoder decoder(track.stream, SingleThreadConfig());
  AvFrameSource source(audio_only ? nullptr : &decoder,
                       audio_only ? &decoder : nullptr, config);
  CHECK(source.frame_capacity() ==
        static_cast<std::size_t>(
            std::max(1, (config.fps.num / config.fps.den) / 4)));
  Push(decoder, track, 7);
  CheckGeneration(source, audio_only ? FilteredReference{} : reference,
                  audio_only ? reference : FilteredReference{}, 7);
}

TEST_CASE(
    "AvFrameSource repeats an ended track's last frame or short audio tail",
    "[av-frame-source][eof][repeat]") {
  bool short_audio = true;
  SECTION("short audio continues with its last unpadded block") {}
  SECTION("short video continues with its last picture") {
    short_audio = false;
  }
  const auto full_video = ReadTrack("h264_aac.mp4");
  const auto video_track = short_audio ? full_video : Prefix(full_video, 2);
  const auto audio_track = short_audio ? PcmTrack(5 * 997) : PcmTrack();
  AvEncoderConfig config;
  const auto video_reference = FilterReference(video_track, config);
  const auto audio_reference = FilterReference(audio_track, config);
  if (short_audio) {
    REQUIRE(audio_reference.frames.size() < video_reference.frames.size());
    REQUIRE(audio_reference.frames.back().samples < 1600);
  } else {
    REQUIRE(video_reference.frames.size() < audio_reference.frames.size());
  }
  AvDecoder video(video_track.stream, SingleThreadConfig());
  AvDecoder audio(audio_track.stream, SingleThreadConfig());
  AvFrameSource source(&video, &audio, config);
  Push(video, video_track, 0);
  Push(audio, audio_track, 0);
  CheckGeneration(source, video_reference, audio_reference, 0);
}

TEST_CASE(
    "AvFrameSource prebuffers four periods and releases a short EOF segment",
    "[av-frame-source][prefill][eof]") {
  const auto full = ReadTrack("vp8_video.ivf");
  const auto track = Prefix(full, 2);
  AvEncoderConfig config;
  const auto reference = FilterReference(track, config);
  REQUIRE(reference.frames.size() == 6);
  AvDecoder decoder(track.stream, SingleThreadConfig());
  AvFrameSource source(&decoder, nullptr, config);
  Push(decoder, track, 0, false);
  REQUIRE(WaitFor([&] { return source.buffered_frames() == 3; }));
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  REQUIRE(decoder.PushEof(0));
  CheckGeneration(source, reference, {}, 0);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
}

TEST_CASE(
    "AvFrameSource output capacity backpressures its worker and Stop keeps "
    "borrowed decoder alive",
    "[av-frame-source][backpressure][stop][ownership]") {
  const auto track = ReadTrack("h264_aac.mp4");
  AvDecoder decoder(track.stream, SingleThreadConfig());
  AvFrameSource source(&decoder, nullptr, {});
  Push(decoder, track, 0);
  REQUIRE(WaitFor(
      [&] { return source.buffered_frames() == source.frame_capacity(); }));
  std::this_thread::sleep_for(20ms);
  CHECK(source.buffered_frames() == source.frame_capacity());
  const auto retained = ReadAvailable(source);
  REQUIRE(retained.video.has_value());
  const auto before = Record(*retained.video->get());
  const auto started = std::chrono::steady_clock::now();
  source.Stop();
  CHECK(std::chrono::steady_clock::now() - started < 500ms);
  CHECK(source.buffered_frames() == 0);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  REQUIRE(decoder.PushPacket(track.packets.front(), 1));
  CHECK(Record(*retained.video->get()) == before);
}

TEST_CASE(
    "AvFrameSource normal EOF preserves FIFO bundles across two generations",
    "[av-frame-source][generation][eof]") {
  const auto video_track = ReadTrack("h264_aac.mp4");
  const auto audio_track = PcmTrack();
  const auto next_video = Shifted(video_track, 10000000);
  const auto next_audio = Shifted(audio_track, 10000000);
  AvEncoderConfig config;
  config.fps = {30000, 1001};
  const auto video_reference = FilterReference(video_track, config);
  const auto audio_reference = FilterReference(audio_track, config);
  const auto next_video_reference =
      FilterReference(next_video, config, 10000000);
  const auto next_audio_reference =
      FilterReference(next_audio, config, 10000000);
  AvDecoder video(video_track.stream, SingleThreadConfig());
  AvDecoder audio(audio_track.stream, SingleThreadConfig());
  AvFrameSource source(&video, &audio, config);
  Push(video, video_track, 0);
  Push(audio, audio_track, 0);
  Push(video, next_video, 1);
  Push(audio, next_audio, 1);
  CheckGeneration(source, video_reference, audio_reference, 0);
  CheckGeneration(source, next_video_reference, next_audio_reference, 1);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
}

TEST_CASE(
    "AvFrameSource discontinuity releases short old cache while another track "
    "catches up",
    "[av-frame-source][generation][discontinuity][prefill]") {
  const auto full_video = ReadTrack("vp8_video.ivf");
  const auto full_audio = PcmTrack();
  const auto old_video = Prefix(full_video, 2);
  const auto old_audio = Prefix(full_audio, 5);
  const auto next_video = Shifted(full_video, 10000000);
  const auto next_audio = Shifted(full_audio, 10000000);
  AvEncoderConfig config;
  const auto next_video_reference =
      FilterReference(next_video, config, 10000000);
  const auto next_audio_reference =
      FilterReference(next_audio, config, 10000000);
  AvDecoder video(full_video.stream, SingleThreadConfig());
  AvDecoder audio(full_audio.stream, SingleThreadConfig());
  AvFrameSource source(&video, &audio, config);
  Push(video, old_video, 0, false);
  Push(audio, old_audio, 0, false);
  REQUIRE(WaitFor([&] { return source.buffered_frames() == 3; }));
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  REQUIRE(audio.PushPacket(next_audio.packets.front(), 1));
  for (std::uint64_t index = 0; index < 3; ++index) {
    const auto result = ReadAvailable(source);
    REQUIRE(result.status == AvFrameSourceReadStatus::kFrame);
    REQUIRE(result.video.has_value());
    REQUIRE(result.audio.has_value());
    CHECK(result.generation == 0);
    CHECK(result.frame_index == index);
    CHECK(result.pts_us < 1000000);
  }
  std::this_thread::sleep_for(20ms);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  Push(video, next_video, 1);
  Push(audio, next_audio, 1, true, 1);
  CheckGeneration(source, next_video_reference, next_audio_reference, 1);
}

TEST_CASE(
    "AvFrameSource nonblocking empty reads and empty EOF leave Decoders "
    "reusable",
    "[av-frame-source][empty][nonblocking][eof]") {
  const auto track = ReadTrack("h264_aac.mp4");
  AvDecoder decoder(track.stream, SingleThreadConfig());
  AvFrameSource source(&decoder, nullptr, {});
  const auto started = std::chrono::steady_clock::now();
  for (int index = 0; index < 1000; ++index) {
    CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  }
  CHECK(std::chrono::steady_clock::now() - started < 250ms);
  REQUIRE(decoder.PushEof(0));
  const auto eof = ReadAvailable(source);
  CHECK(eof.status == AvFrameSourceReadStatus::kEof);
  CHECK(eof.frame_index == 0);
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  source.Stop();
  REQUIRE(decoder.PushPacket(track.packets.front(), 1));
}

TEST_CASE(
    "AvFrameSource validates optional track roles and target configuration",
    "[av-frame-source][validation]") {
  CHECK_THROWS_AS(AvFrameSource(nullptr, nullptr), std::invalid_argument);
  const auto video_track = ReadTrack("h264_aac.mp4");
  const auto audio_track = PcmTrack();
  AvDecoder video(video_track.stream, SingleThreadConfig());
  AvDecoder audio(audio_track.stream, SingleThreadConfig());
  CHECK_THROWS_AS(AvFrameSource(&audio, nullptr), std::invalid_argument);
  CHECK_THROWS_AS(AvFrameSource(nullptr, &video), std::invalid_argument);
  AvEncoderConfig config;
  config.fps = {0, 1};
  CHECK_THROWS_AS(AvFrameSource(&video, nullptr, config),
                  std::invalid_argument);
}

FrameRecord DownloadVideo(const ffmpeg::Frame& gpu) {
  ffmpeg::Frame downloaded;
  REQUIRE(av_hwframe_transfer_data(downloaded.get(), gpu.get(), 0) >= 0);
  REQUIRE(av_frame_copy_props(downloaded.get(), gpu.get()) >= 0);
  if (downloaded->format == AV_PIX_FMT_YUV420P)
    return Record(*downloaded.get());
  REQUIRE(downloaded->format == AV_PIX_FMT_NV12);
  ffmpeg::Frame planar;
  planar->format = AV_PIX_FMT_YUV420P;
  planar->width = downloaded->width;
  planar->height = downloaded->height;
  REQUIRE(av_frame_get_buffer(planar.get(), 0) >= 0);
  REQUIRE(av_frame_copy_props(planar.get(), downloaded.get()) >= 0);
  for (int row = 0; row < planar->height; ++row) {
    std::memcpy(planar->data[0] + row * planar->linesize[0],
                downloaded->data[0] + row * downloaded->linesize[0],
                static_cast<std::size_t>(planar->width));
  }
  for (int row = 0; row < planar->height / 2; ++row) {
    for (int column = 0; column < planar->width / 2; ++column) {
      const auto* sample =
          downloaded->data[1] + row * downloaded->linesize[1] + column * 2;
      planar->data[1][row * planar->linesize[1] + column] = sample[0];
      planar->data[2][row * planar->linesize[2] + column] = sample[1];
    }
  }
  return Record(*planar.get());
}

TEST_CASE(
    "AvFrameSource retains native and cuvid CUDA frames on one shared device",
    "[.][av-frame-source][cuda]") {
  std::vector<ffmpeg::Frame> retained;
  std::vector<FrameRecord> expected_retained;
  {
    ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
    const auto* shared_device = device.get()->data;
    for (const auto name : {"h264_aac.mp4", "h264_cuda_dpb.mp4"}) {
      CAPTURE(name);
      const auto track = ReadTrack(name);
      AvEncoderConfig config;
      const auto reference = FilterReference(track, config);
      const std::array<FilteredReference, 2> gpu_reference{
          FilterReference(track, config, 0, &device, "h264"),
          FilterReference(track, config, 0, &device, "h264_cuvid")};
      for (const auto& hardware : gpu_reference) {
        REQUIRE(hardware.frames.size() == reference.frames.size());
        for (std::size_t index = 0; index < reference.frames.size(); ++index) {
          CHECK(hardware.frames[index].pts == reference.frames[index].pts);
          CHECK(hardware.frames[index].bytes == reference.frames[index].bytes);
        }
      }
      std::array<std::unique_ptr<AvDecoder>, 2> decoders;
      std::array<std::unique_ptr<AvFrameSource>, 2> sources;
      for (std::size_t index = 0; index < decoders.size(); ++index) {
        AvDecoderConfig decoder_config;
        decoder_config.decoder_name = index == 0 ? "h264" : "h264_cuvid";
        decoder_config.thread_count = index == 0 ? 0 : 2;
        decoders[index] =
            std::make_unique<AvDecoder>(track.stream, device, decoder_config);
        sources[index] = std::make_unique<AvFrameSource>(decoders[index].get(),
                                                         nullptr, config);
        Push(*decoders[index], track, 0);
      }
      for (std::size_t index = 0; index < reference.frames.size(); ++index) {
        for (std::size_t backend = 0; backend < sources.size(); ++backend) {
          CAPTURE(index, backend);
          const auto result = ReadAvailable(*sources[backend]);
          REQUIRE(result.status == AvFrameSourceReadStatus::kFrame);
          REQUIRE(result.video.has_value());
          CHECK_FALSE(result.audio.has_value());
          CHECK(result.frame_index == index);
          const auto& video = *result.video;
          REQUIRE(video->format == AV_PIX_FMT_CUDA);
          REQUIRE(video->hw_frames_ctx != nullptr);
          const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
              video->hw_frames_ctx->data);
          REQUIRE(frames->device_ref != nullptr);
          CHECK(frames->device_ref->data == shared_device);
          const auto actual = DownloadVideo(video);
          // Independent CUVID instances produce inconsistent KEY flags on the
          // DPB fixture. All other metadata, including field order, is checked.
          CheckRecord(actual, gpu_reference[backend].frames[index],
                      backend == 1 ? ~AV_FRAME_FLAG_KEY : ~0);
          if (index == 0) {
            retained.push_back(video.Ref());
            expected_retained.push_back(actual);
          }
        }
      }
      for (std::size_t index = 0; index < sources.size(); ++index) {
        CHECK(ReadAvailable(*sources[index]).status ==
              AvFrameSourceReadStatus::kEof);
        sources[index]->Stop();
        REQUIRE(decoders[index]->PushPacket(track.packets.front(), 1));
      }
    }
  }
  REQUIRE(retained.size() == 4);
  for (std::size_t index = 0; index < retained.size(); ++index) {
    CHECK(DownloadVideo(retained[index]) == expected_retained[index]);
  }
}

TEST_CASE(
    "AvFrameSource propagates an upstream decoder exception without fake EOF",
    "[av-frame-source][error][notification]") {
  const auto track = ReadTrack("h264_aac.mp4");
  std::atomic<std::size_t> notifications{0};
  std::atomic<std::size_t> replacement_notifications{0};
  std::exception_ptr observed;
  AvDecoder decoder(track.stream, SingleThreadConfig());
  AvFrameSource source(&decoder, nullptr, {});
  source.SetOnError([&](std::exception_ptr error) {
    observed = error;
    ++notifications;
    throw std::runtime_error("observer failure");
  });
  bool replaced = false;
  bool cleared = false;
  SECTION("throwing observer retains the upstream failure") {}
  SECTION("replacement invokes only the new observer") {
    replaced = true;
    source.SetOnError([&](std::exception_ptr error) {
      observed = error;
      ++replacement_notifications;
    });
  }
  SECTION("cleared observer leaves the upstream failure queryable") {
    cleared = true;
    source.SetOnError({});
  }
  ffmpeg::Packet invalid;
  REQUIRE(av_new_packet(invalid.get(), 1) >= 0);
  invalid->data[0] = 0;
  invalid->stream_index = track.stream.stream_index;
  REQUIRE(decoder.PushPacket(invalid, 0));
  REQUIRE(WaitFor([&] {
    return cleared
               ? source.error() != nullptr
               : notifications.load() + replacement_notifications.load() != 0;
  }));
  source.Stop();
  CHECK(notifications.load() == (replaced || cleared ? 0u : 1u));
  CHECK(replacement_notifications.load() == (replaced ? 1u : 0u));
  CHECK(source.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  const auto source_error = source.error();
  CHECK(observed == (cleared ? nullptr : source_error));
  const auto decoder_error = decoder.error();
  REQUIRE(decoder_error != nullptr);
  std::string decoder_error_message;
  try {
    std::rethrow_exception(decoder_error);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR_INVALIDDATA);
    decoder_error_message = error.what();
  }
  REQUIRE_FALSE(decoder_error_message.empty());
  REQUIRE(source_error != nullptr);
  try {
    std::rethrow_exception(source_error);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR_INVALIDDATA);
    CHECK(std::string(error.what()) == decoder_error_message);
  }
}

}  // namespace
