#include "mw/streamer/pipeline/internal/remux_audio_probe.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include "mpeg4-aac.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer::internal {
namespace {

using ffmpeg::FfmpegException;

ffmpeg::StreamInfo WithConfig(const ffmpeg::StreamInfo& stream,
                              const std::uint8_t* data, std::size_t size) {
  if (size < 2 || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
    throw std::invalid_argument("AAC 探测配置大小无效");
  }
  auto result = stream;
  auto* parameters = result.codec_parameters.get();
  auto* copy = static_cast<std::uint8_t*>(
      av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!copy) throw std::bad_alloc();
  std::memcpy(copy, data, size);
  av_freep(&parameters->extradata);
  parameters->extradata = copy;
  parameters->extradata_size = static_cast<int>(size);
  return result;
}

ffmpeg::CodecContext OpenDecoder(const ffmpeg::StreamInfo& stream) {
  const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (!codec) throw std::runtime_error("AAC 探测缺少解码器");
  ffmpeg::CodecContext decoder(codec);
  auto* context = decoder.get();
  FfmpegException::throwIfError(
      avcodec_parameters_to_context(context, stream.codec_parameters.get()),
      "设置 AAC 探测解码参数");
  context->pkt_timebase = stream.time_base;
  context->thread_count = 1;
  // Compare whole coded frames; keep caller-owned priming/trimming side data.
  context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  context->strict_std_compliance = FF_COMPLIANCE_STRICT;
  context->err_recognition = AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
  FfmpegException::throwIfError(avcodec_open2(context, codec, nullptr),
                                "打开 AAC 探测解码器");
  return decoder;
}

bool IsAdts(const ffmpeg::Packet& packet) {
  return packet->size >= 2 && packet->data[0] == 0xff &&
         (packet->data[1] & 0xf0) == 0xf0;
}

ffmpeg::StreamInfo AdtsConfig(const ffmpeg::StreamInfo& stream,
                              const std::vector<ffmpeg::Packet>& packets) {
  const auto* filter = av_bsf_get_by_name("aac_adtstoasc");
  if (!filter) throw std::runtime_error("AAC 探测缺少 aac_adtstoasc");
  AVBSFContext* raw = nullptr;
  FfmpegException::throwIfError(av_bsf_alloc(filter, &raw),
                                "分配 AAC 配置提取器");
  const auto free_filter = [](AVBSFContext* context) { av_bsf_free(&context); };
  std::unique_ptr<AVBSFContext, decltype(free_filter)> context(raw,
                                                               free_filter);
  FfmpegException::throwIfError(
      avcodec_parameters_copy(raw->par_in, stream.codec_parameters.get()),
      "设置 AAC 配置提取参数");
  raw->time_base_in = stream.time_base;
  FfmpegException::throwIfError(av_bsf_init(raw), "初始化 AAC 配置提取器");
  for (const auto& packet : packets) {
    if (!IsAdts(packet)) continue;
    auto input = packet.Ref();
    FfmpegException::throwIfError(av_bsf_send_packet(raw, input.get()),
                                  "提取 AAC ADTS 配置");
    ffmpeg::Packet output;
    int status;
    while ((status = av_bsf_receive_packet(raw, output.get())) >= 0) {
      std::size_t size = 0;
      const auto* data = av_packet_get_side_data(
          output.get(), AV_PKT_DATA_NEW_EXTRADATA, &size);
      if (data && size) return WithConfig(stream, data, size);
      output.Unref();
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF)
      FfmpegException::throwIfError(status, "读取 AAC ADTS 配置");
  }
  throw std::runtime_error("AAC ADTS 样本未提供有效配置");
}

std::vector<ffmpeg::Frame> DecodeSample(
    const ffmpeg::StreamInfo& stream,
    const std::vector<ffmpeg::Packet>& packets, bool candidate) {
  auto decoder = OpenDecoder(stream);
  auto* context = decoder.get();
  const auto* parameters = stream.codec_parameters.get();
  std::vector<ffmpeg::Frame> frames;
  for (const auto& packet : packets) {
    FfmpegException::throwIfError(avcodec_send_packet(context, packet.get()),
                                  "投递 AAC 探测包");
    const auto before = frames.size();
    for (;;) {
      ffmpeg::Frame frame;
      const int status = avcodec_receive_frame(context, frame.get());
      if (status == AVERROR(EAGAIN)) break;
      FfmpegException::throwIfError(status, "解码 AAC 探测样本");
      if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags ||
          frame->nb_samples != 1024 ||
          frame->sample_rate != parameters->sample_rate ||
          av_channel_layout_compare(&frame->ch_layout,
                                    &parameters->ch_layout) != 0 ||
          (candidate && context->profile != AV_PROFILE_AAC_LOW) ||
          (!candidate && context->profile != AV_PROFILE_UNKNOWN &&
           context->profile != -1 && context->profile != AV_PROFILE_AAC_LOW)) {
        throw std::runtime_error("AAC 短样本不符合稳定的 LC 兼容配置");
      }
      frames.push_back(std::move(frame));
    }
    if (frames.size() != before + 1)
      throw std::runtime_error("AAC 探测包未生成单个完整音频帧");
  }
  // This bounded compatibility check assumes a stable stream. FFmpeg may log
  // and ignore late implicit SBR or unsupported tools without returning an
  // error; the public decoder API cannot certify their absence here. Do not
  // treat matching PCM as recovery of the source profile or a syntax audit.
  return frames;
}

void CompareFrames(const AVFrame& expected, const AVFrame& actual) {
  if (expected.format != actual.format ||
      expected.sample_rate != actual.sample_rate ||
      expected.nb_samples != actual.nb_samples ||
      av_channel_layout_compare(&expected.ch_layout, &actual.ch_layout) != 0) {
    throw std::runtime_error("AAC 候选配置改变探测帧格式");
  }
  const auto format = static_cast<AVSampleFormat>(expected.format);
  const int bytes = av_get_bytes_per_sample(format);
  const bool planar = av_sample_fmt_is_planar(format) != 0;
  const int channels = expected.ch_layout.nb_channels;
  if (bytes <= 0 || channels <= 0)
    throw std::runtime_error("AAC 探测帧缺少有效采样格式");
  const auto plane_bytes = static_cast<std::size_t>(expected.nb_samples) *
                           static_cast<std::size_t>(bytes) *
                           static_cast<std::size_t>(planar ? 1 : channels);
  for (int plane = 0; plane < (planar ? channels : 1); ++plane) {
    if (!expected.extended_data[plane] || !actual.extended_data[plane] ||
        std::memcmp(expected.extended_data[plane], actual.extended_data[plane],
                    plane_bytes) != 0) {
      throw std::runtime_error("AAC 候选配置改变探测样本 PCM");
    }
  }
}

}  // namespace

ffmpeg::StreamInfo ProbeRemuxAudio(const ffmpeg::StreamInfo& stream,
                                   const std::vector<ffmpeg::Packet>& packets) {
  stream.Validate();
  const auto* parameters = stream.codec_parameters.get();
  if (parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
      parameters->codec_id != AV_CODEC_ID_AAC ||
      parameters->extradata_size != 0 || packets.empty() ||
      packets.size() > 8) {
    throw std::invalid_argument("AAC 探测需要缺少配置的音轨及一至八个音频包");
  }
  for (const auto& packet : packets) {
    if (!packet.get() || !packet->data || packet->size <= 0 ||
        packet->stream_index != stream.stream_index) {
      throw std::invalid_argument("AAC 探测包与选中音轨不匹配");
    }
  }
  for (const auto& packet : packets) {
    std::size_t size = 0;
    const auto* data =
        av_packet_get_side_data(packet.get(), AV_PKT_DATA_NEW_EXTRADATA, &size);
    if (data && size) {
      auto result = WithConfig(stream, data, size);
      OpenDecoder(result);
      return result;
    }
  }
  if (std::any_of(packets.begin(), packets.end(), IsAdts)) {
    auto result = AdtsConfig(stream, packets);
    OpenDecoder(result);
    return result;
  }
  if (parameters->profile != AV_PROFILE_UNKNOWN && parameters->profile != -1 &&
      parameters->profile != AV_PROFILE_AAC_LOW) {
    throw std::runtime_error("AAC 已知 profile 不适用 LC 短样本兼容探测");
  }
  const auto& layout = parameters->ch_layout;
  if (layout.order != AV_CHANNEL_ORDER_NATIVE ||
      !((layout.nb_channels == 1 && layout.u.mask == AV_CH_LAYOUT_MONO) ||
        (layout.nb_channels == 2 && layout.u.mask == AV_CH_LAYOUT_STEREO))) {
    throw std::runtime_error("AAC 兼容探测仅支持明确的单声道或双声道布局");
  }
  constexpr std::array<int, 13> rates = {96000, 88200, 64000, 48000, 44100,
                                         32000, 24000, 22050, 16000, 12000,
                                         11025, 8000,  7350};
  const auto rate =
      std::find(rates.begin(), rates.end(), parameters->sample_rate);
  if (rate == rates.end())
    throw std::runtime_error("AAC 兼容探测不支持该采样率");
  mpeg4_aac_t config{};
  config.profile = MPEG4_AAC_LC;
  config.sampling_frequency_index =
      static_cast<std::uint8_t>(rate - rates.begin());
  config.channel_configuration = static_cast<std::uint8_t>(layout.nb_channels);
  std::array<std::uint8_t, 2> asc{};
  if (mpeg4_aac_audio_specific_config_save(&config, asc.data(), asc.size()) !=
      2)
    throw std::runtime_error("无法构造 AAC 兼容探测配置");
  auto result = WithConfig(stream, asc.data(), asc.size());
  const auto baseline = DecodeSample(stream, packets, false);
  const auto candidate = DecodeSample(result, packets, true);
  for (std::size_t index = 0; index < baseline.size(); ++index)
    CompareFrames(*baseline[index].get(), *candidate[index].get());
  result.codec_parameters.get()->profile = AV_PROFILE_AAC_LOW;
  return result;
}

}  // namespace mw::streamer::internal
