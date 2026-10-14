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
#include <string>
#include <thread>
#include <vector>

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
  input->Start(path);
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

bool Finished(const Scheduler& scheduler) {
  const auto state = scheduler.status().state;
  return state == SchedulerState::kEnded || state == SchedulerState::kFailed;
}

TEST_CASE("Scheduler defaults retain source timestamps and drain before EOF",
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
  Scheduler scheduler;
  std::vector<std::int64_t> actual;
  std::vector<std::string> events;
  scheduler.SetOnOutput([&](const MediaFrame& frame) {
    if (frame.audio) {
      actual.push_back(frame.audio->get()->pts);
      events.emplace_back("output");
    }
  });
  scheduler.SetOnEof([&] { events.emplace_back("eof"); });
  scheduler.SetOnEnded([&] { events.emplace_back("ended"); });
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  scheduler.Stop();
  input->Stop();
  CHECK(scheduler.status().state == SchedulerState::kEnded);
  CHECK(actual == expected);
  REQUIRE(events.size() >= 3);
  CHECK(events[events.size() - 3] == "output");
  CHECK(events[events.size() - 2] == "eof");
  CHECK(events.back() == "ended");
  CHECK(std::count(events.begin(), events.end(), "eof") == 1);
  CHECK(std::count(events.begin(), events.end(), "ended") == 1);
}

TEST_CASE("Scheduler submits audio one before consuming video zero",
          "[scheduler][pipeline][concurrency]") {
  auto input = OpenInput(VideoPath());
  Scheduler scheduler;
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
  scheduler.Start({input});
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
  Scheduler scheduler;
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
  scheduler.Start({input});
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

TEST_CASE("Scheduler EOF policies preserve fixed input slots",
          "[scheduler][eof][policy]") {
  AudioFile short_file(6000);
  AudioFile long_file(24000);
  SchedulerConfig config;
  SECTION("all inputs keep the ended slot") {
    config.end_policy = SchedulerEndPolicy::kAllInputs;
  }
  SECTION("any input ends after draining accepted ticks") {
    config.end_policy = SchedulerEndPolicy::kAnyInput;
  }
  auto first = OpenInput(short_file.path());
  auto second = OpenInput(long_file.path());
  Scheduler scheduler(config);
  bool stable = true;
  bool partial_eof = false;
  std::uint64_t last_tick = 0;
  scheduler.SetOnAudio([&](const auto& inputs, const SchedulerTick& tick) {
    stable &= inputs.size() == 2;
    if (inputs.size() == 2)
      partial_eof |= inputs[0].state == SchedulerInputState::kEof &&
                     inputs[1].state == SchedulerInputState::kRunning;
    last_tick = tick.frame_number;
    return FirstTrack(inputs, true);
  });
  scheduler.Start({first, second});
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  scheduler.Stop();
  first->Stop();
  second->Stop();
  CHECK(scheduler.status().state == SchedulerState::kEnded);
  CHECK(stable);
  if (config.end_policy == SchedulerEndPolicy::kAllInputs) {
    CHECK(partial_eof);
    CHECK(last_tick >= 14);
  } else {
    CHECK(last_tick < 10);
  }
}

TEST_CASE("Scheduler loops retain channel cadence and never report final EOF",
          "[scheduler][loop][generation]") {
  AudioFile file;
  auto input = OpenInput(file.path(), {}, true);
  Scheduler scheduler;
  std::atomic<bool> next_generation{false};
  int eof_count = 0;
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
  scheduler.SetOnEof([&] { ++eof_count; });
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return next_generation.load(); }));
  scheduler.Stop();
  input->Stop();
  CHECK(monotonic);
  CHECK(previous_tick > 0);
  CHECK(eof_count == 0);
  CHECK(scheduler.status().state == SchedulerState::kStopped);
}

TEST_CASE("Scheduler retains shared inputs until Stop",
          "[scheduler][lifecycle][ownership]") {
  AudioFile file;
  auto input = OpenInput(file.path());
  std::weak_ptr<AvMediaInput> weak = input;
  Scheduler scheduler;
  scheduler.Start({input});
  input.reset();
  CHECK_FALSE(weak.expired());
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  CHECK_FALSE(weak.expired());
  scheduler.Stop();
  CHECK(weak.expired());
}

TEST_CASE("Scheduler buffered loops preserve cadence and audio carry",
          "[scheduler][loop][cadence][continuity]") {
  SchedulerConfig config;
  config.output.fps = {30000, 1001};
  config.output.audio_sample_format = AV_SAMPLE_FMT_S16;
  config.output.audio_channel_layout = AV_CHANNEL_LAYOUT_MONO;
  SECTION("any input policy ignores segment EOF") {
    config.end_policy = SchedulerEndPolicy::kAnyInput;
  }
  SECTION("all input policy ignores segment EOF") {
    config.end_policy = SchedulerEndPolicy::kAllInputs;
  }
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
  int eofs = 0;
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
  scheduler.SetOnEof([&] { ++eofs; });
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return frames.load() >= 10; }));
  scheduler.Stop();
  input->Stop();
  CHECK_FALSE(underflow);
  CHECK(eofs == 0);
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

TEST_CASE("Scheduler mixed input failure fails the complete channel",
          "[scheduler][failure][policy]") {
  SchedulerConfig config;
  SECTION("any input policy") {
    config.end_policy = SchedulerEndPolicy::kAnyInput;
  }
  SECTION("all input policy") {
    config.end_policy = SchedulerEndPolicy::kAllInputs;
  }
  AudioFile file;
  auto healthy = OpenInput(file.path());
  AvMediaInputConfig input_config;
  input_config.proxy.max_retries = 0;
  auto failed = std::make_shared<AvMediaInput>(input_config);
  failed->Start(file.path() + ".missing");
  REQUIRE(WaitFor(
      [&] { return failed->status().state == AvMediaInputState::kFailed; }));
  Scheduler scheduler(config);
  int failures = 0;
  int eofs = 0;
  int outputs = 0;
  std::exception_ptr error;
  scheduler.SetOnFailed([&](std::exception_ptr failure) {
    error = failure;
    ++failures;
  });
  scheduler.SetOnEof([&] { ++eofs; });
  scheduler.SetOnOutput([&](const MediaFrame&) { ++outputs; });
  scheduler.Start({healthy, failed});
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  scheduler.Stop();
  healthy->Stop();
  failed->Stop();
  CHECK(scheduler.status().state == SchedulerState::kFailed);
  CHECK(scheduler.status().error != nullptr);
  CHECK(error != nullptr);
  CHECK(failures == 1);
  CHECK(eofs == 0);
  if (config.end_policy == SchedulerEndPolicy::kAllInputs)
    CHECK(outputs > 0);
  else
    CHECK(outputs == 0);
}

TEST_CASE("Scheduler missing sources retain their fixed empty slots",
          "[scheduler][slots][waiting]") {
  AudioFile file;
  auto ready = OpenInput(file.path(), {}, true);
  auto waiting = std::make_shared<AvMediaInput>();
  waiting->Start(file.path() + ".missing");
  Scheduler scheduler;
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
  scheduler.Start({waiting, ready});
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
  SchedulerConfig config;
  config.output.fps = {2, 1};
  auto input = OpenInput(file.path(), config.output, true);
  Scheduler scheduler(config);
  std::atomic<int> outputs{0};
  int eofs = 0;
  scheduler.SetOnOutput([&](const MediaFrame&) { ++outputs; });
  scheduler.SetOnEof([&] { ++eofs; });
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return outputs.load() >= 1; }));
  const auto start = std::chrono::steady_clock::now();
  scheduler.Stop();
  CHECK(std::chrono::steady_clock::now() - start < 200ms);
  CHECK(input->status().state != AvMediaInputState::kStopped);
  CHECK(eofs == 0);
  input->Stop();
}

TEST_CASE("Scheduler Stop waits for user callbacks without interrupting them",
          "[scheduler][lifecycle][callback]") {
  auto input = OpenInput(VideoPath());
  Scheduler scheduler;
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
  scheduler.Start({input});
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
  SchedulerConfig config;
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
  scheduler.Start({first, second});
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
  Scheduler scheduler;
  bool empty = true;
  int callbacks = 0;
  scheduler.SetOnAudio([&](const auto&, const SchedulerTick&) {
    ++callbacks;
    return ffmpeg::Frame{};
  });
  scheduler.SetOnOutput(
      [&](const MediaFrame& frame) { empty &= !frame.video && !frame.audio; });
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  scheduler.Stop();
  input->Stop();
  CHECK(empty);
  CHECK(callbacks > 0);
}

TEST_CASE(
    "Scheduler fractional cadence preserves samples across tick boundaries",
    "[scheduler][audio][cadence][continuity]") {
  SchedulerConfig config;
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
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return frames.load() >= 7; }));
  scheduler.Stop();
  input->Stop();
  CHECK(format);
  REQUIRE(actual.size() > 8008);
  REQUIRE(expected.size() >= actual.size() - 1);
  CHECK(actual.front() == 0);
  CHECK(std::equal(actual.begin() + 1, actual.end(), expected.begin()));
}

TEST_CASE("Scheduler short tails hold the final sample in each channel",
          "[scheduler][audio][tail]") {
  SchedulerConfig config;
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
  bool saw_tail = false;
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
  scheduler.Start({input});
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  scheduler.Stop();
  input->Stop();
  CHECK(saw_tail);
  CHECK(held);
}

TEST_CASE("Scheduler terminal unsigned audio uses native midpoint silence",
          "[scheduler][audio][silence][eof]") {
  SchedulerConfig config;
  config.output.audio_sample_format = AV_SAMPLE_FMT_U8;
  AudioFile short_file(6000);
  AudioFile long_file(24000);
  auto first = OpenInput(short_file.path(), config.output);
  auto second = OpenInput(long_file.path(), config.output);
  Scheduler scheduler(config);
  bool saw_silence = false;
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
  scheduler.Start({first, second});
  REQUIRE(WaitFor([&] { return Finished(scheduler); }));
  scheduler.Stop();
  first->Stop();
  second->Stop();
  CHECK(saw_silence);
  CHECK(correct);
}

TEST_CASE("Scheduler rejects empty duplicate and Batch input sets",
          "[scheduler][validation]") {
  Scheduler scheduler;
  CHECK_THROWS(scheduler.Start({}));
  CHECK_THROWS(scheduler.Start({nullptr}));
  AudioFile file;
  auto live = OpenInput(file.path());
  CHECK_THROWS(scheduler.Start({live, live}));
  AvMediaInputConfig config;
  config.mode = AvMediaInputMode::kBatch;
  auto batch = std::make_shared<AvMediaInput>(config);
  CHECK_THROWS(scheduler.Start({batch}));
  live->Stop();
}

}  // namespace
