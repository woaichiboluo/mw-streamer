#ifndef MW_STREAMER_TESTS_ENCODER_ENCODER_TEST_SUPPORT_H_
#define MW_STREAMER_TESTS_ENCODER_ENCODER_TEST_SUPPORT_H_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mw/streamer/encoder/av_encoder.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace encoder_test {

namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kVideoTimeBase{1, 50};
constexpr AVRational kAudioTimeBase{1, 48000};
constexpr std::int64_t kVideoStart = 500;
constexpr std::int64_t kAudioStart = 480000;

inline mw::streamer::MediaFrame Video(ffmpeg::Frame frame) {
  return {std::move(frame), std::nullopt};
}
inline mw::streamer::MediaFrame Audio(ffmpeg::Frame frame) {
  return {std::nullopt, std::move(frame)};
}
inline mw::streamer::MediaFrame Frame(ffmpeg::Frame video,
                                      ffmpeg::Frame audio) {
  return {std::move(video), std::move(audio)};
}

inline mw::streamer::AvEncoderConfig Config() {
  mw::streamer::AvEncoderConfig config;
  config.fps = {50, 1};
  config.video_options = {{"preset", "ultrafast"},
                          {"tune", "zerolatency"},
                          {"threads", "1"},
                          {"crf", "18"}};
  return config;
}

inline ffmpeg::StreamInfo VideoStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 4;
  stream.time_base = kVideoTimeBase;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->width = 64;
  parameters->height = 64;
  parameters->sample_aspect_ratio = {1, 1};
  return stream;
}

inline ffmpeg::StreamInfo AudioStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 7;
  stream.time_base = kAudioTimeBase;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_AAC;
  parameters->format = AV_SAMPLE_FMT_FLTP;
  parameters->sample_rate = 48000;
  parameters->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  return stream;
}

inline ffmpeg::Frame VideoFrame(int index) {
  ffmpeg::Frame frame;
  frame->width = 64;
  frame->height = 64;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->time_base = kVideoTimeBase;
  frame->pts = kVideoStart + index;
  frame->duration = 1;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配视频测试帧");
  for (int plane = 0; plane < 3; ++plane) {
    const int size = plane == 0 ? 64 : 32;
    const int value = plane == 0 ? 20 + index * 20 : 128;
    for (int row = 0; row < size; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane], value,
                  static_cast<std::size_t>(size));
    }
  }
  return frame;
}

inline ffmpeg::Frame AudioFrame(int samples, std::int64_t start = kAudioStart,
                                int leading_samples = 0) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  frame->time_base = kAudioTimeBase;
  frame->pts = start;
  frame->duration = samples;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配音频测试帧");
  for (int channel = 0; channel < 2; ++channel) {
    auto* data = reinterpret_cast<float*>(frame->extended_data[channel]);
    for (int sample = 0; sample < samples; ++sample) {
      data[sample] = sample < leading_samples ? 0.75F : 0.1F;
    }
  }
  return frame;
}

class Gate final {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    wake_.notify_all();
    wake_.wait(lock, [&] { return released_; });
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return wake_.wait_for(lock, std::chrono::seconds(10),
                          [&] { return entered_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    wake_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool entered_ = false;
  bool released_ = false;
};

struct ReleaseGate final {
  Gate& gate;
  ~ReleaseGate() { gate.Release(); }
};

struct Capture final {
  std::mutex mutex;
  std::condition_variable wake;
  int ready = 0;
  int ended = 0;
  int errors = 0;
  std::string callback_error;
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;

  void Bind(mw::streamer::AvEncoder& encoder, Gate* first_packet = nullptr) {
    encoder.SetOnReady([this](const auto& value) noexcept {
      try {
        std::lock_guard lock(mutex);
        ++ready;
        streams = value;
      } catch (...) {
        RecordCallbackFailure();
      }
    });
    encoder.SetOnPacket([this, first_packet](const auto& value) noexcept {
      bool first = false;
      try {
        std::lock_guard lock(mutex);
        first = packets.empty();
        packets.push_back(value.Ref());
        wake.notify_all();
      } catch (...) {
        RecordCallbackFailure();
      }
      if (first && first_packet) first_packet->Enter();
    });
    encoder.SetOnEnded([this]() noexcept {
      std::lock_guard lock(mutex);
      ++ended;
      wake.notify_all();
    });
    encoder.SetOnError([this](int, std::string_view) noexcept {
      std::lock_guard lock(mutex);
      ++errors;
      wake.notify_all();
    });
  }
  bool Wait() {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, std::chrono::seconds(10),
                         [&] { return ended || errors; });
  }
  bool WaitPackets(std::size_t count) {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, std::chrono::seconds(10),
                         [&] { return packets.size() >= count || errors; });
  }
  void RecordCallbackFailure() noexcept {
    std::lock_guard lock(mutex);
    callback_error = "callback failed";
  }
};

inline ffmpeg::CodecContext CreateDecoder(const ffmpeg::StreamInfo& stream,
                                          AVBufferRef* device = nullptr) {
  const auto* codec =
      avcodec_find_decoder(stream.codec_parameters.get()->codec_id);
  if (!codec) throw std::runtime_error("找不到测试解码器");
  ffmpeg::CodecContext decoder(codec);
  auto* context = decoder.get();
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_to_context(context, stream.codec_parameters.get()),
      "配置测试解码器");
  context->pkt_timebase = stream.time_base;
  if (device) {
    context->hw_device_ctx = av_buffer_ref(device);
    if (!context->hw_device_ctx) throw std::bad_alloc();
    context->get_format = [](AVCodecContext*, const AVPixelFormat* formats) {
      for (auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_CUDA) return *format;
      }
      return AV_PIX_FMT_NONE;
    };
  }
  ffmpeg::FfmpegException::throwIfError(avcodec_open2(context, codec, nullptr),
                                        "打开测试解码器");
  return decoder;
}

inline std::vector<ffmpeg::Frame> ReceiveFrames(ffmpeg::CodecContext& decoder) {
  std::vector<ffmpeg::Frame> frames;
  for (;;) {
    ffmpeg::Frame frame;
    const int result = avcodec_receive_frame(decoder.get(), frame.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(result, "接收测试解码帧");
    frame->time_base = decoder.get()->pkt_timebase;
    frames.push_back(std::move(frame));
  }
  return frames;
}

inline std::vector<ffmpeg::Frame> Decode(
    ffmpeg::CodecContext& decoder, const std::vector<ffmpeg::Packet>& packets,
    int stream_index) {
  std::vector<ffmpeg::Frame> frames;
  const auto receive = [&] {
    for (auto& frame : ReceiveFrames(decoder)) {
      frames.push_back(std::move(frame));
    }
  };
  const auto send = [&](const AVPacket* packet) {
    for (;;) {
      const int result = avcodec_send_packet(decoder.get(), packet);
      if (result == AVERROR(EAGAIN)) {
        receive();
        continue;
      }
      ffmpeg::FfmpegException::throwIfError(result, "提交测试解码包");
      break;
    }
    receive();
  };
  for (const auto& packet : packets) {
    if (packet->stream_index == stream_index) send(packet.get());
  }
  send(nullptr);
  return frames;
}

}  // namespace encoder_test

#endif  // MW_STREAMER_TESTS_ENCODER_ENCODER_TEST_SUPPORT_H_
