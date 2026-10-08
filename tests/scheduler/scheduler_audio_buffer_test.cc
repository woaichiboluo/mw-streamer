#include <catch2/catch_test_macros.hpp>

#include "scheduler_test_support.h"

namespace {
using namespace mw::streamer::testing;

constexpr int kSampleRate = 48000;

ffmpeg::Frame SequenceAudio(int count, int first, std::int64_t pts,
                            AVSampleFormat format = AV_SAMPLE_FMT_FLTP) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->sample_rate = kSampleRate;
  frame->nb_samples = count;
  frame->pts = pts;
  frame->time_base = kNanoseconds;
  frame->duration = av_rescale_q(count, {1, kSampleRate}, kNanoseconds);
  av_channel_layout_default(&frame->ch_layout, 2);
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配调度回归音频");
  for (int i = 0; i < count; ++i) {
    if (format == AV_SAMPLE_FMT_FLTP) {
      const auto sample = static_cast<float>(first + i) / 4096.0f;
      reinterpret_cast<float*>(frame->extended_data[0])[i] = sample;
      reinterpret_cast<float*>(frame->extended_data[1])[i] = -sample;
    } else {
      auto* samples = reinterpret_cast<std::int16_t*>(frame->extended_data[0]);
      samples[2 * i] = static_cast<std::int16_t>(first + i);
      samples[2 * i + 1] = static_cast<std::int16_t>(-first - i);
    }
  }
  return frame;
}

std::vector<ffmpeg::StreamInfo> AudioStream() {
  std::vector<ffmpeg::StreamInfo> streams;
  ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = kNanoseconds;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_PCM_F32LE;
  parameters->format = AV_SAMPLE_FMT_FLTP;
  parameters->sample_rate = kSampleRate;
  av_channel_layout_default(&parameters->ch_layout, 2);
  streams.push_back(std::move(stream));
  return streams;
}

class AudioRig {
 public:
  AudioRig() {
    scheduler.SetOnAudio([this](const ffmpeg::Frame& frame) noexcept {
      try {
        std::lock_guard<std::mutex> lock(mutex);
        frames.push_back(frame.Ref());
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        error = std::current_exception();
      }
    });
    scheduler.SetOnEnded([this]() noexcept {
      std::lock_guard<std::mutex> lock(mutex);
      ended = true;
      changed.notify_all();
    });
  }

  bool Initialize() { return scheduler.Start(AudioStream()); }

  bool DrainAndWait() {
    scheduler.Drain();
    std::unique_lock<std::mutex> lock(mutex);
    const bool finished = changed.wait_for(lock, std::chrono::seconds(3),
                                           [this] { return ended; });
    lock.unlock();
    scheduler.Stop();
    if (error) std::rethrow_exception(error);
    return finished;
  }

  // Callback state outlives Scheduler during destruction.
  std::mutex mutex;
  std::condition_variable changed;
  bool ended = false;
  std::exception_ptr error;
  std::vector<ffmpeg::Frame> frames;
  Scheduler scheduler;
};

class AudioDelayGate {
 public:
  void ObserveVideo(std::int64_t pts) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    video_seen_ = true;
    video_pts_ = pts;
    if (entered_ && pts - baseline_pts_ >= 100'000'000) elapsed_ = true;
    changed_.notify_all();
  }

  void BlockFirstAudio() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (entered_) return;
    entered_ = true;
    baseline_pts_ = video_pts_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }

  bool WaitVideo() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(3),
                             [this] { return video_seen_; });
  }

  bool WaitElapsed() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(3),
                             [this] { return elapsed_; });
  }

  void Release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool video_seen_ = false;
  bool entered_ = false;
  bool released_ = false;
  bool elapsed_ = false;
  std::int64_t video_pts_ = 0;
  std::int64_t baseline_pts_ = 0;
};

class AudioGateRelease {
 public:
  explicit AudioGateRelease(AudioDelayGate& gate) : gate_(gate) {}
  ~AudioGateRelease() { gate_.Release(); }

 private:
  AudioDelayGate& gate_;
};

TEST_CASE("窗口音频保留跨输入帧的真实样本顺序与尾样本", "[scheduler][audio]") {
  AudioRig rig;
  REQUIRE(rig.Initialize());
  int submitted = 0;
  // Uneven input frame sizes cross multiple 1024-sample output boundaries.
  for (const int count : {511, 513, 997, 36}) {
    const auto pts = 30'000'000'000LL +
                     av_rescale_q(submitted, {1, kSampleRate}, kNanoseconds);
    auto frame = SequenceAudio(count, submitted + 1, pts);
    REQUIRE(rig.scheduler.SubmitAudio(frame));
    frame.Unref();
    submitted += count;
  }
  REQUIRE(rig.DrainAndWait());
  REQUIRE_FALSE(rig.frames.empty());
  int expected = 1;
  bool saw_content = false;
  for (const auto& frame : rig.frames) {
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    REQUIRE(frame->sample_rate == kSampleRate);
    REQUIRE(frame->nb_samples > 0);
    REQUIRE(frame->nb_samples <= 1024);
    const auto* left = reinterpret_cast<const float*>(frame->extended_data[0]);
    const auto* right = reinterpret_cast<const float*>(frame->extended_data[1]);
    for (int i = 0; i < frame->nb_samples; ++i) {
      if (left[i] == 0.0f) {
        CHECK(right[i] == 0.0f);
        // Source arrival may leave a real leading gap in the first window.
        CHECK_FALSE(saw_content);
        continue;
      }
      saw_content = true;
      CHECK(left[i] == static_cast<float>(expected) / 4096.0f);
      CHECK(right[i] == -static_cast<float>(expected) / 4096.0f);
      ++expected;
    }
  }
  CHECK(expected == submitted + 1);
  CHECK(saw_content);
  for (std::size_t i = 1; i < rig.frames.size(); ++i) {
    CHECK(rig.frames[i]->pts > rig.frames[i - 1]->pts);
  }
}

TEST_CASE("独立Scheduler将打包音频转换后完整交付短尾帧", "[scheduler][audio]") {
  AudioRig rig;
  REQUIRE(rig.Initialize());
  REQUIRE(rig.scheduler.SubmitAudio(
      SequenceAudio(1500, 1, 30'000'000'000, AV_SAMPLE_FMT_S16)));
  REQUIRE(rig.DrainAndWait());
  REQUIRE_FALSE(rig.frames.empty());
  int expected = 1;
  bool started = false;
  for (const auto& frame : rig.frames) {
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    REQUIRE(frame->nb_samples > 0);
    REQUIRE(frame->nb_samples <= 1024);
    REQUIRE(frame->ch_layout.nb_channels == 2);
    const auto* left = reinterpret_cast<const float*>(frame->extended_data[0]);
    const auto* right = reinterpret_cast<const float*>(frame->extended_data[1]);
    for (int i = 0; i < frame->nb_samples; ++i) {
      if (!started && left[i] == 0.0f) {
        CHECK(right[i] == 0.0f);
        continue;
      }
      started = true;
      CHECK(left[i] == static_cast<float>(expected) / 32768.0f);
      CHECK(right[i] == -static_cast<float>(expected) / 32768.0f);
      ++expected;
    }
  }
  CHECK(expected == 1501);
}

TEST_CASE("音频输入格式切换后归一输出且样本与PTS连续", "[scheduler][audio]") {
  AudioRig rig;
  REQUIRE(rig.Initialize());
  constexpr std::int64_t kSourceStart = 30'000'000'000;
  REQUIRE(rig.scheduler.SubmitAudio(SequenceAudio(480, 1, kSourceStart)));
  REQUIRE(rig.scheduler.SubmitAudio(
      SequenceAudio(1024, 481, kSourceStart + 10'000'000, AV_SAMPLE_FMT_S16)));
  REQUIRE(rig.DrainAndWait());
  int expected = 1;
  bool started = false;
  for (std::size_t index = 0; index < rig.frames.size(); ++index) {
    const auto& frame = rig.frames[index];
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    REQUIRE(frame->nb_samples > 0);
    REQUIRE(frame->nb_samples <= 1024);
    const auto* left = reinterpret_cast<const float*>(frame->extended_data[0]);
    const auto* right = reinterpret_cast<const float*>(frame->extended_data[1]);
    for (int i = 0; i < frame->nb_samples; ++i) {
      if (!started && left[i] == 0.0f) {
        CHECK(right[i] == 0.0f);
        continue;
      }
      started = true;
      const float value =
          static_cast<float>(expected) / (expected <= 480 ? 4096.0f : 32768.0f);
      CHECK(left[i] == value);
      CHECK(right[i] == -value);
      ++expected;
    }
    if (index) {
      const auto expected_pts =
          rig.frames[index - 1]->pts + rig.frames[index - 1]->duration;
      CHECK(frame->pts >= expected_pts - 1);
      CHECK(frame->pts <= expected_pts + 1);
    }
  }
  CHECK(expected == 1505);
}

TEST_CASE("慢音频输出回调恢复后按历史窗口真正消费PCM", "[scheduler][audio]") {
  AudioDelayGate gate;
  AudioRig rig;
  // The guard releases a blocked callback before AudioRig's Scheduler dies,
  // including when a submission assertion fails on the test thread.
  AudioGateRelease release(gate);
  rig.scheduler.SetOnVideo([&](const ffmpeg::Frame& frame) noexcept {
    gate.ObserveVideo(frame->pts);
  });
  rig.scheduler.SetOnAudio([&](const ffmpeg::Frame& frame) noexcept {
    try {
      std::lock_guard<std::mutex> lock(rig.mutex);
      rig.frames.push_back(frame.Ref());
    } catch (...) {
      std::lock_guard<std::mutex> lock(rig.mutex);
      rig.error = std::current_exception();
    }
    gate.BlockFirstAudio();
  });
  auto streams = VideoStream();
  auto audio = AudioStream();
  audio.front().stream_index = 1;
  streams.push_back(std::move(audio.front()));
  REQUIRE(rig.scheduler.Start(streams));
  constexpr std::int64_t kStart = 30'000'000'000;
  auto video = IdentifiedVideo(1, kStart);
  video->duration = 300'000'000;
  REQUIRE(rig.scheduler.SubmitVideo(video));
  // Model the input worker's initial AAC prefetch before waiting for output.
  // Separate 1024-sample submissions keep the next-system-PTS prediction
  // current when the first video render establishes its timing adjustment.
  for (int block = 0; block < 3; ++block) {
    const int first = block * 1024;
    const auto pts =
        kStart + av_rescale_q(first, {1, kSampleRate}, kNanoseconds);
    REQUIRE(rig.scheduler.SubmitAudio(SequenceAudio(1024, first + 1, pts)));
  }
  const bool video_started = gate.WaitVideo();
  const bool delayed = gate.WaitElapsed();
  const bool appended =
      rig.scheduler.SubmitAudio(SequenceAudio(1024, 3073, kStart + 64'000'000));
  rig.scheduler.Drain();
  gate.Release();
  const bool ended = rig.DrainAndWait();
  CHECK(video_started);
  CHECK(delayed);
  CHECK(appended);
  CHECK(ended);
  REQUIRE_FALSE(rig.frames.empty());
  int expected = 1;
  for (std::size_t index = 0; index < rig.frames.size(); ++index) {
    const auto& frame = rig.frames[index];
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    const auto* left = reinterpret_cast<const float*>(frame->extended_data[0]);
    const auto* right = reinterpret_cast<const float*>(frame->extended_data[1]);
    for (int i = 0; i < frame->nb_samples; ++i) {
      if (!left[i]) {
        CHECK(expected == 1);
        CHECK(right[i] == 0.0f);
        continue;
      }
      CHECK(left[i] == static_cast<float>(expected) / 4096.0f);
      CHECK(right[i] == -static_cast<float>(expected) / 4096.0f);
      ++expected;
    }
    if (index) {
      const auto& previous = rig.frames[index - 1];
      const auto expected_pts = previous->pts + previous->duration;
      CHECK(frame->pts >= expected_pts - 1);
      CHECK(frame->pts <= expected_pts + 1);
    }
  }
  CHECK(expected == 4097);
}

}  // namespace
