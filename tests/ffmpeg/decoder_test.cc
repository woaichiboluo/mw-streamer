#include "mw/streamer/ffmpeg/decoder.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;

std::unique_ptr<ffmpeg::Decoder> CreateDecoder(const ffmpeg::StreamInfo& stream,
                                               std::string_view name = {}) {
  if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO) {
    return std::make_unique<ffmpeg::AudioDecoder>(stream, name);
  }
  return std::make_unique<ffmpeg::VideoDecoder>(stream, name);
}

class Fixture final {
 public:
  Fixture(std::string_view file, AVMediaType type) {
    AVFormatContext* input = nullptr;
    const auto path =
        std::string(MW_STREAMER_FFMPEG_TEST_DATA_DIR) + "/" + std::string(file);
    ffmpeg::FfmpegException::throwIfError(
        avformat_open_input(&input, path.c_str(), nullptr, nullptr),
        "打开解码测试文件");
    input_.reset(input);
    ffmpeg::FfmpegException::throwIfError(
        avformat_find_stream_info(input, nullptr), "读取测试轨道");
    index_ = av_find_best_stream(input, type, -1, -1, nullptr, 0);
    ffmpeg::FfmpegException::throwIfError(index_, "选择测试轨道");
  }

  ffmpeg::StreamInfo stream() const {
    const auto* source = input_->streams[index_];
    return {index_, ffmpeg::CodecParameters(*source->codecpar),
            source->time_base};
  }

  std::vector<ffmpeg::Packet> ReadPackets() {
    std::vector<ffmpeg::Packet> packets;
    for (;;) {
      ffmpeg::Packet packet;
      const int result = av_read_frame(input_.get(), packet.get());
      if (result == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(result, "读取测试数据包");
      if (packet->stream_index == index_) {
        packets.push_back(std::move(packet));
      }
    }
    REQUIRE_FALSE(packets.empty());
    return packets;
  }

  void SeekToStart() {
    ffmpeg::FfmpegException::throwIfError(
        av_seek_frame(input_.get(), index_, 0, AVSEEK_FLAG_BACKWARD),
        "定位测试文件起点");
    avformat_flush(input_.get());
  }

 private:
  struct CloseInput {
    void operator()(AVFormatContext* input) const {
      avformat_close_input(&input);
    }
  };
  std::unique_ptr<AVFormatContext, CloseInput> input_;
  int index_ = -1;
};

struct FrameRecord {
  std::int64_t pts;
  int format;
  int width;
  int height;
  int samples;
  std::uint64_t hash;

  bool operator==(const FrameRecord& other) const {
    return pts == other.pts && format == other.format && width == other.width &&
           height == other.height && samples == other.samples &&
           hash == other.hash;
  }
};

// Hash visible pixels/samples only; alignment padding is not media content.
FrameRecord Record(const AVFrame& frame) {
  std::vector<std::uint8_t> data;
  if (frame.nb_samples > 0) {
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const int planes =
        av_sample_fmt_is_planar(format) ? frame.ch_layout.nb_channels : 1;
    const int bytes = frame.nb_samples * av_get_bytes_per_sample(format) *
                      (planes == 1 ? frame.ch_layout.nb_channels : 1);
    for (int plane = 0; plane < planes; ++plane) {
      data.insert(data.end(), frame.extended_data[plane],
                  frame.extended_data[plane] + bytes);
    }
  } else {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const int size =
        av_image_get_buffer_size(format, frame.width, frame.height, 1);
    ffmpeg::FfmpegException::throwIfError(size, "计算测试图像大小");
    data.resize(static_cast<std::size_t>(size));
    ffmpeg::FfmpegException::throwIfError(
        av_image_copy_to_buffer(data.data(), size, frame.data, frame.linesize,
                                format, frame.width, frame.height, 1),
        "复制测试图像");
  }
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto byte : data) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return {frame.pts,    frame.format,     frame.width,
          frame.height, frame.nb_samples, hash};
}

// A separate raw FFmpeg loop provides the output count, timestamps and content
// oracle; it does not use the Decoder under test.
std::vector<FrameRecord> ReferenceDecode(
    const ffmpeg::StreamInfo& stream,
    const std::vector<ffmpeg::Packet>& packets) {
  const auto* codec =
      avcodec_find_decoder(stream.codec_parameters.get()->codec_id);
  REQUIRE(codec != nullptr);
  ffmpeg::CodecContext context(codec);
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_to_context(context.get(),
                                    stream.codec_parameters.get()),
      "设置参考解码参数");
  context.get()->pkt_timebase = stream.time_base;
  context.get()->thread_count = 0;
  ffmpeg::FfmpegException::throwIfError(
      avcodec_open2(context.get(), codec, nullptr), "打开参考解码器");
  ffmpeg::Frame frame;
  std::vector<FrameRecord> records;
  const auto receive = [&] {
    for (;;) {
      const int result = avcodec_receive_frame(context.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
      ffmpeg::FfmpegException::throwIfError(result, "参考解码接收帧");
      records.push_back(Record(*frame.get()));
    }
  };
  for (const auto& packet : packets) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(context.get(), packet.get()), "参考解码发送包");
    REQUIRE(receive() == AVERROR(EAGAIN));
  }
  ffmpeg::FfmpegException::throwIfError(
      avcodec_send_packet(context.get(), nullptr), "参考解码EOF");
  REQUIRE(receive() == AVERROR_EOF);
  return records;
}

ffmpeg::DecodeResult ReceiveRecords(ffmpeg::Decoder& decoder,
                                    std::vector<FrameRecord>& records,
                                    ffmpeg::Frame* retained = nullptr) {
  ffmpeg::Frame frame;
  for (;;) {
    const auto result = decoder.ReceiveFrame(frame);
    if (result != ffmpeg::DecodeResult::kFrame) return result;
    records.push_back(Record(*frame.get()));
    if (retained && !retained->get()->buf[0]) *retained = frame.Ref();
  }
}

std::vector<FrameRecord> DecodeAll(ffmpeg::Decoder& decoder,
                                   const std::vector<ffmpeg::Packet>& packets,
                                   ffmpeg::Frame* retained = nullptr) {
  std::vector<FrameRecord> records;
  for (const auto& packet : packets) {
    REQUIRE(decoder.SendPacket(packet));
    REQUIRE(ReceiveRecords(decoder, records, retained) ==
            ffmpeg::DecodeResult::kNeedInput);
  }
  REQUIRE(decoder.Drain());
  REQUIRE(ReceiveRecords(decoder, records, retained) ==
          ffmpeg::DecodeResult::kEnd);
  return records;
}

TEST_CASE("指定软件解码器与原生FFmpeg的音视频输出一致", "[ffmpeg][decoder]") {
  for (const auto file : {"h264_aac.mp4", "h265_aac.mp4", "vp8_video.ivf"}) {
    for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
      if (type == AVMEDIA_TYPE_AUDIO &&
          std::string_view(file) == "vp8_video.ivf")
        continue;
      CAPTURE(file, type);
      Fixture fixture(file, type);
      const auto stream = fixture.stream();
      const auto packets = fixture.ReadPackets();
      const auto reference = ReferenceDecode(stream, packets);
      REQUIRE_FALSE(reference.empty());
      const auto* codec =
          avcodec_find_decoder(stream.codec_parameters.get()->codec_id);
      auto decoder = CreateDecoder(stream, codec->name);
      ffmpeg::Frame retained;
      const auto actual = DecodeAll(*decoder, packets, &retained);
      CHECK(actual == reference);
      CHECK(retained->time_base.num == stream.time_base.num);
      CHECK(retained->time_base.den == stream.time_base.den);
      auto automatic = CreateDecoder(stream);
      CHECK(DecodeAll(*automatic, packets) == reference);
    }
  }
}

TEST_CASE("指定名称不可用或与轨道不符时解码器拒绝打开", "[ffmpeg][decoder]") {
  Fixture fixture("h264_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto stream = fixture.stream();
  CHECK_THROWS_AS(ffmpeg::VideoDecoder(stream, "mw_missing_decoder"),
                  ffmpeg::FfmpegException);
  CHECK_THROWS_AS(ffmpeg::VideoDecoder(stream, "hevc"), std::invalid_argument);
  CHECK_THROWS_AS(ffmpeg::VideoDecoder(stream, "aac"), std::invalid_argument);
  CHECK_THROWS_AS(ffmpeg::VideoDecoder(stream, "h264_cuvid"),
                  std::invalid_argument);
  CHECK_THROWS_AS(ffmpeg::AudioDecoder(stream), std::invalid_argument);
  Fixture audio_fixture("h264_aac.mp4", AVMEDIA_TYPE_AUDIO);
  CHECK_THROWS_AS(ffmpeg::VideoDecoder(audio_fixture.stream()),
                  std::invalid_argument);
}

TEST_CASE("发送EAGAIN重送同一个包且EOF保留B帧尾部", "[ffmpeg][decoder]") {
  Fixture fixture("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto stream = fixture.stream();
  const auto packets = fixture.ReadPackets();
  const auto reference = ReferenceDecode(stream, packets);
  REQUIRE(reference.size() == 20);
  ffmpeg::VideoDecoder decoder(stream, "hevc");
  std::vector<FrameRecord> records;
  bool saw_backpressure = false;
  for (const auto& packet : packets) {
    if (!decoder.SendPacket(packet)) {
      saw_backpressure = true;
      REQUIRE(ReceiveRecords(decoder, records) ==
              ffmpeg::DecodeResult::kNeedInput);
      REQUIRE(decoder.SendPacket(packet));
    }
  }
  CHECK(saw_backpressure);
  REQUIRE(ReceiveRecords(decoder, records) == ffmpeg::DecodeResult::kNeedInput);
  CHECK(records.size() < reference.size());
  REQUIRE(decoder.Drain());
  REQUIRE(ReceiveRecords(decoder, records) == ffmpeg::DecodeResult::kEnd);
  CHECK(records == reference);
  CHECK(decoder.Drain());
  ffmpeg::Frame frame;
  CHECK(decoder.ReceiveFrame(frame) == ffmpeg::DecodeResult::kEnd);
}

TEST_CASE("VP8数据包缺少duration时仍保留有效PTS正常解码", "[ffmpeg][decoder]") {
  Fixture fixture("vp8_video.ivf", AVMEDIA_TYPE_VIDEO);
  const auto stream = fixture.stream();
  auto packets = fixture.ReadPackets();
  for (auto& packet : packets) {
    REQUIRE(packet->pts != AV_NOPTS_VALUE);
    packet->duration = 0;
  }
  const auto reference = ReferenceDecode(stream, packets);
  ffmpeg::VideoDecoder decoder(stream);
  const auto actual = DecodeAll(decoder, packets);
  REQUIRE_FALSE(actual.empty());
  CHECK(actual == reference);
  for (const auto& frame : actual) CHECK(frame.pts != AV_NOPTS_VALUE);
}

TEST_CASE("seek后Flush清理延迟帧并可重复解码已结束轨道", "[ffmpeg][decoder]") {
  Fixture fixture("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto stream = fixture.stream();
  const auto packets = fixture.ReadPackets();
  const auto reference = ReferenceDecode(stream, packets);
  ffmpeg::VideoDecoder decoder(stream);
  ffmpeg::Frame retained;
  REQUIRE(DecodeAll(decoder, packets, &retained) == reference);
  const auto first = Record(*retained.get());
  fixture.SeekToStart();
  decoder.Flush();
  CHECK(Record(*retained.get()) == first);
  REQUIRE(DecodeAll(decoder, fixture.ReadPackets()) == reference);

  decoder.Flush();
  // Leave codec-internal reordered frames pending, then seek and discard them.
  for (std::size_t i = 0; i < packets.size() / 2; ++i) {
    REQUIRE(decoder.SendPacket(packets[i]));
    std::vector<FrameRecord> discard;
    REQUIRE(ReceiveRecords(decoder, discard) ==
            ffmpeg::DecodeResult::kNeedInput);
  }
  fixture.SeekToStart();
  decoder.Flush();
  std::vector<FrameRecord> empty;
  CHECK(ReceiveRecords(decoder, empty) == ffmpeg::DecodeResult::kNeedInput);
  CHECK(empty.empty());
  CHECK(DecodeAll(decoder, fixture.ReadPackets()) == reference);
}

TEST_CASE("解码器和轨道参数销毁后保留的CPU帧仍可读取", "[ffmpeg][decoder]") {
  ffmpeg::Frame retained;
  FrameRecord before{};
  {
    Fixture fixture("h264_aac.mp4", AVMEDIA_TYPE_VIDEO);
    const auto packets = fixture.ReadPackets();
    ffmpeg::VideoDecoder decoder(fixture.stream());
    REQUIRE_FALSE(DecodeAll(decoder, packets, &retained).empty());
    before = Record(*retained.get());
  }
  CHECK(Record(*retained.get()) == before);
}

TEST_CASE("CUDA解码与重建设备共享且GPU帧独立于解码器生命周期",
          "[.][ffmpeg][decoder][cuda]") {
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  const auto* native =
      reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
  for (const auto& configuration :
       {std::pair<std::string_view, std::string_view>{"h264", "h264_aac.mp4"},
        {"h264_cuvid", "h264_aac.mp4"},
        {"hevc", "h265_cuda.mp4"},
        {"hevc_cuvid", "h265_cuda.mp4"},
        {"h264", "h264_cuda_dpb.mp4"},
        {"h264_cuvid", "h264_cuda_dpb.mp4"}}) {
    const auto& [codec, file] = configuration;
    CAPTURE(codec, file);
    Fixture fixture(file, AVMEDIA_TYPE_VIDEO);
    const auto stream = fixture.stream();
    const auto packets = fixture.ReadPackets();
    const auto reference = ReferenceDecode(stream, packets);
    std::vector<ffmpeg::Frame> retained;
    std::vector<FrameRecord> downloads;
    for (int reopen = 0; reopen < 2; ++reopen) {
      ffmpeg::Frame first;
      {
        ffmpeg::VideoDecoder decoder(stream, device, codec);
        std::size_t frames = 0;
        ffmpeg::Frame frame;
        const auto receive = [&] {
          for (;;) {
            const auto result = decoder.ReceiveFrame(frame);
            if (result != ffmpeg::DecodeResult::kFrame) return result;
            REQUIRE(frame->format == AV_PIX_FMT_CUDA);
            REQUIRE(frame->hw_frames_ctx != nullptr);
            const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
                frame->hw_frames_ctx->data);
            CHECK(pool->device_ctx == native);
            CHECK(pool->device_ref->data == device.get()->data);
            CHECK(frame->pts == reference.at(frames).pts);
            CHECK(frame->width == reference.at(frames).width);
            CHECK(frame->height == reference.at(frames).height);
            if (frames++ == 0) first = frame.Ref();
          }
        };
        const auto decode_packets = [&] {
          for (const auto& packet : packets) {
            REQUIRE(decoder.SendPacket(packet));
            REQUIRE(receive() == ffmpeg::DecodeResult::kNeedInput);
          }
          REQUIRE(decoder.Drain());
          REQUIRE(receive() == ffmpeg::DecodeResult::kEnd);
          CHECK(frames == reference.size());
        };
        decode_packets();
        ffmpeg::Frame downloaded;
        REQUIRE(av_hwframe_transfer_data(downloaded.get(), first.get(), 0) ==
                0);
        downloads.push_back(Record(*downloaded.get()));
        if (reopen == 0) {
          const auto before_flush = first.Ref();
          decoder.Flush();
          frames = 0;
          decode_packets();
          // Flush must preserve the device and references already delivered.
          ffmpeg::Frame previous;
          REQUIRE(av_hwframe_transfer_data(previous.get(), before_flush.get(),
                                           0) == 0);
          CHECK(Record(*previous.get()) == downloads.back());
        }
      }
      retained.push_back(std::move(first));
    }
    // Both decoder sessions are gone; GPU buffers still belong to these frames.
    for (std::size_t i = 0; i < retained.size(); ++i) {
      ffmpeg::Frame downloaded;
      REQUIRE(av_hwframe_transfer_data(downloaded.get(), retained[i].get(),
                                       0) == 0);
      CHECK(Record(*downloaded.get()) == downloads[i]);
    }
  }
}

}  // namespace
