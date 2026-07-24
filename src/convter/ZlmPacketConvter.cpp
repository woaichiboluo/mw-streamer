#include "mw/convter/ZlmPacketConvter.h"

#include <climits>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace mw::convter {
namespace {

constexpr AVRational kZlmTimeBase{1, 1000};

struct AVPacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

}  // namespace

ZlmPacketConvter::ZlmPacketConvter(const mediakit::Track::Ptr& track,
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

void ZlmPacketConvter::setOnPacket(OnPacket callback) {
  on_packet_ = std::move(callback);
}

bool ZlmPacketConvter::inputFrame(const mediakit::Frame::Ptr& frame) {
  if (!on_packet_ || !frame || frame->getCodecId() != codec_id_) {
    return false;
  }

  if (codec_id_ != mediakit::CodecH264 &&
      codec_id_ != mediakit::CodecH265) {
    return emitPacket(frame->data(), frame->size(), frame->dts(),
                      frame->pts(), frame->keyFrame());
  }

  last_output_result_ = true;
  auto on_merged = [this](std::uint64_t dts, std::uint64_t pts,
                          const toolkit::Buffer::Ptr& buffer,
                          bool key_frame) {
    last_output_result_ =
        emitPacket(buffer->data(), buffer->size(), dts, pts, key_frame);
  };

  if (!initial_config_sent_) {
    initial_config_sent_ = true;
    for (const auto& config : initial_config_frames_) {
      auto stamped = std::make_shared<mediakit::FrameStamp>(config);
      stamped->setStamp(frame->dts(), frame->pts());
      if (!merger_.inputFrame(stamped, on_merged) ||
          !last_output_result_) {
        return false;
      }
    }
  }

  const auto accepted = merger_.inputFrame(frame, std::move(on_merged));
  return accepted && last_output_result_;
}

bool ZlmPacketConvter::flush() {
  last_output_result_ = true;
  merger_.flush();
  return last_output_result_;
}

void ZlmPacketConvter::reset() {
  merger_.clear();
  initial_config_sent_ = false;
  last_output_result_ = true;
}

bool ZlmPacketConvter::emitPacket(const char* data, size_t size,
                                  std::uint64_t dts, std::uint64_t pts,
                                  bool key_frame) {
  if (!on_packet_ || !data || !size || size > INT_MAX) {
    return false;
  }

  std::unique_ptr<AVPacket, AVPacketDeleter> packet(av_packet_alloc());
  if (!packet || av_new_packet(packet.get(), static_cast<int>(size)) < 0) {
    return false;
  }

  std::memcpy(packet->data, data, size);
  packet->dts = static_cast<std::int64_t>(dts);
  packet->pts = static_cast<std::int64_t>(pts);
  packet->stream_index = stream_index_;
  packet->time_base = kZlmTimeBase;
  packet->pos = -1;
  if (key_frame) {
    packet->flags |= AV_PKT_FLAG_KEY;
  }

  return on_packet_(packet.get());
}

}  // namespace mw::convter
