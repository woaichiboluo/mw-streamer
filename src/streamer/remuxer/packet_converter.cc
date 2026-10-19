#include "mw/streamer/remuxer/packet_converter.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
}

#include "Extension/Factory.h"
#include "Network/Buffer.h"
#include "ext-codec/AAC.h"
#include "ext-codec/H264.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::internal {
namespace {

class PacketBuffer final : public toolkit::Buffer {
 public:
  explicit PacketBuffer(const ffmpeg::Packet& packet) : packet_(packet.Ref()) {}
  char* data() const override { return reinterpret_cast<char*>(packet_->data); }
  std::size_t size() const override {
    return static_cast<std::size_t>(packet_->size);
  }

 private:
  ffmpeg::Packet packet_;
};

class ZeroPtsFrame final : public mediakit::FrameCacheAble {
 public:
  explicit ZeroPtsFrame(const mediakit::Frame::Ptr& frame)
      : FrameCacheAble(frame) {}
  std::uint64_t pts() const override { return 0; }
};

mediakit::CodecId Codec(const AVCodecParameters& parameters) {
  if (parameters.codec_type == AVMEDIA_TYPE_VIDEO) {
    if (parameters.codec_id == AV_CODEC_ID_H264) return mediakit::CodecH264;
    if (parameters.codec_id == AV_CODEC_ID_HEVC) return mediakit::CodecH265;
  } else if (parameters.codec_type == AVMEDIA_TYPE_AUDIO &&
             parameters.codec_id == AV_CODEC_ID_AAC) {
    return mediakit::CodecAAC;
  }
  throw std::invalid_argument("Remuxer 仅支持 H264、H265 和 AAC 编码轨道");
}

bool ValidNalHeader(mediakit::CodecId codec, const char* data,
                    std::size_t size) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
  if (size < (codec == mediakit::CodecH264 ? 2U : 3U))
    throw std::invalid_argument("视频 NAL 负载截断");
  if (bytes[0] & 0x80) return false;
  if (codec == mediakit::CodecH264) {
    const auto type = bytes[0] & 0x1f;
    if (type == 0 || type >= 24)
      throw std::invalid_argument("H264 NAL 类型无效");
    return true;
  }
  return (bytes[1] & 0x07) != 0;
}

void ValidateNal(mediakit::CodecId codec, const char* data, std::size_t size) {
  if (!ValidNalHeader(codec, data, size))
    throw std::invalid_argument("视频 NAL 头部无效");
}

bool HasInvalidNalHeader(mediakit::CodecId codec, const char* data,
                         std::size_t size) {
  const auto prefix = mediakit::prefixSize(data, size);
  if (!prefix) throw std::invalid_argument("视频包缺少 Annex B 起始码");
  bool invalid = false;
  mediakit::splitH264(
      data, size, prefix,
      [&](const char* nal, std::size_t bytes, std::size_t nal_prefix) {
        if (!ValidNalHeader(codec, nal + nal_prefix, bytes - nal_prefix))
          invalid = true;
      });
  return invalid;
}

toolkit::Buffer::Ptr FilterInvalidNals(mediakit::CodecId codec,
                                       const toolkit::Buffer& buffer) {
  // Use FFmpeg's NAL parser to skip corrupt headers without decoding or
  // discarding valid NALs from the same packet. Healthy packets bypass this.
  const auto* filter = av_bsf_get_by_name("filter_units");
  if (!filter) throw std::runtime_error("FFmpeg 缺少 filter_units 过滤器");
  AVBSFContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(av_bsf_alloc(filter, &raw),
                                        "创建视频 NAL 过滤器");
  const auto free_filter = [](AVBSFContext* context) { av_bsf_free(&context); };
  std::unique_ptr<AVBSFContext, decltype(free_filter)> context(raw,
                                                               free_filter);
  raw->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
  raw->par_in->codec_id =
      codec == mediakit::CodecH264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
  raw->time_base_in = AVRational{1, 1000};
  // All NAL types pass; parsing alone removes invalid headers. The input is
  // already Annex B, so container extradata and length sizes are not needed.
  ffmpeg::FfmpegException::throwIfError(
      av_opt_set(raw->priv_data, "pass_types", "0-63", 0),
      "设置视频 NAL 过滤器");
  ffmpeg::FfmpegException::throwIfError(av_bsf_init(raw),
                                        "初始化视频 NAL 过滤器");
  ffmpeg::Packet packet;
  ffmpeg::FfmpegException::throwIfError(
      av_new_packet(packet.get(), static_cast<int>(buffer.size())),
      "分配视频 NAL 过滤包");
  std::memcpy(packet->data, buffer.data(), buffer.size());
  ffmpeg::FfmpegException::throwIfError(av_bsf_send_packet(raw, packet.get()),
                                        "提交视频 NAL 过滤包");
  const int result = av_bsf_receive_packet(raw, packet.get());
  if (result == AVERROR(EAGAIN)) return nullptr;
  ffmpeg::FfmpegException::throwIfError(result, "过滤视频 NAL 头部");
  return std::make_shared<PacketBuffer>(packet);
}

std::uint32_t ReadLength(const std::uint8_t* data, std::size_t size,
                         std::size_t& offset, int width) {
  if (static_cast<std::size_t>(width) > size - offset) {
    throw std::invalid_argument("视频 NAL 长度字段截断");
  }
  std::uint32_t length = 0;
  for (int index = 0; index < width; ++index) {
    length = (length << 8) | data[offset++];
  }
  return length;
}

bool LengthPrefixed(mediakit::CodecId codec, const toolkit::Buffer& buffer,
                    int width) {
  if (!width) return false;
  const auto* data = reinterpret_cast<const std::uint8_t*>(buffer.data());
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    if (static_cast<std::size_t>(width) > buffer.size() - offset) return false;
    std::uint32_t length = 0;
    for (int index = 0; index < width; ++index)
      length = (length << 8) | data[offset++];
    if (length < (codec == mediakit::CodecH264 ? 2U : 3U) ||
        length > buffer.size() - offset)
      return false;
    offset += length;
  }
  return true;
}

void AppendNal(std::string& output, mediakit::CodecId codec,
               const std::uint8_t* data, std::size_t size, std::size_t& offset,
               int length_width, bool validate_header = true) {
  const auto length = ReadLength(data, size, offset, length_width);
  if (length < (codec == mediakit::CodecH264 ? 2U : 3U) ||
      length > size - offset) {
    throw std::invalid_argument("视频 NAL 负载截断");
  }
  if (validate_header)
    ValidateNal(codec, reinterpret_cast<const char*>(data + offset), length);
  output.append("\0\0\0\1", 4);
  output.append(reinterpret_cast<const char*>(data + offset), length);
  offset += length;
}

std::string Configuration(mediakit::CodecId codec, const std::uint8_t* data,
                          std::size_t size, int& length_width) {
  const auto minimum = codec == mediakit::CodecH264 ? 7U : 23U;
  if (size < minimum || data[0] != 1) {
    throw std::invalid_argument("视频编码配置记录无效");
  }
  std::string annex_b;
  std::size_t offset;
  if (codec == mediakit::CodecH264) {
    length_width = (data[4] & 0x03) + 1;
    offset = 6;
    const auto sps_count = data[5] & 0x1f;
    for (int index = 0; index < sps_count; ++index)
      AppendNal(annex_b, codec, data, size, offset, 2);
    if (offset == size) throw std::invalid_argument("avcC 缺少 PPS 字段");
    const auto pps_count = data[offset++];
    for (int index = 0; index < pps_count; ++index)
      AppendNal(annex_b, codec, data, size, offset, 2);
  } else {
    length_width = (data[21] & 0x03) + 1;
    offset = 23;
    for (int array = 0; array < data[22]; ++array) {
      if (offset == size) throw std::invalid_argument("hvcC 数组字段截断");
      ++offset;
      const auto count = ReadLength(data, size, offset, 2);
      for (std::uint32_t index = 0; index < count; ++index)
        AppendNal(annex_b, codec, data, size, offset, 2);
    }
    if (offset != size) throw std::invalid_argument("hvcC 存在多余数据");
  }
  return annex_b;
}

}  // namespace

PacketConverter::PacketConverter(
    const std::vector<ffmpeg::StreamInfo>& streams) {
  for (const auto& stream : streams) {
    stream.Validate();
    if (std::any_of(streams_.begin(), streams_.end(), [&](const auto& value) {
          return value.index == stream.stream_index;
        })) {
      throw std::invalid_argument("Remuxer 编码轨道索引重复");
    }
    const auto& parameters = *stream.codec_parameters.get();
    const auto codec = Codec(parameters);
    auto track = mediakit::Factory::getTrackByCodecId(codec);
    track->setIndex(stream.stream_index);
    if (parameters.extradata_size == 0) {
      streams_.push_back({stream.stream_index, stream.time_base, codec, 0});
      tracks_.push_back(std::move(track));
      continue;
    }
    const auto* data = parameters.extradata;
    const auto size = static_cast<std::size_t>(parameters.extradata_size);
    int length_width = 0;
    if (codec == mediakit::CodecAAC) {
      if (size < 2) throw std::invalid_argument("AAC 编码配置截断");
      track->setExtraData(data, size);
      const auto audio = std::static_pointer_cast<mediakit::AudioTrack>(track);
      if (audio->getAudioSampleRate() <= 0 || audio->getAudioChannel() <= 0 ||
          audio->getAudioSampleRate() != parameters.sample_rate ||
          audio->getAudioChannel() != parameters.ch_layout.nb_channels) {
        throw std::invalid_argument("AAC 配置与编码轨道的采样率或声道数不一致");
      }
    } else {
      std::string config;
      if (mediakit::prefixSize(reinterpret_cast<const char*>(data), size)) {
        config.assign(reinterpret_cast<const char*>(data), size);
      } else {
        config = Configuration(codec, data, size, length_width);
      }
      if (HasInvalidNalHeader(codec, config.data(), config.size()))
        throw std::invalid_argument("视频编码配置 NAL 头部无效");
      auto buffer = std::make_shared<toolkit::BufferString>(std::move(config));
      auto frame = mediakit::Factory::getFrameFromBuffer(codec, buffer, 0, 0);
      frame->setIndex(stream.stream_index);
      track->inputFrame(frame);
    }
    if (!track->ready()) {
      throw std::invalid_argument("Remuxer 编码配置缺少有效参数集");
    }
    streams_.push_back(
        {stream.stream_index, stream.time_base, codec, length_width});
    tracks_.push_back(std::move(track));
  }
}

const std::vector<mediakit::Track::Ptr>& PacketConverter::tracks()
    const noexcept {
  return tracks_;
}

mediakit::Frame::Ptr PacketConverter::Convert(const ffmpeg::Packet& packet,
                                              std::uint64_t dts_ms,
                                              std::uint64_t pts_ms) {
  const auto* input = packet.get();
  if (!input || !input->data || input->size <= 0) {
    throw std::invalid_argument("Remuxer 需要非空编码包");
  }
  const auto stream = std::find_if(
      streams_.begin(), streams_.end(),
      [&](const auto& value) { return value.index == input->stream_index; });
  if (stream == streams_.end() || input->time_base.num <= 0 ||
      input->time_base.den <= 0 ||
      av_cmp_q(input->time_base, stream->time_base) != 0) {
    throw std::invalid_argument("Remuxer 编码包轨道或时间基不匹配");
  }
  toolkit::Buffer::Ptr buffer = std::make_shared<PacketBuffer>(packet);
  if (stream->codec != mediakit::CodecAAC) {
    if (LengthPrefixed(stream->codec, *buffer, stream->nal_length_size) ||
        !mediakit::prefixSize(buffer->data(), buffer->size())) {
      if (!stream->nal_length_size) {
        throw std::invalid_argument(
            "视频包需要 Annex B 格式或编码配置中的 NAL 长度");
      }
      std::string annex_b;
      std::size_t offset = 0;
      while (offset < buffer->size()) {
        AppendNal(annex_b, stream->codec,
                  reinterpret_cast<const std::uint8_t*>(buffer->data()),
                  buffer->size(), offset, stream->nal_length_size, false);
      }
      buffer = std::make_shared<toolkit::BufferString>(std::move(annex_b));
    }
    if (HasInvalidNalHeader(stream->codec, buffer->data(), buffer->size())) {
      buffer = FilterInvalidNals(stream->codec, *buffer);
      if (!buffer) return nullptr;
    }
  }
  auto frame = mediakit::Factory::getFrameFromBuffer(
      stream->codec, std::move(buffer), dts_ms, pts_ms);
  if (stream->codec == mediakit::CodecAAC && frame->prefixSize()) {
    std::size_t offset = 0;
    while (offset < frame->size()) {
      const auto remaining = frame->size() - offset;
      const auto length = mediakit::getAacFrameLength(
          reinterpret_cast<const std::uint8_t*>(frame->data() + offset),
          remaining);
      if (length <= 7 || static_cast<std::size_t>(length) > remaining) {
        throw std::invalid_argument("AAC ADTS 帧无效或负载截断");
      }
      offset += static_cast<std::size_t>(length);
    }
  }
  if (stream->codec == mediakit::CodecAAC) {
    const auto& track =
        tracks_[static_cast<std::size_t>(stream - streams_.begin())];
    if (!track->ready()) {
      if (!frame->prefixSize()) {
        throw std::invalid_argument("缺少 AAC 配置时输入包必须包含 ADTS 头");
      }
      // The output layer clones tracks; retain configuration discovered from
      // ADTS here as well so subsequent raw AAC packets can use it.
      track->inputFrame(frame);
      if (!track->ready()) {
        throw std::invalid_argument("AAC ADTS 头缺少有效编码配置");
      }
    }
  }
  frame->setIndex(stream->index);
  if (pts_ms == 0 && dts_ms != 0) {
    frame = std::make_shared<ZeroPtsFrame>(frame);
  }
  return frame;
}

}  // namespace mw::streamer::internal
