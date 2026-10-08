#ifndef MW_STREAMER_TESTS_ENCODER_ENCODER_TEST_SUPPORT_H_
#define MW_STREAMER_TESTS_ENCODER_ENCODER_TEST_SUPPORT_H_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "mw/streamer/encoder/encoder.h"
#include "mw/streamer/ffmpeg/decoder.h"
#include "mw/streamer/ffmpeg/error.h"

namespace encoder_test {

namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};
constexpr std::int64_t kStart = 10000000000;
constexpr std::int64_t kFrameInterval = 20000000;

inline mw::streamer::EncoderConfig Config() {
  mw::streamer::EncoderConfig config;
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
  stream.time_base = kNanoseconds;
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
  stream.time_base = kNanoseconds;
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
  frame->time_base = kNanoseconds;
  frame->pts = kStart + index * kFrameInterval;
  frame->duration = kFrameInterval;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配视频测试帧");
  for (int plane = 0; plane < 3; ++plane) {
    const int size = plane == 0 ? 64 : 32;
    const int value = plane == 0 ? 20 + index * 20 : 128;
    for (int row = 0; row < size; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane], value,
                  size);
    }
  }
  return frame;
}

inline ffmpeg::Frame AudioFrame(int samples, std::int64_t start = kStart,
                                int leading_samples = 0) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  frame->time_base = kNanoseconds;
  frame->pts = start;
  frame->duration = av_rescale_q(samples, {1, 48000}, kNanoseconds);
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
  std::vector<std::int64_t> dts_ns;

  void Bind(mw::streamer::Encoder& encoder, Gate* first_packet = nullptr) {
    encoder.SetOnReady([this](const auto& value) noexcept {
      try {
        std::lock_guard lock(mutex);
        ++ready;
        streams = value;
      } catch (...) {
        RecordCallbackFailure();
      }
    });
    encoder.SetOnPacket([this, first_packet](const auto& value,
                                             std::int64_t timestamp) noexcept {
      bool first = false;
      try {
        std::lock_guard lock(mutex);
        first = packets.empty();
        packets.push_back(value.Ref());
        dts_ns.push_back(timestamp);
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
  void RecordCallbackFailure() noexcept {
    std::lock_guard lock(mutex);
    callback_error = "callback failed";
  }
};

inline std::vector<ffmpeg::Frame> Decode(
    ffmpeg::Decoder& decoder, const std::vector<ffmpeg::Packet>& packets,
    int stream_index) {
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
    if (packet->stream_index != stream_index) continue;
    while (!decoder.SendPacket(packet)) receive();
    receive();
  }
  while (!decoder.Drain()) receive();
  receive();
  return frames;
}

}  // namespace encoder_test

#endif  // MW_STREAMER_TESTS_ENCODER_ENCODER_TEST_SUPPORT_H_
