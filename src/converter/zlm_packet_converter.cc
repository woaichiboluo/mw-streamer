#include "mw/converter/zlm_packet_converter.h"

#include <climits>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "ext-codec/H264.h"
#include "ext-codec/H265.h"

namespace mw::streamer::converter {
namespace {

constexpr AVRational kZlmTimeBase{1, 1000};

}  // namespace

ZlmPacketConverter::ZlmPacketConverter(const mediakit::Track::Ptr& track,
                                       int stream_index)
    : codec_id_(track ? track->getCodecId() : mediakit::CodecInvalid),
      stream_index_(stream_index) {
  if (!track) {
    throw std::invalid_argument("track不能为空");
  }
  if (stream_index < 0) {
    throw std::invalid_argument("stream_index不能为负数");
  }
  if (track->getTrackType() == mediakit::TrackVideo) {
    initial_config_frames_ =
        std::static_pointer_cast<mediakit::VideoTrack>(track)
            ->getConfigFrames();
  }
}

void ZlmPacketConverter::SetOnPacket(OnPacket callback) {
  on_packet_ = std::move(callback);
}

bool ZlmPacketConverter::InputFrame(const mediakit::Frame::Ptr& frame) {
  if (!on_packet_ || !frame || frame->getCodecId() != codec_id_) {
    return false;
  }

  if (codec_id_ != mediakit::CodecH264 && codec_id_ != mediakit::CodecH265) {
    return EmitPacket(frame->data(), frame->size(), frame->dts(), frame->pts(),
                      frame->keyFrame());
  }

  last_output_result_ = true;
  auto on_merged = [this](std::uint64_t dts, std::uint64_t pts,
                          const toolkit::Buffer::Ptr& buffer, bool key_frame) {
    last_output_result_ =
        EmitPacket(buffer->data(), buffer->size(), dts, pts, key_frame);
  };

  if (!initial_config_sent_) {
    initial_config_sent_ = true;
    for (const auto& config : initial_config_frames_) {
      auto stamped = std::make_shared<mediakit::FrameStamp>(config);
      stamped->setStamp(frame->dts(), frame->pts());
      if (!merger_.inputFrame(stamped, on_merged) || !last_output_result_) {
        return false;
      }
    }
  }

  bool accepted = true;
  bool found_nal = false;
  mediakit::splitH264(
      frame->data(), frame->size(), frame->prefixSize(),
      [&](const char* data, size_t size, size_t prefix_size) {
        found_nal = true;
        if (!accepted || size <= prefix_size) {
          accepted = false;
          return;
        }

        mediakit::Frame::Ptr nal;
        if (codec_id_ == mediakit::CodecH264) {
          using H264Frame =
              mediakit::FrameInternal<mediakit::H264FrameNoCacheAble>;
          nal = std::make_shared<H264Frame>(frame, const_cast<char*>(data),
                                            size, prefix_size);
        } else {
          using H265Frame =
              mediakit::FrameInternal<mediakit::H265FrameNoCacheAble>;
          nal = std::make_shared<H265Frame>(frame, const_cast<char*>(data),
                                            size, prefix_size);
        }

        accepted = merger_.inputFrame(nal, on_merged) && last_output_result_;
      });
  return found_nal && accepted;
}

bool ZlmPacketConverter::Flush() {
  last_output_result_ = true;
  merger_.flush();
  return last_output_result_;
}

void ZlmPacketConverter::Reset() {
  merger_.clear();
  initial_config_sent_ = false;
  last_output_result_ = true;
}

bool ZlmPacketConverter::EmitPacket(const char* data, size_t size,
                                    std::uint64_t dts, std::uint64_t pts,
                                    bool key_frame) {
  if (!on_packet_ || !data || !size || size > INT_MAX) {
    return false;
  }

  ffmpeg::Packet packet;
  if (av_new_packet(packet.get(), static_cast<int>(size)) < 0) {
    return false;
  }

  std::memcpy(packet.get()->data, data, size);
  packet.get()->dts = static_cast<std::int64_t>(dts);
  packet.get()->pts = static_cast<std::int64_t>(pts);
  packet.get()->stream_index = stream_index_;
  packet.get()->time_base = kZlmTimeBase;
  packet.get()->pos = -1;
  if (key_frame) {
    packet.get()->flags |= AV_PKT_FLAG_KEY;
  }

  return on_packet_(packet);
}

}  // namespace mw::streamer::converter
