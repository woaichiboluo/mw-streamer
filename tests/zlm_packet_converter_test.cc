#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "Extension/Factory.h"
#include "Record/MP4Demuxer.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"
#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"

namespace {

using mediakit::CodecAAC;
using mediakit::CodecH264;
using mediakit::CodecH265;
using mediakit::CodecId;
using mediakit::CodecVP8;
using mediakit::CodecVP9;
using mediakit::Frame;
using mediakit::FrameWriterInterface;
using mediakit::MP4Demuxer;
using mediakit::Track;
using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::ffmpeg::CodecContext;
using mw::streamer::ffmpeg::CodecParameters;
using FfmpegFrame = mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::Packet;

const AVCodec* FindDecoder(const CodecParameters& parameters) {
  REQUIRE(parameters.get());
  const auto* codec = avcodec_find_decoder(parameters.get()->codec_id);
  REQUIRE(codec);
  return codec;
}

AVCodecID ExpectedCodecId(CodecId codec) {
  switch (codec) {
    case CodecH264:
      return AV_CODEC_ID_H264;
    case CodecH265:
      return AV_CODEC_ID_HEVC;
    case CodecAAC:
      return AV_CODEC_ID_AAC;
    case CodecVP8:
      return AV_CODEC_ID_VP8;
    case CodecVP9:
      return AV_CODEC_ID_VP9;
    default:
      return AV_CODEC_ID_NONE;
  }
}

class TestDecoder {
 public:
  TestDecoder(const CodecParameters& parameters, AVRational time_base)
      : context_(FindDecoder(parameters)) {
    REQUIRE(avcodec_parameters_to_context(context_.get(), parameters.get()) >=
            0);
    context_.get()->pkt_timebase = time_base;
    context_.get()->thread_count = 1;
    REQUIRE(avcodec_open2(context_.get(), context_.get()->codec, nullptr) >= 0);
  }

  bool InputPacket(const Packet& packet) {
    for (;;) {
      const auto result = avcodec_send_packet(context_.get(), packet.get());
      if (result == AVERROR(EAGAIN)) {
        if (!ReceiveFrames()) {
          return false;
        }
        continue;
      }
      return result >= 0 && ReceiveFrames();
    }
  }

  bool Flush() {
    for (;;) {
      const auto result = avcodec_send_packet(context_.get(), nullptr);
      if (result == AVERROR(EAGAIN)) {
        if (!ReceiveFrames()) {
          return false;
        }
        continue;
      }
      return (result >= 0 || result == AVERROR_EOF) && ReceiveFrames();
    }
  }

  void Reset() { context_.FlushBuffers(); }

  std::size_t decoded_frame_count() const { return decoded_frame_count_; }

 private:
  bool ReceiveFrames() {
    for (;;) {
      frame_.Unref();
      const auto result = avcodec_receive_frame(context_.get(), frame_.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return true;
      }
      if (result < 0) {
        return false;
      }
      ++decoded_frame_count_;
    }
  }

  CodecContext context_;
  FfmpegFrame frame_;
  std::size_t decoded_frame_count_ = 0;
};

bool StartsWithAnnexB(const Packet& packet) {
  if (packet->size >= 4 && packet->data[0] == 0 && packet->data[1] == 0 &&
      packet->data[2] == 0 && packet->data[3] == 1) {
    return true;
  }
  return packet->size >= 3 && packet->data[0] == 0 && packet->data[1] == 0 &&
         packet->data[2] == 1;
}

bool HasZeroPadding(const Packet& packet) {
  for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; ++i) {
    if (packet->data[packet->size + i] != 0) {
      return false;
    }
  }
  return true;
}

std::string SamplePath(const std::string& name) {
  return std::string(MW_ZLM_PACKET_CONVERTER_TEST_DATA_DIR) + "/" + name;
}

struct Binding {
  CodecId codec = mediakit::CodecInvalid;
  int stream_index = -1;
  bool valid_packets = true;
  std::size_t packet_count = 0;
  std::size_t key_packet_count = 0;
  std::optional<Packet> retained_packet;
  std::vector<std::uint8_t> retained_payload;
  Track::Ptr track;
  FrameWriterInterface* delegate = nullptr;
  ZlmCodecParametersConverter::Ptr codec_parameters_converter;
  ZlmPacketConverter::Ptr packet_converter;
  std::unique_ptr<TestDecoder> decoder;
};

std::shared_ptr<Binding> MakeBinding(const Track::Ptr& track,
                                     int stream_index) {
  auto binding = std::make_shared<Binding>();
  binding->codec = track->getCodecId();
  binding->stream_index = stream_index;
  binding->track = track;
  binding->codec_parameters_converter =
      std::make_shared<ZlmCodecParametersConverter>(track);
  binding->packet_converter =
      std::make_shared<ZlmPacketConverter>(track, stream_index);
  binding->decoder = std::make_unique<TestDecoder>(
      binding->codec_parameters_converter->codec_parameters(),
      binding->codec_parameters_converter->time_base());

  std::weak_ptr<Binding> weak_binding = binding;
  binding->packet_converter->SetOnPacket([weak_binding](const Packet& packet) {
    auto binding = weak_binding.lock();
    if (!binding) {
      return false;
    }
    if (!packet.get() || !packet->buf || !packet->data || packet->size <= 0 ||
        packet->stream_index != binding->stream_index ||
        packet->time_base.num != 1 || packet->time_base.den != 1000 ||
        packet->dts == AV_NOPTS_VALUE || packet->pts == AV_NOPTS_VALUE ||
        !HasZeroPadding(packet)) {
      binding->valid_packets = false;
      return false;
    }

    if ((binding->codec == CodecH264 || binding->codec == CodecH265) &&
        !StartsWithAnnexB(packet)) {
      binding->valid_packets = false;
      return false;
    }

    ++binding->packet_count;
    if (packet->flags & AV_PKT_FLAG_KEY) {
      ++binding->key_packet_count;
    }
    if (!binding->retained_packet) {
      binding->retained_payload.assign(packet->data,
                                       packet->data + packet->size);
      binding->retained_packet = packet;
    }
    return binding->decoder->InputPacket(packet);
  });
  return binding;
}

struct Mp4Sample {
  const char* file;
  CodecId video_codec;
  bool has_audio;
  bool verify_reset;
};

const std::vector<Mp4Sample> kMp4Samples = {
    {"h264_aac.mp4", CodecH264, true, false},
    {"h265_aac.mp4", CodecH265, true, false},
    {"h264_video.mp4", CodecH264, false, true},
    {"h265_video.mp4", CodecH265, false, false},
    {"h264_aac_fragmented.mp4", CodecH264, true, false},
    {"h265_aac_fragmented.mp4", CodecH265, true, false},
};

std::uint32_t ReadLittleEndian32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t ReadLittleEndian64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | data[i];
  }
  return value;
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct IvfSample {
  const char* file;
  const char* fourcc;
  CodecId codec;
};

const std::vector<IvfSample> kIvfSamples = {
    {"vp8_video.ivf", "VP80", CodecVP8},
    {"vp9_video.ivf", "VP90", CodecVP9},
};

}  // namespace

TEST_CASE("ZLM MP4 frames become owned decodable AVPackets") {
  for (const auto& sample : kMp4Samples) {
    DYNAMIC_SECTION(sample.file) {
      MP4Demuxer demuxer;
      demuxer.openMP4(SamplePath(sample.file));

      auto tracks = demuxer.getTracks(false);
      REQUIRE(tracks.size() == (sample.has_audio ? 2 : 1));

      std::map<int, std::shared_ptr<Binding>> bindings;
      bool found_video = false;
      bool found_audio = false;
      int stream_index = 0;
      for (const auto& track : tracks) {
        auto binding = MakeBinding(track, stream_index++);
        const auto& parameters =
            binding->codec_parameters_converter->codec_parameters();
        REQUIRE(parameters.get());
        CHECK(parameters.get()->codec_id == ExpectedCodecId(binding->codec));
        CHECK(parameters.get()->extradata_size > 0);
        if (track->getTrackType() == mediakit::TrackVideo) {
          CHECK(parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO);
          CHECK(parameters.get()->width > 0);
          CHECK(parameters.get()->height > 0);
        } else {
          CHECK(parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO);
          CHECK(parameters.get()->sample_rate > 0);
          CHECK(parameters.get()->ch_layout.nb_channels > 0);
        }
        found_video |= binding->codec == sample.video_codec;
        found_audio |= binding->codec == CodecAAC;
        bindings.emplace(track->getIndex(), binding);
        binding->delegate =
            track->addDelegate([packet_converter = binding->packet_converter](
                                   const Frame::Ptr& frame) {
              return packet_converter->InputFrame(frame);
            });
      }

      REQUIRE(found_video);
      REQUIRE(found_audio == sample.has_audio);

      bool eof = false;
      bool reset_done = false;
      std::size_t video_input_count = 0;
      while (!eof) {
        bool key_frame = false;
        int error = 0;
        auto frame = demuxer.readFrame(key_frame, eof, &error);
        REQUIRE(error == 0);
        if (frame && sample.verify_reset && frame->getCodecId() == CodecH264) {
          ++video_input_count;
          if (video_input_count == 10) {
            auto& binding = bindings.at(frame->getIndex());
            REQUIRE(binding->packet_converter->Flush());
            binding->decoder->Flush();
            binding->packet_converter->Reset();
            binding->decoder->Reset();
            reset_done = true;
          }
        }
      }
      CHECK(reset_done == sample.verify_reset);

      for (const auto& entry : bindings) {
        auto& binding = entry.second;
        REQUIRE(binding->packet_converter->Flush());
        REQUIRE(binding->decoder->Flush());
        CHECK(binding->valid_packets);
        CHECK(binding->packet_count > 0);
        CHECK(binding->decoder->decoded_frame_count() > 0);
        if (binding->codec == sample.video_codec) {
          CHECK(binding->packet_count == 20);
          CHECK(binding->key_packet_count > 0);
          CHECK(binding->decoder->decoded_frame_count() == 20);
          REQUIRE(binding->retained_packet);
          CHECK(binding->retained_packet->get()->flags & AV_PKT_FLAG_KEY);
        }
      }

      for (const auto& entry : bindings) {
        auto& binding = entry.second;
        if (binding->delegate) {
          binding->track->delDelegate(binding->delegate);
          binding->delegate = nullptr;
        }
      }
      demuxer.closeMP4();
      for (const auto& entry : bindings) {
        auto& binding = entry.second;
        binding->packet_converter.reset();
        REQUIRE(binding->retained_packet);
        CHECK(binding->retained_packet->get()->buf);
        CHECK(binding->retained_payload ==
              std::vector<std::uint8_t>(
                  binding->retained_packet->get()->data,
                  binding->retained_packet->get()->data +
                      binding->retained_packet->get()->size));
      }
    }
  }
}

TEST_CASE("VP8 and VP9 packets preserve payload and decode through callbacks") {
  for (const auto& sample : kIvfSamples) {
    DYNAMIC_SECTION(sample.file) {
      auto file = ReadFile(SamplePath(sample.file));
      REQUIRE(file.size() >= 32);
      REQUIRE(std::equal(file.begin(), file.begin() + 4, "DKIF"));
      REQUIRE(std::equal(file.begin() + 8, file.begin() + 12, sample.fourcc));

      const auto rate = ReadLittleEndian32(file.data() + 16);
      const auto scale = ReadLittleEndian32(file.data() + 20);
      REQUIRE(rate > 0);
      REQUIRE(scale > 0);

      auto track = mediakit::Factory::getTrackByCodecId(sample.codec);
      REQUIRE(track);
      auto binding = MakeBinding(track, 0);
      const auto& parameters =
          binding->codec_parameters_converter->codec_parameters();
      REQUIRE(parameters.get());
      CHECK(parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO);
      CHECK(parameters.get()->codec_id == ExpectedCodecId(sample.codec));

      const std::vector<std::uint8_t>* expected_payload = nullptr;
      std::weak_ptr<Binding> weak_binding = binding;
      binding->packet_converter->SetOnPacket(
          [weak_binding, &expected_payload](const Packet& packet) {
            auto binding = weak_binding.lock();
            if (!binding) {
              return false;
            }
            if (!packet.get() || !packet->buf || !expected_payload ||
                packet->size != static_cast<int>(expected_payload->size()) ||
                !std::equal(packet->data, packet->data + packet->size,
                            expected_payload->begin()) ||
                packet->time_base.num != 1 || packet->time_base.den != 1000 ||
                !HasZeroPadding(packet)) {
              binding->valid_packets = false;
              return false;
            }
            ++binding->packet_count;
            if (packet->flags & AV_PKT_FLAG_KEY) {
              ++binding->key_packet_count;
            }
            if (!binding->retained_packet) {
              binding->retained_payload.assign(packet->data,
                                               packet->data + packet->size);
              binding->retained_packet = packet;
            }
            return binding->decoder->InputPacket(packet);
          });

      std::size_t offset = 32;
      while (offset < file.size()) {
        REQUIRE(file.size() - offset >= 12);
        const auto frame_size = ReadLittleEndian32(file.data() + offset);
        const auto timestamp = ReadLittleEndian64(file.data() + offset + 4);
        offset += 12;
        REQUIRE(file.size() - offset >= frame_size);

        std::vector<std::uint8_t> payload(file.begin() + offset,
                                          file.begin() + offset + frame_size);
        expected_payload = &payload;
        const auto timestamp_ms = timestamp * scale * 1000 / rate;
        auto frame = mediakit::Factory::getFrameFromPtr(
            sample.codec, reinterpret_cast<const char*>(payload.data()),
            payload.size(), timestamp_ms, timestamp_ms);
        REQUIRE(frame);
        REQUIRE(binding->packet_converter->InputFrame(frame));
        offset += frame_size;
      }

      REQUIRE(offset == file.size());
      REQUIRE(binding->packet_converter->Flush());
      REQUIRE(binding->decoder->Flush());

      CHECK(binding->valid_packets);
      CHECK(binding->packet_count == 10);
      CHECK(binding->key_packet_count > 0);
      CHECK(binding->decoder->decoded_frame_count() == 10);

      binding->packet_converter.reset();
      REQUIRE(binding->retained_packet);
      CHECK(binding->retained_packet->get()->buf);
      CHECK(
          binding->retained_payload ==
          std::vector<std::uint8_t>(binding->retained_packet->get()->data,
                                    binding->retained_packet->get()->data +
                                        binding->retained_packet->get()->size));
    }
  }
}

TEST_CASE("H264复合AU按NAL拆分后保留关键帧语义") {
  auto track = mediakit::Factory::getTrackByCodecId(CodecH264);
  REQUIRE(track);

  ZlmPacketConverter converter(track, 0);
  std::vector<Packet> packets;
  converter.SetOnPacket([&packets](const Packet& packet) {
    packets.push_back(packet.Clone());
    return true;
  });

  std::vector<std::uint8_t> key_access_unit = {
      0x00, 0x00, 0x00, 0x01, 0x09, 0xF0, 0x00, 0x00, 0x00, 0x01, 0x65, 0x80,
  };
  auto key_frame = mediakit::Factory::getFrameFromPtr(
      CodecH264, reinterpret_cast<const char*>(key_access_unit.data()),
      key_access_unit.size(), 0, 0);
  REQUIRE(key_frame);
  REQUIRE_FALSE(key_frame->keyFrame());
  REQUIRE(converter.InputFrame(key_frame));

  std::vector<std::uint8_t> predicted_access_unit = {
      0x00, 0x00, 0x00, 0x01, 0x09, 0xF0, 0x00, 0x00, 0x00, 0x01, 0x41, 0x80,
  };
  auto predicted_frame = mediakit::Factory::getFrameFromPtr(
      CodecH264, reinterpret_cast<const char*>(predicted_access_unit.data()),
      predicted_access_unit.size(), 40, 40);
  REQUIRE(predicted_frame);
  REQUIRE(converter.InputFrame(predicted_frame));
  REQUIRE(converter.Flush());

  REQUIRE(packets.size() == 2);
  CHECK(packets[0].get()->flags & AV_PKT_FLAG_KEY);
  CHECK_FALSE(packets[1].get()->flags & AV_PKT_FLAG_KEY);
  CHECK(std::vector<std::uint8_t>(
            packets[0].get()->data,
            packets[0].get()->data + packets[0].get()->size) ==
        key_access_unit);
}
