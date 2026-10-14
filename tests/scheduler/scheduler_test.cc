#include "mw/streamer/scheduler/scheduler.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/output/output.h"

namespace {

using namespace std::chrono_literals;
using namespace mw::streamer;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

class AudioFile final {
 public:
  explicit AudioFile(int samples = 6000, bool ramp = false, int channels = 1) {
    static std::atomic<unsigned int> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("mw-scheduler-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence.fetch_add(1)) + ".wav");
    std::ofstream file(path_, std::ios::binary);
    REQUIRE(file.good());
    const auto write = [&file](std::uint32_t value, int bytes) {
      for (int index = 0; index < bytes; ++index)
        file.put(static_cast<char>((value >> (index * 8)) & 255U));
    };
    const auto channel_count = static_cast<std::uint32_t>(channels);
    const auto size = static_cast<std::uint32_t>(samples) * 2U * channel_count;
    file.write("RIFF", 4);
    write(36U + size, 4);
    file.write("WAVEfmt ", 8);
    write(16, 4);
    write(1, 2);
    write(channel_count, 2);
    write(48000, 4);
    write(96000U * channel_count, 4);
    write(2U * channel_count, 2);
    write(16, 2);
    file.write("data", 4);
    write(size, 4);
    for (int index = 0; index < samples; ++index) {
      for (int channel = 0; channel < channels; ++channel) {
        const auto value = ramp ? (channel == 0 ? index + 1 : 32000 - index)
                                : 8192 * (channel + 1);
        write(static_cast<std::uint32_t>(value), 2);
      }
    }
    REQUIRE(file.good());
  }
  ~AudioFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

std::shared_ptr<AvMediaInput> OpenInput(const std::string& path,
                                        AvEncoderConfig output = {},
                                        bool loop = false) {
  AvMediaInputConfig config;
  config.output = output;
  config.proxy.loop = loop;
  config.proxy.max_retries = 0;
  config.video_decoder.thread_count = 1;
  config.audio_decoder.thread_count = 1;
  auto input = std::make_shared<AvMediaInput>(config);
  REQUIRE(input->Start(path));
  REQUIRE(WaitFor([&] {
    const auto state = input->status().state;
    return state == AvMediaInputState::kRunning ||
           state == AvMediaInputState::kDraining ||
           state == AvMediaInputState::kFailed;
  }));
  REQUIRE(input->status().state != AvMediaInputState::kFailed);
  // Running precedes source prefill. Assertions still ignore empty ticks.
  std::this_thread::sleep_for(30ms);
  return input;
}

std::string VideoPath() {
  return std::string(MW_SCHEDULER_TEST_DATA_DIR) + "/h264_aac.mp4";
}

ffmpeg::Frame FirstTrack(const Scheduler::Inputs& inputs, bool audio) {
  for (const auto& input : inputs) {
    const auto& frame = audio ? input.frame.audio : input.frame.video;
    if (frame) return frame->Ref();
  }
  return {};
}

SchedulerConfig Config() {
  SchedulerConfig config;
  config.output.width = 8;
  config.output.height = 8;
  config.output.video_pixel_format = AV_PIX_FMT_GRAY8;
  return config;
}

class SchedulerTestFrameSink final : public FrameSink {
 public:
  explicit SchedulerTestFrameSink(bool clock = false, bool block_send = false)
      : clock_(clock), block_send_(block_send) {}

  bool Initialize(const AvEncoderConfig& config) noexcept override {
    initialized_fps_ = config.fps;
    ++initialize_calls_;
    return initialize_success_.load();
  }

  std::future<bool> Send(OutputFrame frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.push_back(std::move(frame));
    std::promise<bool> completion;
    auto future = completion.get_future();
    if (block_send_) {
      sends_.push_back(std::move(completion));
    } else {
      completion.set_value(true);
    }
    return future;
  }

  bool has_synchronization_clock() const noexcept override { return clock_; }

  std::vector<OutputFrame> frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
  }

  bool ReleaseSend() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sends_.empty()) return false;
    sends_.front().set_value(true);
    sends_.erase(sends_.begin());
    return true;
  }

  AVRational initialized_fps_{};
  std::atomic<int> initialize_calls_{0};
  std::atomic<bool> initialize_success_{true};

 private:
  const bool clock_;
  const bool block_send_;
  mutable std::mutex mutex_;
  std::vector<OutputFrame> frames_;
  std::vector<std::promise<bool>> sends_;
};

static_assert(noexcept(std::declval<Scheduler&>().Start({})));

TEST_CASE("Scheduler requires every frame sink to initialize before running",
          "[scheduler][output][start]") {
  AudioFile file;
  auto input = OpenInput(file.path(), {}, true);
  auto ready = std::make_shared<SchedulerTestFrameSink>();
  auto failing = std::make_shared<SchedulerTestFrameSink>();
  failing->initialize_success_ = false;
  auto output = std::make_shared<Output>(
      std::vector<std::shared_ptr<FrameSink>>{ready, failing});
  Scheduler scheduler(Config());
  scheduler.SetOutput(output);
  std::atomic<int> filter_calls{0};
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    ++filter_calls;
    return FirstTrack(inputs, true);
  });

  CHECK_FALSE(scheduler.Start({input}));
  CHECK(scheduler.status().state == SchedulerState::kStopped);
  CHECK(filter_calls.load() == 0);
  CHECK(ready->frames().empty());
  CHECK(failing->frames().empty());
  CHECK(ready->initialize_calls_.load() == 1);
  CHECK(failing->initialize_calls_.load() == 1);

  failing->initialize_success_ = true;
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return ready->frames().size() >= 2; }));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  CHECK(ready->initialize_calls_.load() == 1);
  CHECK(failing->initialize_calls_.load() == 2);
  CHECK(filter_calls.load() > 0);

  const auto delivered = ready->frames().size();
  CHECK_FALSE(scheduler.Start({input}));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  REQUIRE(WaitFor([&] { return ready->frames().size() > delivered; }));
  CHECK(ready->initialize_calls_.load() == 1);
  CHECK(failing->initialize_calls_.load() == 2);
  scheduler.Stop();
  input->Stop();
}

TEST_CASE("Scheduler EOF retains source timestamps and stays running",
          "[scheduler][pts][eof]") {
  AudioFile file;
  std::vector<std::int64_t> expected;
  auto reference = OpenInput(file.path());
  REQUIRE(WaitFor([&] {
    const auto result = reference->TryReadFrame();
    if (result.audio) expected.push_back(result.audio->get()->pts);
    return result.status == AvFrameSourceReadStatus::kEof;
  }));
  reference->Stop();
  REQUIRE_FALSE(expected.empty());
  auto input = OpenInput(file.path());
  Scheduler scheduler(Config());
  std::vector<std::int64_t> actual;
  std::atomic<int> eof_ticks{0};
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    if (inputs[0].state == SchedulerInputState::kRunning &&
        inputs[0].frame.audio)
      actual.push_back(inputs[0].frame.audio->get()->pts);
    if (inputs[0].state == SchedulerInputState::kEof) ++eof_ticks;
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return eof_ticks.load() >= 3; }));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  scheduler.Stop();
  input->Stop();
  CHECK(scheduler.status().state == SchedulerState::kStopped);
  CHECK(actual == expected);
}

TEST_CASE("Scheduler submits audio one before consuming video zero",
          "[scheduler][pipeline][concurrency]") {
  auto input = OpenInput(VideoPath());
  Scheduler scheduler(Config());
  std::mutex mutex;
  std::condition_variable changed;
  bool audio_one = false;
  bool overlapped = false;
  std::thread::id video_thread;
  std::thread::id audio_thread;
  std::atomic<int> outputs{0};
  std::vector<std::string> events;
  scheduler.SetOnVideo([&](const auto& inputs, const SchedulerTick& tick) {
    video_thread = std::this_thread::get_id();
    if (tick.frame_number == 0) {
      std::unique_lock<std::mutex> lock(mutex);
      overlapped = changed.wait_for(lock, 2s, [&] { return audio_one; });
    }
    return FirstTrack(inputs, false);
  });
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    audio_thread = std::this_thread::get_id();
    if (tick.frame_number == 1) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        audio_one = true;
        events.emplace_back("audio1");
      }
      changed.notify_all();
    }
    return FirstTrack(inputs, true);
  });
  scheduler.SetOnOutput([&](const MediaFrame&) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      events.emplace_back("output");
    }
    if (++outputs == 3) scheduler.RequestStop();
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return outputs.load() >= 3; }));
  scheduler.Stop();
  input->Stop();
  CHECK(overlapped);
  CHECK(video_thread != audio_thread);
  REQUIRE(events.size() >= 2);
  CHECK(events.front() == "audio1");
}

TEST_CASE("Scheduler default video references the prepared source unchanged",
          "[scheduler][video][pts][reference]") {
  auto input = OpenInput(VideoPath());
  Scheduler scheduler(Config());
  std::vector<ffmpeg::Frame> prepared;
  std::vector<ffmpeg::Frame> output;
  std::atomic<int> delivered{0};
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    if (!inputs.empty() && inputs[0].frame.video)
      prepared.push_back(inputs[0].frame.video->Ref());
    return FirstTrack(inputs, true);
  });
  scheduler.SetOnOutput([&](const MediaFrame& frame) {
    if (frame.video) {
      output.push_back(frame.video->Ref());
      if (++delivered == 3) scheduler.RequestStop();
    }
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return delivered.load() >= 3; }));
  scheduler.Stop();
  input->Stop();
  REQUIRE(prepared.size() >= output.size());
  for (std::size_t index = 0; index < output.size(); ++index) {
    CHECK(output[index]->pts == prepared[index]->pts);
    CHECK(output[index]->time_base.num == prepared[index]->time_base.num);
    CHECK(output[index]->time_base.den == prepared[index]->time_base.den);
    REQUIRE(output[index]->buf[0] != nullptr);
    REQUIRE(prepared[index]->buf[0] != nullptr);
    CHECK(output[index]->buf[0]->buffer == prepared[index]->buf[0]->buffer);
  }
}

TEST_CASE("Scheduler EOF preserves fixed input slots and keeps processing",
          "[scheduler][eof][slots]") {
  AudioFile short_file(6000);
  AudioFile long_file(24000);
  auto first = OpenInput(short_file.path());
  auto second = OpenInput(long_file.path());
  Scheduler scheduler(Config());
  bool stable = true;
  bool partial_eof = false;
  std::atomic<int> terminal_ticks{0};
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    stable &= inputs.size() == 2;
    if (inputs.size() == 2) {
      partial_eof |= inputs[0].state == SchedulerInputState::kEof &&
                     inputs[1].state == SchedulerInputState::kRunning;
      if (inputs[0].state == SchedulerInputState::kEof &&
          inputs[1].state == SchedulerInputState::kEof)
        ++terminal_ticks;
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({first, second}));
  REQUIRE(WaitFor([&] { return terminal_ticks.load() >= 3; }));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  scheduler.Stop();
  first->Stop();
  second->Stop();
  CHECK(stable);
  CHECK(partial_eof);
}

TEST_CASE("Scheduler loops retain channel cadence and never report final EOF",
          "[scheduler][loop][generation]") {
  AudioFile file;
  auto input = OpenInput(file.path(), {}, true);
  Scheduler scheduler(Config());
  std::atomic<bool> next_generation{false};
  bool monotonic = true;
  std::uint64_t previous_tick = 0;
  bool first_tick = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    if (!first_tick) monotonic &= tick.frame_number == previous_tick + 1;
    first_tick = false;
    previous_tick = tick.frame_number;
    if (!inputs.empty() && inputs[0].generation > 0 && inputs[0].frame.audio) {
      next_generation = true;
      scheduler.RequestStop();
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return next_generation.load(); }));
  scheduler.Stop();
  input->Stop();
  CHECK(monotonic);
  CHECK(previous_tick > 0);
  CHECK(scheduler.status().state == SchedulerState::kStopped);
}

TEST_CASE("Scheduler retains shared inputs until Stop",
          "[scheduler][lifecycle][ownership]") {
  AudioFile file;
  auto input = OpenInput(file.path());
  std::weak_ptr<AvMediaInput> weak = input;
  Scheduler scheduler(Config());
  REQUIRE(scheduler.Start({input}));
  input.reset();
  CHECK_FALSE(weak.expired());
  std::this_thread::sleep_for(250ms);
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  CHECK_FALSE(weak.expired());
  scheduler.Stop();
  CHECK(weak.expired());
}

TEST_CASE("Scheduler buffered loops preserve cadence and audio carry",
          "[scheduler][loop][cadence][continuity]") {
  SchedulerConfig config = Config();
  config.output.fps = {30000, 1001};
  config.output.audio_sample_format = AV_SAMPLE_FMT_S16;
  config.output.audio_channel_layout = AV_CHANNEL_LAYOUT_MONO;
  // Exactly five native cadence blocks, with distinct samples up to the
  // boundary. Slow user processing lets Input cache the following generation.
  AudioFile file(8008, true);
  auto input = OpenInput(file.path(), config.output, true);
  Scheduler scheduler(config);
  std::atomic<int> frames{0};
  bool started = false;
  bool underflow = false;
  std::vector<std::int16_t> samples;
  std::vector<std::uint64_t> generations;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    if (!inputs.empty()) {
      const auto& slot = inputs[0];
      if (started && slot.state != SchedulerInputState::kRunning)
        underflow = true;
      if (slot.state == SchedulerInputState::kRunning && slot.frame.audio) {
        started = true;
        const auto* audio = slot.frame.audio->get();
        const auto* data =
            reinterpret_cast<const std::int16_t*>(audio->data[0]);
        samples.insert(samples.end(), data, data + audio->nb_samples);
        generations.push_back(slot.generation);
        if (++frames == 10) scheduler.RequestStop();
      }
    }
    std::this_thread::sleep_for(100ms);
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return frames.load() >= 10; }));
  scheduler.Stop();
  input->Stop();
  CHECK_FALSE(underflow);
  REQUIRE(generations.size() == 10);
  CHECK(generations[5] > generations[4]);
  REQUIRE(samples.size() == 16016);
  CHECK(samples.front() == 0);
  bool continuous = true;
  for (std::size_t index = 1; index < samples.size(); ++index) {
    const auto expected = static_cast<std::int16_t>((index - 1) % 8008 + 1);
    continuous &= samples[index] == expected;
  }
  CHECK(continuous);
}

TEST_CASE("Scheduler isolates failed inputs and processes healthy input",
          "[scheduler][failure]") {
  AudioFile file;
  auto healthy = OpenInput(file.path(), {}, true);
  AvMediaInputConfig input_config;
  input_config.proxy.max_retries = 0;
  auto failed = std::make_shared<AvMediaInput>(input_config);
  REQUIRE(failed->Start(file.path() + ".missing"));
  REQUIRE(WaitFor(
      [&] { return failed->status().state == AvMediaInputState::kFailed; }));
  Scheduler scheduler(Config());
  std::atomic<int> ticks{0};
  bool isolated = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    isolated &=
        inputs.size() == 2 && inputs[1].state == SchedulerInputState::kFailed;
    ++ticks;
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({healthy, failed}));
  REQUIRE(WaitFor([&] { return ticks.load() >= 5; }));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  scheduler.Stop();
  healthy->Stop();
  failed->Stop();
  CHECK(isolated);
}

TEST_CASE("Scheduler missing sources retain their fixed empty slots",
          "[scheduler][slots][waiting]") {
  AudioFile file;
  auto ready = OpenInput(file.path(), {}, true);
  auto waiting = std::make_shared<AvMediaInput>();
  REQUIRE(waiting->Start(file.path() + ".missing"));
  Scheduler scheduler(Config());
  std::atomic<int> real_frames{0};
  bool stable = true;
  bool empty = true;
  bool pending = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    stable &= inputs.size() == 2;
    if (inputs.size() == 2) {
      empty &= !inputs[0].frame.video && !inputs[0].frame.audio;
      pending &= inputs[0].state == SchedulerInputState::kWaiting ||
                 inputs[0].state == SchedulerInputState::kUnderflow;
      if (inputs[1].frame.audio && ++real_frames == 3) scheduler.RequestStop();
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({waiting, ready}));
  REQUIRE(WaitFor([&] { return real_frames.load() >= 3; }));
  scheduler.Stop();
  waiting->Stop();
  ready->Stop();
  CHECK(stable);
  CHECK(empty);
  CHECK(pending);
}

TEST_CASE("Scheduler Stop wakes pacing and leaves external Input running",
          "[scheduler][lifecycle][pacing]") {
  AudioFile file(144000);
  SchedulerConfig config = Config();
  config.output.fps = {2, 1};
  auto input = OpenInput(file.path(), config.output, true);
  Scheduler scheduler(config);
  std::atomic<int> outputs{0};
  scheduler.SetOnOutput([&](const MediaFrame&) { ++outputs; });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return outputs.load() >= 1; }));
  const auto start = std::chrono::steady_clock::now();
  scheduler.Stop();
  CHECK(std::chrono::steady_clock::now() - start < 200ms);
  CHECK(input->status().state != AvMediaInputState::kStopped);
  input->Stop();
}

TEST_CASE("Scheduler Stop waits for user callbacks without interrupting them",
          "[scheduler][lifecycle][callback]") {
  auto input = OpenInput(VideoPath());
  Scheduler scheduler(Config());
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  bool completed = false;
  scheduler.SetOnVideo([&](const auto& inputs, const SchedulerTick&) {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
    completed = true;
    return FirstTrack(inputs, false);
  });
  REQUIRE(scheduler.Start({input}));
  bool observed = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    observed = changed.wait_for(lock, 2s, [&] { return entered; });
  }
  auto stopped = std::async(std::launch::async, [&] { scheduler.Stop(); });
  const bool waited = stopped.wait_for(30ms) == std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  changed.notify_all();
  stopped.get();
  input->Stop();
  CHECK(observed);
  CHECK(waited);
  CHECK(completed);
}

TEST_CASE("Scheduler prepares fractional audio cadence across sample formats",
          "[scheduler][audio][cadence][format]") {
  SchedulerConfig config = Config();
  SECTION("29.97 planar float") { config.output.fps = {30000, 1001}; }
  SECTION("59.94 unsigned packed") {
    config.output.fps = {60000, 1001};
    config.output.audio_sample_format = AV_SAMPLE_FMT_U8;
  }
  SECTION("29.97 signed packed") {
    config.output.fps = {30000, 1001};
    config.output.audio_sample_format = AV_SAMPLE_FMT_S16;
  }
  AudioFile file(24000);
  auto first = OpenInput(file.path(), config.output, true);
  auto second = OpenInput(file.path(), config.output, true);
  Scheduler scheduler(config);
  std::atomic<int> ticks{0};
  bool cadence = true;
  bool formats = true;
  bool counts = true;
  std::vector<int> real_frames(2, 0);
  std::vector<int> sample_counts;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    const auto numerator = std::uint64_t{48000} *
                           static_cast<std::uint64_t>(config.output.fps.den);
    const auto denominator = static_cast<std::uint64_t>(config.output.fps.num);
    const auto expected =
        static_cast<int>(numerator * (tick.frame_number + 1) / denominator -
                         numerator * tick.frame_number / denominator);
    cadence &= tick.audio_samples == expected;
    sample_counts.push_back(tick.audio_samples);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      const auto& slot = inputs[index];
      if (!slot.frame.audio) continue;
      if (slot.state == SchedulerInputState::kRunning) ++real_frames[index];
      const auto* audio = slot.frame.audio->get();
      counts &= audio->nb_samples == tick.audio_samples;
      formats &= audio->format == config.output.audio_sample_format &&
                 audio->sample_rate == 48000 &&
                 audio->ch_layout.nb_channels == 2;
    }
    if (++ticks == 10) scheduler.RequestStop();
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({first, second}));
  REQUIRE(WaitFor([&] { return ticks.load() >= 10; }));
  scheduler.Stop();
  first->Stop();
  second->Stop();
  CHECK(cadence);
  CHECK(formats);
  CHECK(counts);
  CHECK(real_frames[0] > 0);
  CHECK(real_frames[1] > 0);
  REQUIRE(sample_counts.size() >= 5);
  CHECK((sample_counts[0] != sample_counts[1] ||
         sample_counts[0] != sample_counts[4]));
}

TEST_CASE("Scheduler empty filters suppress output tracks",
          "[scheduler][filter][empty]") {
  AudioFile file;
  auto input = OpenInput(file.path());
  Scheduler scheduler(Config());
  bool empty = true;
  std::atomic<int> callbacks{0};
  scheduler.SetOnAudio([&](const auto&, const SchedulerTick&) {
    ++callbacks;
    return ffmpeg::Frame{};
  });
  scheduler.SetOnOutput(
      [&](const MediaFrame& frame) { empty &= !frame.video && !frame.audio; });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return callbacks.load() >= 5; }));
  scheduler.Stop();
  input->Stop();
  CHECK(empty);
  CHECK(callbacks > 0);
}

TEST_CASE(
    "Scheduler fractional cadence preserves samples across tick boundaries",
    "[scheduler][audio][cadence][continuity]") {
  SchedulerConfig config = Config();
  config.output.fps = {30000, 1001};
  config.output.audio_sample_format = AV_SAMPLE_FMT_S16;
  config.output.audio_channel_layout = AV_CHANNEL_LAYOUT_MONO;
  AudioFile file(24000, true);
  auto reference = OpenInput(file.path(), config.output);
  std::vector<std::int16_t> expected;
  int reference_frames = 0;
  REQUIRE(WaitFor([&] {
    const auto result = reference->TryReadFrame();
    if (result.audio) {
      const auto* audio = result.audio->get();
      const auto* samples =
          reinterpret_cast<const std::int16_t*>(audio->data[0]);
      expected.insert(expected.end(), samples, samples + audio->nb_samples);
      ++reference_frames;
    }
    return reference_frames >= 8;
  }));
  reference->Stop();

  auto input = OpenInput(file.path(), config.output);
  Scheduler scheduler(config);
  std::vector<std::int16_t> actual;
  std::atomic<int> frames{0};
  bool format = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    if (!inputs.empty() && inputs[0].state == SchedulerInputState::kRunning &&
        inputs[0].frame.audio) {
      const auto* audio = inputs[0].frame.audio->get();
      format &= audio->format == AV_SAMPLE_FMT_S16 &&
                audio->ch_layout.nb_channels == 1;
      const auto* samples =
          reinterpret_cast<const std::int16_t*>(audio->data[0]);
      actual.insert(actual.end(), samples, samples + audio->nb_samples);
      if (++frames == 7) scheduler.RequestStop();
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return frames.load() >= 7; }));
  scheduler.Stop();
  input->Stop();
  CHECK(format);
  REQUIRE(actual.size() > 8008);
  REQUIRE(expected.size() >= actual.size() - 1);
  CHECK(actual.front() == 0);
  CHECK(std::equal(actual.begin() + 1, actual.end(), expected.begin()));
}

TEST_CASE("Scheduler bounds excess audio carry without rejecting the tick",
          "[scheduler][audio][cadence][overflow]") {
  auto config = Config();
  config.output.fps = {50, 1};
  config.output.audio_sample_format = AV_SAMPLE_FMT_S16;
  config.output.audio_channel_layout = AV_CHANNEL_LAYOUT_STEREO;
  SECTION("packed") {}
  SECTION("planar") { config.output.audio_sample_format = AV_SAMPLE_FMT_S16P; }
  auto input_format = config.output;
  // Longer source blocks deliberately exercise the carry limit. Media format
  // remains identical; sample count differences must not reject a whole tick.
  input_format.fps = {25, 1};
  AudioFile file(24000, true, 2);
  auto input = OpenInput(file.path(), input_format);
  Scheduler scheduler(config);
  const std::vector<int> source_offsets{0, 960, 1920, 3840, 5760, 7680, 9600};
  std::atomic<int> frames{0};
  bool lengths = true;
  bool values = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    const auto& slot = inputs[0];
    if (slot.state == SchedulerInputState::kRunning && slot.frame.audio) {
      const auto* audio = slot.frame.audio->get();
      const auto frame_index = static_cast<std::size_t>(frames.load());
      if (frame_index < source_offsets.size()) {
        lengths &= audio->nb_samples == 960 && tick.audio_samples == 960;
        const bool planar =
            av_sample_fmt_is_planar(config.output.audio_sample_format) != 0;
        for (int channel = 0; channel < 2; ++channel) {
          const auto* data = reinterpret_cast<const std::int16_t*>(
              audio->extended_data[planar ? channel : 0]);
          for (int index = 0; index < audio->nb_samples; ++index) {
            const int source_index = source_offsets[frame_index] + index;
            const int expected =
                channel == 0 ? source_index + 1 : 32000 - source_index;
            values &= data[planar ? index : index * 2 + channel] == expected;
          }
        }
        if (++frames == static_cast<int>(source_offsets.size()))
          scheduler.RequestStop();
      }
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] {
    return frames.load() == static_cast<int>(source_offsets.size());
  }));
  scheduler.Stop();
  input->Stop();
  CHECK(lengths);
  CHECK(values);
}

TEST_CASE("Scheduler short tails hold the final sample in each channel",
          "[scheduler][audio][tail]") {
  SchedulerConfig config = Config();
  SECTION("planar float") {}
  SECTION("unsigned packed") {
    config.output.audio_sample_format = AV_SAMPLE_FMT_U8;
  }
  SECTION("signed packed") {
    config.output.audio_sample_format = AV_SAMPLE_FMT_S16;
  }
  AudioFile file(6000, true, 2);
  auto input = OpenInput(file.path(), config.output);
  Scheduler scheduler(config);
  std::atomic<bool> saw_tail{false};
  bool held = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    if (!inputs.empty() && inputs[0].frame.audio) {
      const auto* audio = inputs[0].frame.audio->get();
      if (audio->pts == 4800) {
        saw_tail = true;
        held &= audio->nb_samples == tick.audio_samples;
        const auto format = static_cast<AVSampleFormat>(audio->format);
        const int bytes = av_get_bytes_per_sample(format);
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        for (int channel = 0; channel < audio->ch_layout.nb_channels;
             ++channel) {
          const auto* plane = audio->extended_data[planar ? channel : 0];
          const int stride =
              planar ? bytes : bytes * audio->ch_layout.nb_channels;
          const int offset = planar ? 0 : channel * bytes;
          const auto* last = plane + 1199 * stride + offset;
          // Different samples and different left/right channels rule out
          // copying the first sample or repeating the wrong channel.
          held &= std::memcmp(plane + offset, last,
                              static_cast<std::size_t>(bytes)) != 0;
          for (int index = 1200; index < audio->nb_samples; ++index)
            held &= std::memcmp(last, plane + index * stride + offset,
                                static_cast<std::size_t>(bytes)) == 0;
        }
        const auto* left =
            audio->extended_data[0] + 1199 * (planar ? bytes : bytes * 2);
        const auto* right =
            planar ? audio->extended_data[1] + 1199 * bytes : left + bytes;
        held &= std::memcmp(left, right, static_cast<std::size_t>(bytes)) != 0;
      }
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return saw_tail.load(); }));
  scheduler.Stop();
  input->Stop();
  CHECK(saw_tail.load());
  CHECK(held);
}

TEST_CASE("Scheduler terminal unsigned audio uses native midpoint silence",
          "[scheduler][audio][silence][eof]") {
  SchedulerConfig config = Config();
  config.output.audio_sample_format = AV_SAMPLE_FMT_U8;
  AudioFile short_file(6000);
  AudioFile long_file(24000);
  auto first = OpenInput(short_file.path(), config.output);
  auto second = OpenInput(long_file.path(), config.output);
  Scheduler scheduler(config);
  std::atomic<bool> saw_silence{false};
  bool correct = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    if (inputs.size() == 2 && inputs[0].state == SchedulerInputState::kEof &&
        inputs[0].frame.audio) {
      saw_silence = true;
      const auto* audio = inputs[0].frame.audio->get();
      const int size = audio->nb_samples * audio->ch_layout.nb_channels;
      for (int index = 0; index < size; ++index)
        correct &= audio->data[0][index] == 128;
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({first, second}));
  REQUIRE(WaitFor([&] { return saw_silence.load(); }));
  scheduler.Stop();
  first->Stop();
  second->Stop();
  CHECK(saw_silence.load());
  CHECK(correct);
}

TEST_CASE(
    "Scheduler output frame sinks share delayed tick identity and cadence",
    "[scheduler][output][cadence]") {
  SchedulerConfig config = Config();
  config.output.fps = {30000, 1001};
  AudioFile file(24000);
  std::string path;
  bool expect_video = false;
  SECTION("audio only") { path = file.path(); }
  SECTION("audio and video") {
    path = VideoPath();
    expect_video = true;
  }
  auto input = OpenInput(path, config.output);
  auto first = std::make_shared<SchedulerTestFrameSink>();
  auto second = std::make_shared<SchedulerTestFrameSink>();
  auto output = std::make_shared<Output>(
      std::vector<std::shared_ptr<FrameSink>>{first, second});
  Scheduler scheduler(config);
  scheduler.SetOutput(output);
  std::vector<SchedulerTick> ticks;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    ticks.push_back(tick);
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return first->frames().size() >= 8; }));
  scheduler.Stop();
  input->Stop();
  CHECK(scheduler.status().state == SchedulerState::kStopped);
  CHECK(first->initialized_fps_.num == config.output.fps.num);
  CHECK(first->initialized_fps_.den == config.output.fps.den);
  const auto first_frames = first->frames();
  const auto second_frames = second->frames();
  REQUIRE_FALSE(first_frames.empty());
  REQUIRE(first_frames.size() == second_frames.size());
  for (std::size_t index = 0; index < first_frames.size(); ++index) {
    const auto& frame = first_frames[index];
    const auto& other = second_frames[index];
    const auto numerator = std::uint64_t{48000} * 1001;
    const auto sample_index = numerator * frame.frame_number / 30000;
    const auto next_sample_index = numerator * (frame.frame_number + 1) / 30000;
    CHECK(frame.audio_sample_index == sample_index);
    CHECK(frame.audio_samples ==
          static_cast<int>(next_sample_index - sample_index));
    REQUIRE(frame.frame_number < ticks.size());
    CHECK(frame.audio_samples == ticks[frame.frame_number].audio_samples);
    CHECK(frame.frame_number == other.frame_number);
    CHECK(frame.audio_sample_index == other.audio_sample_index);
    CHECK(frame.audio_samples == other.audio_samples);
    REQUIRE(frame.frame.audio);
    REQUIRE(other.frame.audio);
    CHECK(frame.frame.audio->get()->data[0] ==
          other.frame.audio->get()->data[0]);
    CHECK(frame.frame.audio->get()->nb_samples == frame.audio_samples);
    if (expect_video) {
      REQUIRE(frame.frame.video);
      REQUIRE(other.frame.video);
      CHECK(frame.frame.video->get()->data[0] ==
            other.frame.video->get()->data[0]);
    }
    if (index > 0) {
      CHECK(frame.frame_number == first_frames[index - 1].frame_number + 1);
    }
  }
}

TEST_CASE("Scheduler EOF and Stop preserve frame sink lifetime and allow reuse",
          "[scheduler][output][eof]") {
  AudioFile file;
  auto input = OpenInput(file.path());
  auto frame_sink = std::make_shared<SchedulerTestFrameSink>();
  auto output = std::make_shared<Output>(
      std::vector<std::shared_ptr<FrameSink>>{frame_sink});
  Scheduler scheduler(Config());
  scheduler.SetOutput(output);
  std::atomic<int> eof_ticks{0};
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    if (inputs[0].state == SchedulerInputState::kEof) ++eof_ticks;
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return eof_ticks.load() >= 3; }));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  scheduler.Stop();
  const auto previous = frame_sink->frames().size();
  REQUIRE(scheduler.Start({}));
  REQUIRE(WaitFor([&] { return frame_sink->frames().size() >= previous + 3; }));
  scheduler.Stop();
  input->Stop();
  CHECK(frame_sink->initialize_calls_.load() == 1);
}

TEST_CASE("Scheduler Stop interrupts send waiting and retains the frame sink",
          "[scheduler][output][stop]") {
  AudioFile file;
  auto input = OpenInput(file.path(), {}, true);
  auto frame_sink = std::make_shared<SchedulerTestFrameSink>(false, true);
  auto output = std::make_shared<Output>(
      std::vector<std::shared_ptr<FrameSink>>{frame_sink});
  Scheduler scheduler(Config());
  scheduler.SetOutput(output);
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return !frame_sink->frames().empty(); }));
  const auto start = std::chrono::steady_clock::now();
  scheduler.Stop();
  CHECK(std::chrono::steady_clock::now() - start < 200ms);
  CHECK(frame_sink->ReleaseSend());
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return frame_sink->frames().size() >= 2; }));
  scheduler.Stop();
  input->Stop();
  CHECK(frame_sink->initialize_calls_.load() == 1);
  CHECK(scheduler.status().state == SchedulerState::kStopped);
}

TEST_CASE("Scheduler frame sink clock controls progress without system pacing",
          "[scheduler][output][clock]") {
  SchedulerConfig config = Config();
  config.output.fps = {2, 1};
  AudioFile file(96000);
  auto input = OpenInput(file.path(), config.output, true);
  auto frame_sink = std::make_shared<SchedulerTestFrameSink>(true, true);
  auto output = std::make_shared<Output>(
      std::vector<std::shared_ptr<FrameSink>>{frame_sink});
  Scheduler scheduler(config);
  scheduler.SetOutput(output);
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return frame_sink->frames().size() == 1; }));
  // The future really holds the channel until the device grants progress.
  CHECK_FALSE(WaitFor([&] { return frame_sink->frames().size() > 1; }, 50ms));
  REQUIRE(frame_sink->ReleaseSend());
  REQUIRE(WaitFor([&] { return frame_sink->frames().size() == 2; }, 500ms));
  REQUIRE(frame_sink->ReleaseSend());
  REQUIRE(WaitFor([&] { return frame_sink->frames().size() == 3; }, 500ms));
  scheduler.Stop();
  input->Stop();
  CHECK(scheduler.status().state == SchedulerState::kStopped);
}

TEST_CASE("Scheduler rejects null duplicate and Batch input sets",
          "[scheduler][validation]") {
  Scheduler scheduler(Config());
  CHECK_FALSE(scheduler.Start({nullptr}));
  AudioFile file;
  auto live = OpenInput(file.path());
  CHECK_FALSE(scheduler.Start({live, live}));
  AvMediaInputConfig config;
  config.mode = AvMediaInputMode::kBatch;
  auto batch = std::make_shared<AvMediaInput>(config);
  CHECK_FALSE(scheduler.Start({batch}));
  live->Stop();
}

TEST_CASE(
    "Scheduler no inputs and all failed inputs produce CPU zero and silence",
    "[scheduler][default][failure]") {
  const auto run = [](AVPixelFormat pixel_format) {
    std::vector<std::shared_ptr<AvMediaInput>> inputs;
    SECTION("no inputs") {}
    SECTION("all inputs failed") {
      AvMediaInputConfig config;
      config.proxy.max_retries = 0;
      auto input = std::make_shared<AvMediaInput>(config);
      AudioFile file;
      REQUIRE(input->Start(file.path() + ".missing"));
      REQUIRE(WaitFor(
          [&] { return input->status().state == AvMediaInputState::kFailed; }));
      inputs.push_back(input);
    }
    auto config = Config();
    config.output.fps = {30000, 1001};
    config.output.video_pixel_format = pixel_format;
    Scheduler scheduler(config);
    std::atomic<int> filter_calls{0};
    std::atomic<int> outputs{0};
    bool valid = true;
    bool zero = true;
    scheduler.SetOnVideo([&](const auto&, const SchedulerTick&) {
      ++filter_calls;
      return ffmpeg::Frame{};
    });
    scheduler.SetOnAudio([&](const auto&, const SchedulerTick&) {
      ++filter_calls;
      return ffmpeg::Frame{};
    });
    scheduler.SetOnOutput([&](const MediaFrame& frame) {
      valid &= bool(frame.video) && bool(frame.audio);
      if (frame.video) {
        const auto* video = frame.video->get();
        valid &= video->width == 8 && video->height == 8 &&
                 video->format == config.output.video_pixel_format &&
                 !video->hw_frames_ctx;
        bool has_buffer = false;
        for (const auto* buffer : video->buf) {
          if (!buffer) continue;
          has_buffer = true;
          for (std::size_t index = 0; index < buffer->size; ++index)
            zero &= buffer->data[index] == 0;
        }
        valid &= has_buffer;
      }
      if (frame.audio) {
        const auto* audio = frame.audio->get();
        valid &= audio->sample_rate == 48000 &&
                 audio->format == AV_SAMPLE_FMT_FLTP &&
                 audio->ch_layout.nb_channels == 2 &&
                 (audio->nb_samples == 1601 || audio->nb_samples == 1602);
        for (int channel = 0; channel < audio->ch_layout.nb_channels; ++channel)
          for (int index = 0; index < audio->nb_samples * 4; ++index)
            zero &= audio->extended_data[channel][index] == 0;
      }
      ++outputs;
    });
    REQUIRE(scheduler.Start(inputs));
    REQUIRE(WaitFor([&] { return outputs.load() >= 5; }));
    CHECK(scheduler.status().state == SchedulerState::kRunning);
    scheduler.Stop();
    for (auto& input : inputs) input->Stop();
    CHECK(filter_calls.load() == 0);
    CHECK(valid);
    CHECK(zero);
  };
  SECTION("GRAY8") { run(AV_PIX_FMT_GRAY8); }
  SECTION("NV12") { run(AV_PIX_FMT_NV12); }
  SECTION("BGRA") { run(AV_PIX_FMT_BGRA); }
}

TEST_CASE("Scheduler reuses default buffers prepared at construction",
          "[scheduler][default][silence][buffers]") {
  auto config = Config();
  config.output.audio_sample_format = AV_SAMPLE_FMT_U8;
  SECTION("fixed cadence") {}
  SECTION("fractional cadence") { config.output.fps = {30000, 1001}; }
  Scheduler scheduler(config);
  std::vector<MediaFrame> frames;
  std::atomic<int> outputs{0};
  scheduler.SetOnOutput([&](const MediaFrame& frame) {
    frames.push_back(frame);
    ++outputs;
  });
  REQUIRE(scheduler.Start({}));
  REQUIRE(WaitFor([&] { return outputs.load() >= 6; }));
  scheduler.Stop();
  std::vector<const AVFrame*> silence_templates;
  for (const auto& frame : frames) {
    REQUIRE(frame.video);
    REQUIRE(frame.audio);
    const auto* audio = frame.audio->get();
    if (&frame != &frames.front())
      CHECK(frame.video->get() != frames.front().video->get());
    CHECK(frame.video->get()->data[0] == frames.front().video->get()->data[0]);
    const auto same_length =
        std::find_if(silence_templates.begin(), silence_templates.end(),
                     [audio](const auto* cached) {
                       return cached->nb_samples == audio->nb_samples;
                     });
    if (same_length == silence_templates.end()) {
      silence_templates.push_back(audio);
    } else {
      CHECK(audio != *same_length);
      CHECK(audio->data[0] == (*same_length)->data[0]);
    }
    for (int index = 0;
         index < audio->nb_samples * audio->ch_layout.nb_channels; ++index)
      REQUIRE(audio->data[0][index] == 128);
  }
  CHECK(silence_templates.size() ==
        (config.output.fps.den == 1 ? std::size_t{1} : std::size_t{2}));
}

TEST_CASE("Scheduler EOF continues held video and silent audio",
          "[scheduler][eof][video][silence]") {
  auto input = OpenInput(VideoPath());
  Scheduler scheduler(Config());
  std::atomic<int> eof_ticks{0};
  ffmpeg::Frame last_video;
  bool held = true;
  bool silent = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    const auto& slot = inputs[0];
    if (slot.state == SchedulerInputState::kRunning && slot.frame.video)
      last_video = slot.frame.video->Ref();
    if (slot.state == SchedulerInputState::kEof) {
      held &= bool(slot.frame.video) && last_video->data[0] != nullptr;
      if (slot.frame.video && last_video->data[0] != nullptr)
        held &= slot.frame.video->get()->data[0] == last_video->data[0];
      silent &= bool(slot.frame.audio);
      if (slot.frame.audio) {
        const auto* audio = slot.frame.audio->get();
        for (int channel = 0; channel < audio->ch_layout.nb_channels; ++channel)
          for (int index = 0; index < audio->nb_samples * 4; ++index)
            silent &= audio->extended_data[channel][index] == 0;
      }
      ++eof_ticks;
    }
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return eof_ticks.load() >= 3; }));
  CHECK(scheduler.status().state == SchedulerState::kRunning);
  scheduler.Stop();
  input->Stop();
  CHECK(held);
  CHECK(silent);
}

TEST_CASE("Scheduler constructor requires a complete CPU output format",
          "[scheduler][validation][format]") {
  CHECK_THROWS(Scheduler{});
  auto config = Config();
  SECTION("zero width") { config.output.width = 0; }
  SECTION("zero height") { config.output.height = 0; }
  SECTION("unspecified pixel format") {
    config.output.video_pixel_format = AV_PIX_FMT_NONE;
  }
  SECTION("hardware pixel format") {
    config.output.video_pixel_format = AV_PIX_FMT_CUDA;
  }
  CHECK_THROWS(Scheduler{config});
}

TEST_CASE("Scheduler accepts an input before it starts",
          "[scheduler][waiting][lifecycle]") {
  auto input = std::make_shared<AvMediaInput>();
  Scheduler scheduler(Config());
  std::atomic<int> ticks{0};
  bool waiting = true;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick&) {
    waiting &= inputs.size() == 1 &&
               (inputs[0].state == SchedulerInputState::kWaiting ||
                inputs[0].state == SchedulerInputState::kStopped);
    ++ticks;
    return FirstTrack(inputs, true);
  });
  REQUIRE(scheduler.Start({input}));
  REQUIRE(WaitFor([&] { return ticks.load() >= 3; }));
  scheduler.Stop();
  CHECK(waiting);
}

}  // namespace
