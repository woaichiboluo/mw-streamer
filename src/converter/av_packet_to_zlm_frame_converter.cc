#include "mw/converter/av_packet_to_zlm_frame_converter.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Extension/Factory.h"
#include "Network/Buffer.h"
#include "ext-codec/AAC.h"
#include "ext-codec/H264.h"
#include "ext-codec/H265.h"

namespace mw::streamer::converter {
namespace {

constexpr AVRational kZlmTimeBase{1, 1000};

mediakit::CodecId GetZlmCodecId(AVCodecID codec_id) {
  switch (codec_id) {
    case AV_CODEC_ID_H264:
      return mediakit::CodecH264;
    case AV_CODEC_ID_HEVC:
      return mediakit::CodecH265;
    case AV_CODEC_ID_AAC:
      return mediakit::CodecAAC;
    case AV_CODEC_ID_PCM_ALAW:
      return mediakit::CodecG711A;
    case AV_CODEC_ID_PCM_MULAW:
      return mediakit::CodecG711U;
    case AV_CODEC_ID_OPUS:
      return mediakit::CodecOpus;
    case AV_CODEC_ID_MJPEG:
      return mediakit::CodecJPEG;
    case AV_CODEC_ID_VP8:
      return mediakit::CodecVP8;
    case AV_CODEC_ID_VP9:
      return mediakit::CodecVP9;
    default:
      return mediakit::CodecInvalid;
  }
}

class AvPacketBuffer final : public toolkit::Buffer {
 public:
  static std::shared_ptr<AvPacketBuffer> Create(ffmpeg::Packet packet) {
    try {
      return std::shared_ptr<AvPacketBuffer>(
          new AvPacketBuffer(std::move(packet)));
    } catch (const std::bad_alloc&) {
      return nullptr;
    }
  }

  char* data() const override {
    return reinterpret_cast<char*>(packet_.get()->data);
  }

  size_t size() const override {
    return static_cast<size_t>(packet_.get()->size);
  }

 private:
  explicit AvPacketBuffer(ffmpeg::Packet packet) : packet_(std::move(packet)) {}

  ffmpeg::Packet packet_;
};

bool HasValidTimestamp(const AVPacket& packet) {
  return packet.dts != AV_NOPTS_VALUE && packet.pts != AV_NOPTS_VALUE;
}

}  // namespace

AvPacketToZlmFrameConverter::AvPacketToZlmFrameConverter(
    const ffmpeg::CodecParameters& codec_parameters, AVRational time_base,
    int stream_index)
    : codec_id_(GetZlmCodecId(codec_parameters.get()->codec_id)),
      time_base_(time_base),
      stream_index_(stream_index) {
  const auto& parameters = *codec_parameters.get();
  if (codec_id_ == mediakit::CodecInvalid) {
    throw std::invalid_argument("不支持的FFmpeg codec");
  }
  if (time_base.num <= 0 || time_base.den <= 0) {
    throw std::invalid_argument("time_base必须为正数");
  }
  if (stream_index < 0) {
    throw std::invalid_argument("stream_index不能为负数");
  }
  if (parameters.extradata_size < 0 ||
      (parameters.extradata_size > 0 && !parameters.extradata)) {
    throw std::invalid_argument("codec extradata无效");
  }
  if (codec_id_ == mediakit::CodecAAC && parameters.extradata_size > 0) {
    aac_config_.assign(reinterpret_cast<const char*>(parameters.extradata),
                       static_cast<size_t>(parameters.extradata_size));
  }
}

std::vector<mediakit::Frame::Ptr> AvPacketToZlmFrameConverter::Convert(
    ffmpeg::Packet packet) const {
  const auto* raw_packet = packet.get();
  if (!raw_packet || !raw_packet->data || raw_packet->size <= 0 ||
      raw_packet->stream_index != stream_index_ ||
      !HasValidTimestamp(*raw_packet)) {
    return {};
  }

  const auto dts = av_rescale_q(raw_packet->dts, time_base_, kZlmTimeBase);
  const auto pts = av_rescale_q(raw_packet->pts, time_base_, kZlmTimeBase);
  if (dts < 0 || pts < 0) {
    return {};
  }

  toolkit::Buffer::Ptr buffer = AvPacketBuffer::Create(std::move(packet));
  if (!buffer) {
    return {};
  }

  auto frame = mediakit::Factory::getFrameFromBuffer(
      codec_id_, buffer, static_cast<std::uint64_t>(dts),
      static_cast<std::uint64_t>(pts));
  if (!frame) {
    return {};
  }

  if ((codec_id_ == mediakit::CodecH264 || codec_id_ == mediakit::CodecH265) &&
      frame->prefixSize() == 0) {
    return {};
  }

  if (codec_id_ == mediakit::CodecAAC && frame->prefixSize() == 0) {
    if (aac_config_.empty()) {
      return {};
    }

    std::uint8_t adts_header[32] = {};
    const auto header_size = mediakit::dumpAacConfig(
        aac_config_, buffer->size(), adts_header, sizeof(adts_header));
    if (header_size <= 0) {
      return {};
    }

    auto aac_buffer = std::make_shared<toolkit::BufferLikeString>();
    aac_buffer->assign(reinterpret_cast<const char*>(adts_header),
                       static_cast<size_t>(header_size));
    aac_buffer->append(buffer->data(), buffer->size());
    frame = mediakit::Factory::getFrameFromBuffer(
        codec_id_, std::move(aac_buffer), static_cast<std::uint64_t>(dts),
        static_cast<std::uint64_t>(pts));
    if (!frame) {
      return {};
    }
  }

  frame->setIndex(stream_index_);
  if (codec_id_ != mediakit::CodecH264 && codec_id_ != mediakit::CodecH265) {
    return {std::move(frame)};
  }

  std::vector<mediakit::Frame::Ptr> frames;
  bool valid = true;
  mediakit::splitH264(
      frame->data(), frame->size(), frame->prefixSize(),
      [&](const char* data, size_t size, size_t prefix_size) {
        if (size <= prefix_size) {
          valid = false;
          return;
        }
        if (codec_id_ == mediakit::CodecH264) {
          using H264Frame =
              mediakit::FrameInternal<mediakit::H264FrameNoCacheAble>;
          frames.push_back(std::make_shared<H264Frame>(
              frame, const_cast<char*>(data), size, prefix_size));
          return;
        }

        using H265Frame =
            mediakit::FrameInternal<mediakit::H265FrameNoCacheAble>;
        frames.push_back(std::make_shared<H265Frame>(
            frame, const_cast<char*>(data), size, prefix_size));
      });
  if (!valid) {
    return {};
  }
  return frames;
}

}  // namespace mw::streamer::converter
