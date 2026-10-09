#include "mw/streamer/remuxer/packet_converter.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "Extension/Factory.h"
#include "Network/Buffer.h"
#include "ext-codec/H264.h"

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
  throw std::invalid_argument("Remuxer仅支持H264、H265和AAC编码轨道");
}

void ValidateNal(mediakit::CodecId codec, const char* data, std::size_t size) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
  if (size < (codec == mediakit::CodecH264 ? 2U : 3U) || (bytes[0] & 0x80)) {
    throw std::invalid_argument("视频NAL头部无效或负载截断");
  }
  if (codec == mediakit::CodecH264) {
    const auto type = bytes[0] & 0x1f;
    if (type == 0 || type >= 24) {
      throw std::invalid_argument("H264 NAL类型无效");
    }
  } else if (!(bytes[1] & 0x07)) {
    throw std::invalid_argument("H265 NAL temporal_id无效");
  }
}

void ValidateAnnexB(mediakit::CodecId codec, const char* data,
                    std::size_t size) {
  const auto prefix = mediakit::prefixSize(data, size);
  if (!prefix) throw std::invalid_argument("视频包缺少Annex B起始码");
  mediakit::splitH264(
      data, size, prefix,
      [&](const char* nal, std::size_t bytes, std::size_t nal_prefix) {
        ValidateNal(codec, nal + nal_prefix, bytes - nal_prefix);
      });
}

std::uint32_t ReadLength(const std::uint8_t* data, std::size_t size,
                         std::size_t& offset, int width) {
  if (static_cast<std::size_t>(width) > size - offset) {
    throw std::invalid_argument("视频NAL长度字段截断");
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
               int length_width) {
  const auto length = ReadLength(data, size, offset, length_width);
  if (length > size - offset) {
    throw std::invalid_argument("视频NAL负载截断");
  }
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
    if (offset == size) throw std::invalid_argument("avcC缺少PPS字段");
    const auto pps_count = data[offset++];
    for (int index = 0; index < pps_count; ++index)
      AppendNal(annex_b, codec, data, size, offset, 2);
  } else {
    length_width = (data[21] & 0x03) + 1;
    offset = 23;
    for (int array = 0; array < data[22]; ++array) {
      if (offset == size) throw std::invalid_argument("hvcC数组字段截断");
      ++offset;
      const auto count = ReadLength(data, size, offset, 2);
      for (std::uint32_t index = 0; index < count; ++index)
        AppendNal(annex_b, codec, data, size, offset, 2);
    }
    if (offset != size) throw std::invalid_argument("hvcC存在多余数据");
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
      throw std::invalid_argument("Remuxer编码轨道索引重复");
    }
    const auto& parameters = *stream.codec_parameters.get();
    const auto codec = Codec(parameters);
    auto track = mediakit::Factory::getTrackByCodecId(codec);
    track->setIndex(stream.stream_index);
    if (parameters.extradata_size == 0) {
      throw std::invalid_argument("Remuxer需要编码器提供全局编码配置");
    }
    const auto* data = parameters.extradata;
    const auto size = static_cast<std::size_t>(parameters.extradata_size);
    int length_width = 0;
    if (codec == mediakit::CodecAAC) {
      if (size < 2) throw std::invalid_argument("AAC编码配置截断");
      track->setExtraData(data, size);
      const auto audio = std::static_pointer_cast<mediakit::AudioTrack>(track);
      if (audio->getAudioSampleRate() <= 0 || audio->getAudioChannel() <= 0 ||
          audio->getAudioSampleRate() != parameters.sample_rate ||
          audio->getAudioChannel() != parameters.ch_layout.nb_channels) {
        throw std::invalid_argument("AAC配置与编码轨道的采样率或声道数不一致");
      }
    } else {
      std::string config;
      if (mediakit::prefixSize(reinterpret_cast<const char*>(data), size)) {
        config.assign(reinterpret_cast<const char*>(data), size);
      } else {
        config = Configuration(codec, data, size, length_width);
      }
      ValidateAnnexB(codec, config.data(), config.size());
      auto buffer = std::make_shared<toolkit::BufferString>(std::move(config));
      auto frame = mediakit::Factory::getFrameFromBuffer(codec, buffer, 0, 0);
      frame->setIndex(stream.stream_index);
      track->inputFrame(frame);
    }
    if (!track->ready()) {
      throw std::invalid_argument("Remuxer编码配置缺少有效参数集");
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
    throw std::invalid_argument("Remuxer需要非空编码包");
  }
  const auto stream = std::find_if(
      streams_.begin(), streams_.end(),
      [&](const auto& value) { return value.index == input->stream_index; });
  if (stream == streams_.end() || input->time_base.num <= 0 ||
      input->time_base.den <= 0 ||
      av_cmp_q(input->time_base, stream->time_base) != 0) {
    throw std::invalid_argument("Remuxer编码包轨道或时间基不匹配");
  }
  toolkit::Buffer::Ptr buffer = std::make_shared<PacketBuffer>(packet);
  if (stream->codec != mediakit::CodecAAC) {
    if (LengthPrefixed(stream->codec, *buffer, stream->nal_length_size) ||
        !mediakit::prefixSize(buffer->data(), buffer->size())) {
      if (!stream->nal_length_size) {
        throw std::invalid_argument(
            "视频包需要Annex B格式或编码配置中的NAL长度");
      }
      std::string annex_b;
      std::size_t offset = 0;
      while (offset < buffer->size()) {
        AppendNal(annex_b, stream->codec,
                  reinterpret_cast<const std::uint8_t*>(buffer->data()),
                  buffer->size(), offset, stream->nal_length_size);
      }
      buffer = std::make_shared<toolkit::BufferString>(std::move(annex_b));
    }
    ValidateAnnexB(stream->codec, buffer->data(), buffer->size());
  }
  auto frame = mediakit::Factory::getFrameFromBuffer(
      stream->codec, std::move(buffer), dts_ms, pts_ms);
  frame->setIndex(stream->index);
  if (pts_ms == 0 && dts_ms != 0) {
    frame = std::make_shared<ZeroPtsFrame>(frame);
  }
  return frame;
}

}  // namespace mw::streamer::internal
