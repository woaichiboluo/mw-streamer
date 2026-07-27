#include "mw/converter/av_packet_to_zlm_frame_converter.h"

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "Extension/Factory.h"
#include "Network/Buffer.h"
#include "ext-codec/AAC.h"

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
  static std::shared_ptr<AvPacketBuffer> Create(const AVPacket* source) {
    auto packet = av_packet_clone(source);
    if (!packet) {
      return nullptr;
    }
    return std::shared_ptr<AvPacketBuffer>(new AvPacketBuffer(packet));
  }

  ~AvPacketBuffer() override { av_packet_free(&packet_); }

  char* data() const override { return reinterpret_cast<char*>(packet_->data); }

  size_t size() const override { return static_cast<size_t>(packet_->size); }

 private:
  explicit AvPacketBuffer(AVPacket* packet) : packet_(packet) {}

  AVPacket* packet_;
};

bool HasValidTimestamp(const AVPacket* packet) {
  return packet->dts != AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE;
}

}  // namespace

AvPacketToZlmFrameConverter::AvPacketToZlmFrameConverter(
    const AVCodecParameters& codec_parameters, AVRational time_base,
    int stream_index)
    : codec_id_(GetZlmCodecId(codec_parameters.codec_id)),
      time_base_(time_base),
      stream_index_(stream_index) {
  if (codec_id_ == mediakit::CodecInvalid) {
    throw std::invalid_argument("不支持的FFmpeg codec");
  }
  if (time_base.num <= 0 || time_base.den <= 0) {
    throw std::invalid_argument("time_base必须为正数");
  }
  if (stream_index < 0) {
    throw std::invalid_argument("stream_index不能为负数");
  }
  if (codec_parameters.extradata_size < 0 ||
      (codec_parameters.extradata_size > 0 && !codec_parameters.extradata)) {
    throw std::invalid_argument("codec extradata无效");
  }
  if (codec_id_ == mediakit::CodecAAC && codec_parameters.extradata_size > 0) {
    aac_config_.assign(
        reinterpret_cast<const char*>(codec_parameters.extradata),
        static_cast<size_t>(codec_parameters.extradata_size));
  }
}

mediakit::Frame::Ptr AvPacketToZlmFrameConverter::Convert(
    const AVPacket* packet) const {
  if (!packet || !packet->data || packet->size <= 0 ||
      packet->stream_index != stream_index_ || !HasValidTimestamp(packet)) {
    return nullptr;
  }

  const auto dts = av_rescale_q(packet->dts, time_base_, kZlmTimeBase);
  const auto pts = av_rescale_q(packet->pts, time_base_, kZlmTimeBase);
  if (dts < 0 || pts < 0) {
    return nullptr;
  }

  toolkit::Buffer::Ptr buffer = AvPacketBuffer::Create(packet);
  if (!buffer) {
    return nullptr;
  }

  auto frame = mediakit::Factory::getFrameFromBuffer(
      codec_id_, buffer, static_cast<std::uint64_t>(dts),
      static_cast<std::uint64_t>(pts));
  if (!frame) {
    return nullptr;
  }

  if ((codec_id_ == mediakit::CodecH264 || codec_id_ == mediakit::CodecH265) &&
      frame->prefixSize() == 0) {
    return nullptr;
  }

  if (codec_id_ == mediakit::CodecAAC && frame->prefixSize() == 0) {
    if (aac_config_.empty()) {
      return nullptr;
    }

    std::uint8_t adts_header[32] = {};
    const auto header_size = mediakit::dumpAacConfig(
        aac_config_, buffer->size(), adts_header, sizeof(adts_header));
    if (header_size <= 0) {
      return nullptr;
    }

    auto aac_buffer = std::make_shared<toolkit::BufferLikeString>();
    aac_buffer->assign(reinterpret_cast<const char*>(adts_header),
                       static_cast<size_t>(header_size));
    aac_buffer->append(buffer->data(), buffer->size());
    frame = mediakit::Factory::getFrameFromBuffer(
        codec_id_, std::move(aac_buffer), static_cast<std::uint64_t>(dts),
        static_cast<std::uint64_t>(pts));
    if (!frame) {
      return nullptr;
    }
  }

  frame->setIndex(stream_index_);
  if (frame->getTrackType() == mediakit::TrackVideo &&
      (packet->flags & AV_PKT_FLAG_KEY) && !frame->keyFrame()) {
    return std::make_shared<mediakit::FrameCacheAble>(frame, true);
  }
  return frame;
}

}  // namespace mw::streamer::converter
