#include "mw/streamer/ffmpeg/encoder.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "mw/streamer/ffmpeg/decoder.h"
#include "mw/streamer/ffmpeg/error.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};
constexpr std::int64_t kVideoStart = 10000000000;
constexpr std::int64_t kAudioStart = kVideoStart + 20000000;

class Options final {
 public:
  ~Options() { av_dict_free(&value_); }
  void Set(const char* key, const char* value) {
    ffmpeg::FfmpegException::throwIfError(av_dict_set(&value_, key, value, 0),
                                          "设置编码测试选项");
  }
  AVDictionary* get() const { return value_; }

 private:
  AVDictionary* value_ = nullptr;
};

ffmpeg::VideoEncoderConfig VideoConfig() {
  ffmpeg::VideoEncoderConfig config;
  config.encoder_name = "libx264";
  config.width = 64;
  config.height = 64;
  config.pixel_format = AV_PIX_FMT_YUV420P;
  config.frame_rate = {30, 1};
  config.time_base = {1, 90000};
  config.max_b_frames = 2;
  return config;
}

ffmpeg::Frame VideoFrame(int index) {
  ffmpeg::Frame frame;
  frame->width = 64;
  frame->height = 64;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->time_base = kNanoseconds;
  frame->pts = kVideoStart + av_rescale_q(index, {1, 30}, kNanoseconds);
  frame->duration = av_rescale_q(1, {1, 30}, kNanoseconds);
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配编码测试图像");
  for (int plane = 0; plane < 3; ++plane) {
    const int size = plane == 0 ? 64 : 32;
    const int value = plane == 0 ? 32 + index : 128;
    for (int row = 0; row < size; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane], value,
                  static_cast<std::size_t>(size));
    }
  }
  return frame;
}

ffmpeg::Frame AudioFrame(int samples, std::int64_t offset) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  frame->time_base = kNanoseconds;
  frame->pts = kAudioStart + av_rescale_q(offset, {1, 48000}, kNanoseconds);
  frame->duration = av_rescale_q(samples, {1, 48000}, kNanoseconds);
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配编码测试音频");
  for (int channel = 0; channel < 2; ++channel) {
    auto* data = reinterpret_cast<float*>(frame->extended_data[channel]);
    for (int sample = 0; sample < samples; ++sample) {
      data[sample] = static_cast<float>(
          0.2 * std::sin(2 * 3.141592653589793 * 440 *
                         static_cast<double>(offset + sample) / 48000));
    }
  }
  return frame;
}

ffmpeg::EncodeResult ReceivePackets(ffmpeg::Encoder& encoder,
                                    std::vector<ffmpeg::Packet>& packets) {
  ffmpeg::Packet packet;
  for (;;) {
    const auto result = encoder.ReceivePacket(packet);
    if (result != ffmpeg::EncodeResult::kPacket) return result;
    REQUIRE(packet->size > 0);
    packets.push_back(packet.Ref());
  }
}

void Send(ffmpeg::Encoder& encoder, const ffmpeg::Frame& frame,
          std::vector<ffmpeg::Packet>& packets, int* retries = nullptr) {
  while (!encoder.SendFrame(frame)) {
    if (retries) ++*retries;
    REQUIRE(ReceivePackets(encoder, packets) ==
            ffmpeg::EncodeResult::kNeedInput);
  }
}

void Finish(ffmpeg::Encoder& encoder, std::vector<ffmpeg::Packet>& packets) {
  while (!encoder.Drain()) {
    REQUIRE(ReceivePackets(encoder, packets) ==
            ffmpeg::EncodeResult::kNeedInput);
  }
  REQUIRE(ReceivePackets(encoder, packets) == ffmpeg::EncodeResult::kEnd);
  ffmpeg::Packet packet;
  REQUIRE(encoder.ReceivePacket(packet) == ffmpeg::EncodeResult::kEnd);
  CHECK(packet->data == nullptr);
}

std::vector<ffmpeg::Frame> Decode(ffmpeg::Decoder& decoder,
                                  const std::vector<ffmpeg::Packet>& packets) {
  std::vector<ffmpeg::Frame> frames;
  ffmpeg::Frame frame;
  const auto receive = [&] {
    for (;;) {
      const auto result = decoder.ReceiveFrame(frame);
      if (result != ffmpeg::DecodeResult::kFrame) return result;
      frames.push_back(frame.Ref());
    }
  };
  for (const auto& packet : packets) {
    while (!decoder.SendPacket(packet)) {
      REQUIRE(receive() == ffmpeg::DecodeResult::kNeedInput);
    }
    REQUIRE(receive() == ffmpeg::DecodeResult::kNeedInput);
  }
  while (!decoder.Drain()) {
    REQUIRE(receive() == ffmpeg::DecodeResult::kNeedInput);
  }
  REQUIRE(receive() == ffmpeg::DecodeResult::kEnd);
  return frames;
}

std::array<double, 3> PixelError(const ffmpeg::Frame& source,
                                 const ffmpeg::Frame& decoded) {
  ffmpeg::Frame downloaded;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_transfer_data(downloaded.get(), source.get(), 0),
      "下载CUDA测试图像以核验编码像素");
  REQUIRE(downloaded->format == AV_PIX_FMT_NV12);
  REQUIRE(decoded->format == AV_PIX_FMT_YUV420P);
  REQUIRE(downloaded->width == decoded->width);
  REQUIRE(downloaded->height == decoded->height);
  std::array<double, 3> mean_squared_error{};
  for (int plane = 0; plane < 3; ++plane) {
    const auto plane_index = static_cast<std::size_t>(plane);
    const int width = plane == 0 ? decoded->width : (decoded->width + 1) / 2;
    const int height = plane == 0 ? decoded->height : (decoded->height + 1) / 2;
    const int input_plane = plane == 0 ? 0 : 1;
    for (int row = 0; row < height; ++row) {
      const auto* expected = downloaded->data[input_plane] +
                             row * downloaded->linesize[input_plane];
      const auto* actual =
          decoded->data[plane] + row * decoded->linesize[plane];
      for (int column = 0; column < width; ++column) {
        const int input_column = plane == 0 ? column : 2 * column + (plane - 1);
        const int difference = expected[input_column] - actual[column];
        mean_squared_error[plane_index] += difference * difference;
      }
    }
    mean_squared_error[plane_index] /= width * height;
  }
  return mean_squared_error;
}

class Fixture final {
 public:
  Fixture() {
    AVFormatContext* input = nullptr;
    const std::string path =
        std::string(MW_STREAMER_FFMPEG_TEST_DATA_DIR) + "/h265_cuda.mp4";
    ffmpeg::FfmpegException::throwIfError(
        avformat_open_input(&input, path.c_str(), nullptr, nullptr),
        "打开CUDA编码测试文件");
    input_.reset(input);
    ffmpeg::FfmpegException::throwIfError(
        avformat_find_stream_info(input, nullptr), "读取CUDA编码测试轨道");
    index_ = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    ffmpeg::FfmpegException::throwIfError(index_, "选择CUDA编码测试视频");
  }

  ffmpeg::StreamInfo stream() const {
    const auto* source = input_->streams[index_];
    return {index_, ffmpeg::CodecParameters(*source->codecpar),
            source->time_base};
  }

  std::vector<ffmpeg::Packet> packets() {
    std::vector<ffmpeg::Packet> packets;
    for (;;) {
      ffmpeg::Packet packet;
      const int result = av_read_frame(input_.get(), packet.get());
      if (result == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(result, "读取CUDA编码测试包");
      if (packet->stream_index == index_) packets.push_back(std::move(packet));
    }
    REQUIRE_FALSE(packets.empty());
    return packets;
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

TEST_CASE("视频编码背压重送与B帧排空保留图像和非零时间线",
          "[ffmpeg][encoder][video]") {
  constexpr int kFrames = 40;
  Options options;
  options.Set("crf", "18");
  options.Set("preset", "medium");
  options.Set("x264-params", "b-adapt=0:rc-lookahead=5:sync-lookahead=0");
  options.Set("threads", "1");
  const auto* options_before = options.get();
  std::vector<ffmpeg::Packet> packets;
  ffmpeg::StreamInfo stream;
  {
    ffmpeg::VideoEncoder encoder(VideoConfig(), options.get());
    CHECK(options.get() == options_before);
    CHECK(std::string(av_dict_get(options.get(), "crf", nullptr, 0)->value) ==
          "18");
    CHECK(av_dict_count(options.get()) == 4);
    ffmpeg::Packet empty;
    REQUIRE(av_new_packet(empty.get(), 16) == 0);
    REQUIRE(encoder.ReceivePacket(empty) == ffmpeg::EncodeResult::kNeedInput);
    CHECK(empty->data == nullptr);
    int retries = 0;
    for (int index = 0; index < kFrames; ++index) {
      auto frame = VideoFrame(index);
      const auto* original_data = frame->data[0];
      const auto original_pts = frame->pts;
      const auto original_duration = frame->duration;
      Send(encoder, frame, packets, &retries);
      CHECK(frame->pts == original_pts);
      CHECK(frame->duration == original_duration);
      CHECK(av_cmp_q(frame->time_base, kNanoseconds) == 0);
      CHECK(frame->data[0] == original_data);
      CHECK(frame->data[0][0] == 32 + index);
    }
    CHECK(retries > 0);
    Finish(encoder, packets);
    stream = encoder.stream_info(3);
  }
  REQUIRE(packets.size() == kFrames);
  for (const auto& packet : packets) {
    CHECK(av_cmp_q(packet->time_base, {1, 90000}) == 0);
    CHECK(packet->duration == 3000);
    CHECK(packet->pts >= 900000);
  }
  CHECK(stream.stream_index == 3);
  CHECK(av_cmp_q(stream.time_base, {1, 90000}) == 0);
  CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_H264);
  CHECK(stream.codec_parameters.get()->width == 64);
  CHECK(stream.codec_parameters.get()->height == 64);
  REQUIRE(stream.codec_parameters.get()->extradata_size > 0);
  CHECK(packets.front()->dts < packets.front()->pts);
  ffmpeg::VideoDecoder decoder(stream);
  const auto frames = Decode(decoder, packets);
  REQUIRE(frames.size() == kFrames);
  int b_frames = 0;
  for (int index = 0; index < kFrames; ++index) {
    const auto& frame = frames[static_cast<std::size_t>(index)];
    CHECK(frame->pts == 900000 + index * 3000);
    REQUIRE(frame->format == AV_PIX_FMT_YUV420P);
    REQUIRE(frame->width == 64);
    REQUIRE(frame->height == 64);
    if (frame->pict_type == AV_PICTURE_TYPE_B) ++b_frames;
    for (int row = 0; row < 64; ++row) {
      CHECK(std::abs(frame->data[0][row * frame->linesize[0] + 31] -
                     (32 + index)) <= 2);
    }
  }
  CHECK(b_frames > 0);
}

TEST_CASE("音频编码保留共享起点偏移并排空AAC短尾及priming",
          "[ffmpeg][encoder][audio]") {
  constexpr int kBlocks = 4;
  constexpr int kTail = 137;
  std::unique_ptr<ffmpeg::AudioEncoder> encoder;
  {
    ffmpeg::AudioEncoderConfig config;
    config.encoder_name = "aac";
    encoder = std::make_unique<ffmpeg::AudioEncoder>(config);
    av_channel_layout_uninit(&config.channel_layout);
    av_channel_layout_default(&config.channel_layout, 1);
  }
  REQUIRE(encoder->frame_size() == 1024);
  std::vector<ffmpeg::Packet> packets;
  std::int64_t offset = 0;
  for (int index = 0; index <= kBlocks; ++index) {
    const int samples = index == kBlocks ? kTail : encoder->frame_size();
    auto frame = AudioFrame(samples, offset);
    const auto original_pts = frame->pts;
    const auto original_duration = frame->duration;
    const auto original_sample =
        reinterpret_cast<const float*>(frame->extended_data[0])[17];
    Send(*encoder, frame, packets);
    CHECK(frame->pts == original_pts);
    CHECK(frame->duration == original_duration);
    CHECK(frame->nb_samples == samples);
    CHECK(reinterpret_cast<const float*>(frame->extended_data[0])[17] ==
          original_sample);
    offset += samples;
  }
  Finish(*encoder, packets);
  auto stream = encoder->stream_info(4);
  encoder.reset();
  REQUIRE_FALSE(packets.empty());
  CHECK(stream.stream_index == 4);
  CHECK(av_cmp_q(stream.time_base, {1, 48000}) == 0);
  CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(stream.codec_parameters.get()->sample_rate == 48000);
  CHECK(stream.codec_parameters.get()->ch_layout.nb_channels == 2);
  REQUIRE(stream.codec_parameters.get()->extradata_size > 0);
  const auto origin = av_rescale_q(kAudioStart, kNanoseconds, stream.time_base);
  const int priming = stream.codec_parameters.get()->initial_padding;
  REQUIRE(priming > 0);
  CHECK(packets.front()->pts == origin - priming);
  CHECK(packets.back()->pts + packets.back()->duration == origin + offset);
  CHECK(av_rescale_q(origin, stream.time_base, kNanoseconds) - kVideoStart ==
        20000000);
  std::int64_t duration = 0;
  for (const auto& packet : packets) duration += packet->duration;
  CHECK(duration == offset + priming);
  ffmpeg::AudioDecoder decoder(stream);
  const auto frames = Decode(decoder, packets);
  REQUIRE_FALSE(frames.empty());
  std::int64_t decoded_samples = 0;
  double energy = 0;
  for (const auto& frame : frames) {
    CHECK(frame->sample_rate == 48000);
    CHECK(frame->ch_layout.nb_channels == 2);
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    decoded_samples += frame->nb_samples;
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    for (int index = 0; index < frame->nb_samples; ++index) {
      energy += static_cast<double>(samples[index] * samples[index]);
    }
  }
  CHECK(decoded_samples >= offset);
  CHECK(decoded_samples <= offset + priming + 1024);
  CHECK(energy > 1);
}

TEST_CASE("编码器拒绝不匹配配置与缺失时间戳的输入",
          "[ffmpeg][encoder][validation]") {
  SECTION("编码器不存在或类型错误") {
    auto video = VideoConfig();
    video.encoder_name = "not_an_encoder";
    CHECK_THROWS(ffmpeg::VideoEncoder(video));
    video.encoder_name = "aac";
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(video), std::invalid_argument);
    ffmpeg::AudioEncoderConfig audio;
    audio.encoder_name = "libx264";
    CHECK_THROWS_AS(ffmpeg::AudioEncoder(audio), std::invalid_argument);
  }
  SECTION("视频尺寸格式速率和时间基") {
    auto config = VideoConfig();
    config.width = 0;
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config), std::invalid_argument);
    config = VideoConfig();
    config.pixel_format = AV_PIX_FMT_NONE;
    CHECK_THROWS(ffmpeg::VideoEncoder(config));
    config.pixel_format = AV_PIX_FMT_RGB24;
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config), std::invalid_argument);
    config.pixel_format = AV_PIX_FMT_CUDA;
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config), std::invalid_argument);
    config = VideoConfig();
    config.frame_rate = {0, 1};
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config), std::invalid_argument);
    config = VideoConfig();
    config.time_base = {1, 0};
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config), std::invalid_argument);
  }
  SECTION("硬件编码需要外部CUDA设备") {
    auto config = VideoConfig();
    config.encoder_name = "hevc_nvenc";
    config.pixel_format = AV_PIX_FMT_NV12;
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config), std::invalid_argument);
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config, cpu), std::invalid_argument);
  }
  SECTION("显式CPU设备可以编码软件帧") {
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    CHECK_NOTHROW(ffmpeg::VideoEncoder(VideoConfig(), cpu));
  }
  SECTION("音频格式采样率和布局") {
    ffmpeg::AudioEncoderConfig config;
    config.sample_rate = 0;
    CHECK_THROWS_AS(ffmpeg::AudioEncoder(config), std::invalid_argument);
    config = ffmpeg::AudioEncoderConfig{};
    config.sample_format = AV_SAMPLE_FMT_NONE;
    CHECK_THROWS(ffmpeg::AudioEncoder(config));
    config = ffmpeg::AudioEncoderConfig{};
    av_channel_layout_uninit(&config.channel_layout);
    CHECK_THROWS_AS(ffmpeg::AudioEncoder(config), std::invalid_argument);
  }
  SECTION("帧尺寸格式时间戳和时间基") {
    ffmpeg::VideoEncoder encoder(VideoConfig());
    auto frame = VideoFrame(0);
    frame->width = 32;
    CHECK_THROWS_AS(encoder.SendFrame(frame), std::invalid_argument);
    frame->width = 64;
    frame->format = AV_PIX_FMT_NV12;
    CHECK_THROWS_AS(encoder.SendFrame(frame), std::invalid_argument);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->pts = AV_NOPTS_VALUE;
    CHECK_THROWS_AS(encoder.SendFrame(frame), std::invalid_argument);
    frame->pts = kVideoStart;
    frame->time_base = {0, 1};
    CHECK_THROWS_AS(encoder.SendFrame(frame), std::invalid_argument);
    ffmpeg::Frame empty;
    CHECK_THROWS_AS(encoder.SendFrame(empty), std::invalid_argument);
  }
  SECTION("音频输入需要匹配采样率布局和帧长度") {
    ffmpeg::AudioEncoder encoder(ffmpeg::AudioEncoderConfig{});
    auto frame = AudioFrame(1024, 0);
    frame->sample_rate = 44100;
    CHECK_THROWS_AS(encoder.SendFrame(frame), std::invalid_argument);
    frame->sample_rate = 48000;
    av_channel_layout_uninit(&frame->ch_layout);
    av_channel_layout_default(&frame->ch_layout, 1);
    CHECK_THROWS_AS(encoder.SendFrame(frame), std::invalid_argument);
    auto large = AudioFrame(2048, 0);
    CHECK_THROWS_AS(encoder.SendFrame(large), std::invalid_argument);
  }
}

TEST_CASE("CUDA解码编码共享外部设备且保留GPU输入帧",
          "[.][ffmpeg][encoder][cuda]") {
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  auto* native_device = device.get()->data;
  Fixture fixture;
  const auto input_stream = fixture.stream();
  ffmpeg::VideoDecoder decoder(input_stream, device, "hevc");
  const auto frames = Decode(decoder, fixture.packets());
  REQUIRE_FALSE(frames.empty());
  CHECK_THROWS_AS(ffmpeg::VideoEncoder(VideoConfig(), device),
                  std::invalid_argument);
  for (const auto name : {"h264_nvenc", "hevc_nvenc"}) {
    CAPTURE(name);
    auto config = VideoConfig();
    config.encoder_name = name;
    config.pixel_format = AV_PIX_FMT_RGB24;
    CHECK_THROWS_AS(ffmpeg::VideoEncoder(config, device),
                    std::invalid_argument);
    config.width = frames.front()->width;
    config.height = frames.front()->height;
    config.pixel_format = AV_PIX_FMT_NV12;
    config.bit_rate = 2000000;
    config.max_b_frames = 0;
    Options options;
    options.Set("preset", "p2");
    ffmpeg::VideoEncoder encoder(config, device, options.get());
    std::vector<ffmpeg::Packet> packets;
    for (const auto& frame : frames) {
      REQUIRE(frame->format == AV_PIX_FMT_CUDA);
      const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
          frame->hw_frames_ctx->data);
      CHECK(pool->device_ref->data == native_device);
      const auto* frame_pool = frame->hw_frames_ctx->data;
      const auto pts = frame->pts;
      const auto* buffer = frame->data[0];
      const auto picture_type = frame->pict_type;
      const auto dts = frame->pkt_dts;
      Send(encoder, frame, packets);
      CHECK(frame->hw_frames_ctx->data == frame_pool);
      CHECK(frame->data[0] == buffer);
      CHECK(frame->pts == pts);
      CHECK(frame->pict_type == picture_type);
      CHECK(frame->pkt_dts == dts);
    }
    Finish(encoder, packets);
    CHECK(device.get()->data == native_device);
    ffmpeg::VideoDecoder verification(encoder.stream_info(0));
    const auto output = Decode(verification, packets);
    REQUIRE(output.size() == frames.size());
    std::array<double, 3> maximum_mse{};
    for (std::size_t index = 0; index < output.size(); ++index) {
      CHECK(output[index]->width == config.width);
      CHECK(output[index]->height == config.height);
      CHECK(output[index]->pts == av_rescale_q(frames[index]->pts,
                                               frames[index]->time_base,
                                               config.time_base));
      CHECK(output[index]->format != AV_PIX_FMT_CUDA);
      const auto error = PixelError(frames[index], output[index]);
      for (std::size_t plane = 0; plane < maximum_mse.size(); ++plane) {
        maximum_mse[plane] = std::max(maximum_mse[plane], error[plane]);
      }
    }
    for (std::size_t plane = 0; plane < maximum_mse.size(); ++plane) {
      CAPTURE(name, plane, maximum_mse[plane]);
      // MSE 25 corresponds to 34.15 dB PSNR for an 8-bit plane. At 2 Mbps
      // this small fixture must retain its pixels, including NV12 UV ordering.
      CHECK(maximum_mse[plane] <= 25);
    }
  }
}

}  // namespace
