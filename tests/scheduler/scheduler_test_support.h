#ifndef MW_STREAMER_TESTS_SCHEDULER_TEST_SUPPORT_H_
#define MW_STREAMER_TESTS_SCHEDULER_TEST_SUPPORT_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace mw::streamer::testing {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using mw::streamer::Processor;
using mw::streamer::Scheduler;
using mw::streamer::SchedulerConfig;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};

// Assertions stay on the test thread: callbacks obey the noexcept contract.
class CallbackErrors {
 public:
  template <typename F>
  void Run(F&& action) noexcept {
    try {
      action();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!error_) error_ = std::current_exception();
    }
  }
  template <typename F>
  ffmpeg::Frame Filter(F&& action) noexcept {
    try {
      return action();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!error_) error_ = std::current_exception();
      return ffmpeg::Frame();
    }
  }
  void Rethrow() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_) std::rethrow_exception(error_);
  }

 private:
  std::mutex mutex_;
  std::exception_ptr error_;
};

class Gate {
 public:
  ~Gate() { Release(); }
  void Block() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }
  bool WaitEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 2s, [&] { return entered_; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool released_ = false;
};

class GateRelease final {
 public:
  explicit GateRelease(Gate& gate) : gate_(gate) {}
  ~GateRelease() { gate_.Release(); }

 private:
  Gate& gate_;
};

inline std::vector<ffmpeg::StreamInfo> Streams(bool video, bool audio) {
  std::vector<ffmpeg::StreamInfo> streams;
  if (video) {
    ffmpeg::StreamInfo stream;
    stream.stream_index = 0;
    stream.time_base = kNanoseconds;
    auto* parameters = stream.codec_parameters.get();
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
    parameters->format = AV_PIX_FMT_GRAY8;
    parameters->width = parameters->height = 8;
    streams.push_back(std::move(stream));
  }
  if (audio) {
    ffmpeg::StreamInfo stream;
    stream.stream_index = 1;
    stream.time_base = kNanoseconds;
    auto* parameters = stream.codec_parameters.get();
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = AV_CODEC_ID_PCM_F32LE;
    parameters->format = AV_SAMPLE_FMT_FLTP;
    parameters->sample_rate = 48000;
    av_channel_layout_default(&parameters->ch_layout, 2);
    streams.push_back(std::move(stream));
  }
  return streams;
}

inline ffmpeg::Frame Video(int id, int64_t pts, int64_t duration = 33'333'333) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = frame->height = 8;
  frame->pts = pts;
  frame->duration = duration;
  frame->time_base = kNanoseconds;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配测试视频帧");
  frame->data[0][0] = static_cast<uint8_t>(id);
  return frame;
}

inline ffmpeg::Frame Audio(int samples, int64_t pts = 0, int rate = 48000,
                           AVSampleFormat format = AV_SAMPLE_FMT_FLTP) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->sample_rate = rate;
  frame->nb_samples = samples;
  av_channel_layout_default(&frame->ch_layout, 2);
  frame->pts = pts;
  frame->time_base = kNanoseconds;
  frame->duration = av_rescale_q(samples, {1, rate}, kNanoseconds);
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配测试音频帧");
  for (int i = 0; i < samples; ++i) {
    if (format == AV_SAMPLE_FMT_FLTP) {
      reinterpret_cast<float*>(frame->extended_data[0])[i] = i / 4096.0f;
      reinterpret_cast<float*>(frame->extended_data[1])[i] = -i / 4096.0f;
    } else if (format == AV_SAMPLE_FMT_S16) {
      auto* data = reinterpret_cast<int16_t*>(frame->extended_data[0]);
      data[i * 2] = 8192;
      data[i * 2 + 1] = -8192;
    }
  }
  return frame;
}

struct VideoOutput {
  int id;
  int64_t pts;
  Clock::time_point time;
  const uint8_t* buffer;
};
struct AudioOutput {
  int samples;
  int sample_rate;
  int format;
  int channels;
  int64_t pts;
  std::vector<float> left;
  std::vector<float> right;
};

struct Rig {
  CallbackErrors errors;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<VideoOutput> videos;
  std::vector<AudioOutput> audios;
  int ended = 0;
  int stopped = 0;
  std::thread::id stop_thread;
  Scheduler scheduler;
  Processor processor;
  std::function<void(const ffmpeg::Frame&)> video_sink;
  std::function<void(const ffmpeg::Frame&)> audio_sink;

  explicit Rig(SchedulerConfig config = {}) : scheduler(config) {
    scheduler.SetOnVideo([&](const ffmpeg::Frame& input) {
      errors.Run([&] {
        auto frame = processor.ProcessVideo(input);
        if (video_sink) {
          video_sink(frame);
          return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        videos.push_back(
            {frame->data[0][0], frame->pts, Clock::now(), frame->buf[0]->data});
        changed.notify_all();
      });
    });
    scheduler.SetOnAudio([&](const ffmpeg::Frame& input) {
      errors.Run([&] {
        auto frame = processor.ProcessAudio(input);
        if (audio_sink) {
          audio_sink(frame);
          return;
        }
        AudioOutput item{frame->nb_samples,
                         frame->sample_rate,
                         frame->format,
                         frame->ch_layout.nb_channels,
                         frame->pts,
                         {},
                         {}};
        if (frame->format == AV_SAMPLE_FMT_FLTP && item.channels == 2) {
          const auto* left =
              reinterpret_cast<const float*>(frame->extended_data[0]);
          const auto* right =
              reinterpret_cast<const float*>(frame->extended_data[1]);
          item.left.assign(left, left + item.samples);
          item.right.assign(right, right + item.samples);
        }
        std::lock_guard<std::mutex> lock(mutex);
        audios.push_back(std::move(item));
        changed.notify_all();
      });
    });
    scheduler.SetOnEnded([&] { processor.End(); });
    processor.SetOnEnded([&] {
      errors.Run([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++ended;
        changed.notify_all();
      });
    });
    processor.SetOnStop([&] {
      errors.Run([&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++stopped;
        stop_thread = std::this_thread::get_id();
        changed.notify_all();
      });
    });
  }
  bool Initialize(bool video, bool audio) {
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    const auto streams = Streams(video, audio);
    return processor.Start(streams, cpu) && scheduler.Start(streams);
  }
  template <typename F>
  bool Wait(F&& predicate, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, predicate);
  }
  void SetVideoSink(std::function<void(const ffmpeg::Frame&)> callback) {
    video_sink = std::move(callback);
  }
  void SetAudioSink(std::function<void(const ffmpeg::Frame&)> callback) {
    audio_sink = std::move(callback);
  }
  void Stop() {
    scheduler.Stop();
    processor.Stop();
  }
  ~Rig() { Stop(); }
  void Finish() {
    Stop();
    errors.Rethrow();
  }
};

inline ffmpeg::Frame IdentifiedVideo(int id, std::int64_t pts) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = frame->height = 8;
  frame->pts = pts;
  frame->time_base = kNanoseconds;
  frame->duration = 33'333'333;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配调度空队列回归视频");
  frame->data[0][0] = static_cast<std::uint8_t>(id);
  return frame;
}

inline std::vector<ffmpeg::StreamInfo> VideoStream() {
  std::vector<ffmpeg::StreamInfo> streams;
  ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = kNanoseconds;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
  parameters->format = AV_PIX_FMT_GRAY8;
  parameters->width = parameters->height = 8;
  streams.push_back(std::move(stream));
  return streams;
}

}  // namespace mw::streamer::testing

#endif  // MW_STREAMER_TESTS_SCHEDULER_TEST_SUPPORT_H_
