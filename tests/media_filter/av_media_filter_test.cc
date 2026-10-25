#include "mw/streamer/filter/av_media_filter.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include <ffnvcodec/dynlink_cuda.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::AvMediaFilter;
namespace ffmpeg = mw::streamer::ffmpeg;

class TestCudaDriver final {
 public:
  using SetCurrent = CUresult(CUDAAPI*)(CUcontext);

  TestCudaDriver() {
#if defined(_WIN32)
    library_ = LoadLibraryW(L"nvcuda.dll");
#else
    library_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!library_) throw std::runtime_error("Cannot load CUDA driver");
    try {
      current = Load<tcuCtxGetCurrent*>("cuCtxGetCurrent");
      set = Load<SetCurrent>("cuCtxSetCurrent");
      push = Load<tcuCtxPushCurrent_v2*>("cuCtxPushCurrent_v2");
      pop = Load<tcuCtxPopCurrent_v2*>("cuCtxPopCurrent_v2");
    } catch (...) {
      Close();
      throw;
    }
  }
  ~TestCudaDriver() { Close(); }
  TestCudaDriver(const TestCudaDriver&) = delete;
  TestCudaDriver& operator=(const TestCudaDriver&) = delete;

  tcuCtxGetCurrent* current = nullptr;
  SetCurrent set = nullptr;
  tcuCtxPushCurrent_v2* push = nullptr;
  tcuCtxPopCurrent_v2* pop = nullptr;

 private:
  template <typename Function>
  Function Load(const char* name) {
#if defined(_WIN32)
    const auto symbol = GetProcAddress(library_, name);
#else
    const auto symbol = dlsym(library_, name);
#endif
    if (!symbol) throw std::runtime_error("Cannot load CUDA context function");
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(symbol));
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
  }
  void Close() noexcept {
#if defined(_WIN32)
    FreeLibrary(library_);
#else
    dlclose(library_);
#endif
  }
#if defined(_WIN32)
  HMODULE library_ = nullptr;
#else
  void* library_ = nullptr;
#endif
};

template <typename Exception>
void CheckException(const std::exception_ptr& error,
                    const std::string& message) {
  REQUIRE(error);
  // Recapturing an exception may copy it; its identity is not portable.
  REQUIRE_THROWS_MATCHES(std::rethrow_exception(error), Exception,
                         Catch::Matchers::Message(message));
}

ffmpeg::Frame Video(std::uint8_t pixel = 12) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = 8;
  frame->height = 8;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate video frame");
  }
  frame->data[0][0] = pixel;
  return frame;
}

ffmpeg::Frame InvalidFrame() {
  ffmpeg::Frame frame;
  const auto owned = std::move(frame);
  return frame;
}

ffmpeg::Frame Audio(std::int16_t sample = 34) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_S16;
  frame->sample_rate = 48000;
  frame->nb_samples = 128;
  av_channel_layout_default(&frame->ch_layout, 1);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate audio frame");
  }
  reinterpret_cast<std::int16_t*>(frame->data[0])[0] = sample;
  return frame;
}

AvDecoderReadResult Input(ffmpeg::Frame frame, std::int64_t pts = 12000) {
  frame->pts = pts;
  frame->pkt_dts = pts - 2;
  frame->best_effort_timestamp = pts + 1;
  frame->time_base = AVRational{1, 48000};
  frame->duration = 128;
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  result.stream_index = 7;
  result.time_base = AVRational{1, 90000};
  result.end_pts = 98765;
  return result;
}

void CheckTiming(const AvDecoderReadResult& output, std::int64_t pts = 12000,
                 int time_base_den = 90000) {
  REQUIRE(output.status == AvDecoderReadStatus::kFrame);
  REQUIRE(output.stream_index == 7);
  REQUIRE(output.time_base.num == 1);
  REQUIRE(output.time_base.den == time_base_den);
  REQUIRE(output.end_pts == 98765);
  REQUIRE(output.frame.has_value());
  REQUIRE((*output.frame)->pts == pts);
  REQUIRE((*output.frame)->pkt_dts == pts - 2);
  REQUIRE((*output.frame)->best_effort_timestamp == pts + 1);
  REQUIRE((*output.frame)->time_base.num == 1);
  REQUIRE((*output.frame)->time_base.den == 48000);
  REQUIRE((*output.frame)->duration == 128);
}

TEST_CASE("Business filters can explicitly copy source timing with new content",
          "[media_filter]") {
  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& frame) {
    auto output = Video(static_cast<std::uint8_t>(frame->data[0][0] + 3));
    output.CopyPropertiesFrom(frame);
    return output;
  });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) {
    auto output =
        Audio(reinterpret_cast<const std::int16_t*>(frame->data[0])[0] * 2);
    output.CopyPropertiesFrom(frame);
    return output;
  });
  filter.Start();
  auto video = filter.ProcessVideo(Input(Video()));
  auto audio = filter.ProcessAudio(Input(Audio()));
  CheckTiming(video, 12000, 48000);
  CheckTiming(audio, 12000, 48000);
  REQUIRE((*video.frame)->data[0][0] == 15);
  REQUIRE(reinterpret_cast<const std::int16_t*>((*audio.frame)->data[0])[0] ==
          68);
}

TEST_CASE("Business filters own every returned frame timing field",
          "[media_filter]") {
  const bool video = GENERATE(false, true);
  auto returned = video ? Video() : Audio();
  returned->pts = -96000;
  returned->pkt_dts = -97000;
  returned->best_effort_timestamp = -95000;
  returned->time_base = {1001, 30000};
  returned->duration = 19;
  AvMediaFilter filter;
  auto callback = [&](const ffmpeg::Frame&) { return returned.Ref(); };
  filter.SetOnVideo(callback);
  filter.SetOnAudio(callback);
  filter.Start();
  auto input = Input(video ? Video() : Audio());
  auto original = input.frame->Ref();
  auto output = video ? filter.ProcessVideo(std::move(input))
                      : filter.ProcessAudio(std::move(input));
  REQUIRE(output.status == AvDecoderReadStatus::kFrame);
  REQUIRE(output.stream_index == 7);
  REQUIRE(output.end_pts == 98765);
  REQUIRE(output.time_base.num == 1001);
  REQUIRE(output.time_base.den == 30000);
  REQUIRE((*output.frame)->pts == -96000);
  REQUIRE((*output.frame)->pkt_dts == -97000);
  REQUIRE((*output.frame)->best_effort_timestamp == -95000);
  REQUIRE((*output.frame)->time_base.num == 1001);
  REQUIRE((*output.frame)->time_base.den == 30000);
  REQUIRE((*output.frame)->duration == 19);
  REQUIRE(original->pts == 12000);
  REQUIRE(original->time_base.den == 48000);
  REQUIRE(original->duration == 128);
}

TEST_CASE("Business filters buffer one hundred frames without forwarding null",
          "[media_filter]") {
  const bool video = GENERATE(false, true);
  std::deque<ffmpeg::Frame> buffered;
  auto callback =
      [&](const ffmpeg::Frame& frame) -> std::optional<ffmpeg::Frame> {
    buffered.push_back(frame.Ref());
    if (buffered.size() <= 100) return std::nullopt;
    auto output = std::move(buffered.front());
    buffered.pop_front();
    return output;
  };
  AvMediaFilter filter;
  filter.SetOnVideo(callback);
  filter.SetOnAudio(callback);
  filter.Start();
  for (int index = 0; index < 103; ++index) {
    auto input = Input(video ? Video() : Audio(), 12000 + index * 128);
    auto output = video ? filter.ProcessVideo(std::move(input))
                        : filter.ProcessAudio(std::move(input));
    if (index < 100) {
      REQUIRE(output.status == AvDecoderReadStatus::kNull);
      REQUIRE_FALSE(output.frame);
    } else {
      CheckTiming(output, 12000 + (index - 100) * 128, 48000);
    }
    REQUIRE_FALSE(filter.error());
  }
}

TEST_CASE("Missing business callbacks pass through the original frames",
          "[media_filter]") {
  AvMediaFilter filter;
  filter.Start();
  auto video = Input(Video());
  auto audio = Input(Audio());
  auto* video_frame = video.frame->get();
  auto* audio_frame = audio.frame->get();
  auto video_output = filter.ProcessVideo(std::move(video));
  auto audio_output = filter.ProcessAudio(std::move(audio));
  REQUIRE(video_output.frame->get() == video_frame);
  REQUIRE(audio_output.frame->get() == audio_frame);
  CheckTiming(video_output);
  CheckTiming(audio_output);
}

TEST_CASE("Interleaved tracks retain business state until Stop and Start",
          "[media_filter]") {
  std::vector<std::string> calls;
  AvMediaFilter filter;
  filter.SetOnStart([&] { calls.emplace_back("start"); });
  filter.SetOnStop([&] { calls.emplace_back("stop"); });
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("video");
    return frame.Ref();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    calls.emplace_back("audio");
    return frame.Ref();
  });
  filter.Start();
  filter.ProcessVideo(Input(Video()));
  filter.ProcessAudio(Input(Audio()));
  filter.ProcessAudio(Input(Audio()));
  filter.ProcessVideo(Input(Video()));
  REQUIRE_NOTHROW(filter.ProcessAudio(Input(Audio())));
  filter.Stop();
  filter.Stop();
  filter.Start();
  filter.ProcessVideo(Input(Video()));
  filter.Stop();
  REQUIRE(calls == std::vector<std::string>{"start", "video", "audio", "audio",
                                            "video", "audio", "stop", "start",
                                            "video", "stop"});
}

TEST_CASE("PTS jumps do not reset or repair business-filter timestamps",
          "[media_filter]") {
  AvMediaFilter filter;
  int processed = 0;
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    auto output = Video(static_cast<std::uint8_t>(++processed));
    output.CopyPropertiesFrom(frame);
    return output;
  });
  filter.Start();
  for (const std::int64_t pts : {12000, 90000000, -300000, 100}) {
    auto output = filter.ProcessVideo(Input(Video(), pts));
    CheckTiming(output, pts, 48000);
    REQUIRE((*output.frame)->data[0][0] == processed);
  }
  REQUIRE(processed == 4);
}

TEST_CASE("EOF and underflow pass through without resetting or stopping",
          "[media_filter]") {
  int stops = 0;
  AvMediaFilter filter;
  filter.SetOnStop([&] { ++stops; });
  filter.Start();
  filter.ProcessVideo(Input(Video()));
  for (auto status : {AvDecoderReadStatus::kEof, AvDecoderReadStatus::kNull}) {
    AvDecoderReadResult input;
    input.status = status;
    input.stream_index = 3;
    input.end_pts = 600;
    input.time_base = AVRational{1, 25};
    auto output = filter.ProcessAudio(std::move(input));
    REQUIRE(output.status == status);
    REQUIRE(output.stream_index == 3);
    REQUIRE(output.end_pts == 600);
    REQUIRE(output.time_base.den == 25);
    REQUIRE_FALSE(output.frame.has_value());
  }
  auto output = filter.ProcessVideo(Input(Video()));
  CheckTiming(output);
  REQUIRE(stops == 0);
  filter.Stop();
  REQUIRE(stops == 1);
}

TEST_CASE("Callback failures require a fresh filter session",
          "[media_filter]") {
  class BusinessError final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
  };
  const auto failure =
      std::make_exception_ptr(BusinessError("Business processing failed"));
  int stops = 0;
  AvMediaFilter filter;
  REQUIRE_FALSE(filter.error());
  filter.SetOnStop([&] { ++stops; });
  filter.SetOnVideo([&](const ffmpeg::Frame&) -> ffmpeg::Frame {
    std::rethrow_exception(failure);
  });
  filter.Start();
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input(Video())), BusinessError);
  const auto error = filter.error();
  CheckException<BusinessError>(error, "Business processing failed");
  REQUIRE_THROWS_AS(filter.ProcessAudio(Input(Audio())), std::logic_error);
  REQUIRE(filter.error() == error);
  filter.Stop();
  REQUIRE(stops == 1);
  REQUIRE(filter.error() == error);
  filter.SetOnStart([] { throw std::runtime_error("Start failed"); });
  REQUIRE_THROWS_AS(filter.Start(), std::runtime_error);
  REQUIRE(filter.error() == error);
  filter.SetOnStart({});
  filter.SetOnVideo({});
  filter.Start();
  REQUIRE_FALSE(filter.error());
  auto output = filter.ProcessVideo(Input(Video()));
  CheckTiming(output);
}

TEST_CASE("Session callbacks have explicit lifecycle semantics",
          "[media_filter]") {
  AvMediaFilter filter;
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input(Video())), std::logic_error);
  filter.SetOnStart([] { throw std::runtime_error("Start failed"); });
  REQUIRE_THROWS_AS(filter.Start(), std::runtime_error);
  REQUIRE_FALSE(filter.error());
  filter.SetOnStart({});
  filter.Start();
  REQUIRE_THROWS_AS(filter.Start(), std::logic_error);
  REQUIRE_THROWS_AS(filter.SetOnVideo({}), std::logic_error);
  filter.Stop();

  int stops = 0;
  filter.SetOnStop([&] {
    ++stops;
    throw std::runtime_error("Stop failed");
  });
  filter.Start();
  REQUIRE_THROWS_AS(filter.Stop(), std::runtime_error);
  REQUIRE_FALSE(filter.error());
  REQUIRE_NOTHROW(filter.Stop());
  REQUIRE(stops == 1);
}

TEST_CASE("Frame consumers ignore invalid business callback output",
          "[media_filter]") {
  AvMediaFilter filter;
  int videos = 0;
  int audios = 0;
  filter.SetOnVideo([&](const ffmpeg::Frame&) {
    ++videos;
    return InvalidFrame();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame&) {
    ++audios;
    return InvalidFrame();
  });
  filter.Start();
  REQUIRE_NOTHROW(filter.ConsumeVideo(Input(Video())));
  REQUIRE_NOTHROW(filter.ConsumeAudio(Input(Audio())));
  REQUIRE(videos == 1);
  REQUIRE(audios == 1);
  REQUIRE_FALSE(filter.error());
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input(Video())), std::invalid_argument);
  REQUIRE(filter.error());
  REQUIRE_THROWS_AS(filter.ConsumeAudio(Input(Audio())), std::logic_error);
}

TEST_CASE("Consumer callback failures retain their original exception",
          "[media_filter]") {
  AvMediaFilter filter;
  const auto failure = std::make_exception_ptr(std::runtime_error("Consume"));
  filter.SetOnAudio([&](const ffmpeg::Frame&) -> ffmpeg::Frame {
    std::rethrow_exception(failure);
  });
  int stopped = 0;
  filter.SetOnStop([&] { ++stopped; });
  filter.Start();
  REQUIRE_THROWS_AS(filter.ConsumeAudio(Input(Audio())), std::runtime_error);
  const auto error = filter.error();
  CheckException<std::runtime_error>(error, "Consume");
  REQUIRE_THROWS_AS(filter.Reset(), std::logic_error);
  REQUIRE_NOTHROW(filter.Stop());
  REQUIRE(stopped == 1);
  REQUIRE(filter.error() == error);
}

TEST_CASE("Explicit reset clears business history without restarting filters",
          "[media_filter]") {
  AvMediaFilter filter;
  int starts = 0;
  int resets = 0;
  int history = 0;
  filter.SetOnStart([&] { ++starts; });
  filter.SetOnReset([&] {
    ++resets;
    history = 0;
  });
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    ++history;
    return frame.Ref();
  });
  REQUIRE_THROWS_AS(filter.Reset(), std::logic_error);
  filter.Start();
  REQUIRE_THROWS_AS(filter.SetOnReset({}), std::logic_error);
  filter.ConsumeVideo(Input(Video()));
  AvDecoderReadResult eof;
  eof.status = AvDecoderReadStatus::kEof;
  filter.ConsumeVideo(eof);
  REQUIRE(filter.ProcessAudio(eof).status == AvDecoderReadStatus::kEof);
  REQUIRE(resets == 0);
  REQUIRE(history == 1);
  filter.Reset();
  REQUIRE(resets == 1);
  REQUIRE(history == 0);
  filter.ConsumeVideo(Input(Video()));
  REQUIRE(history == 1);
  REQUIRE(starts == 1);
  filter.Stop();
  filter.SetOnReset({});
  filter.Start();
  REQUIRE_NOTHROW(filter.Reset());
}

TEST_CASE("Reset waits for an active consumer before invoking its callback",
          "[media_filter]") {
  AvMediaFilter filter;
  std::promise<void> entered;
  auto entry = entered.get_future();
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> consumed = false;
  bool consumed_at_reset = false;
  filter.SetOnVideo([&](const ffmpeg::Frame&) {
    entered.set_value();
    released.wait();
    consumed = true;
    return InvalidFrame();
  });
  filter.SetOnReset([&] { consumed_at_reset = consumed.load(); });
  filter.Start();
  auto consumer = std::async(std::launch::async,
                             [&] { filter.ConsumeVideo(Input(Video())); });
  const auto started = entry.wait_for(2s);
  auto reset = std::async(std::launch::async, [&] { filter.Reset(); });
  const auto pending = reset.wait_for(40ms);
  release.set_value();
  consumer.get();
  reset.get();
  REQUIRE(started == std::future_status::ready);
  REQUIRE(pending == std::future_status::timeout);
  REQUIRE(consumed_at_reset);
  REQUIRE_NOTHROW(filter.Stop());
}

TEST_CASE("Reset callback failures poison the session but still allow Stop",
          "[media_filter]") {
  AvMediaFilter filter;
  const auto failure = std::make_exception_ptr(std::runtime_error("Reset"));
  filter.SetOnReset([&] { std::rethrow_exception(failure); });
  int stops = 0;
  filter.SetOnStop([&] { ++stops; });
  filter.Start();
  REQUIRE_THROWS_AS(filter.Reset(), std::runtime_error);
  const auto error = filter.error();
  CheckException<std::runtime_error>(error, "Reset");
  REQUIRE_THROWS_AS(filter.ConsumeVideo(Input(Video())), std::logic_error);
  REQUIRE_THROWS_AS(filter.ProcessAudio(Input(Audio())), std::logic_error);
  REQUIRE_NOTHROW(filter.Stop());
  REQUIRE(stops == 1);
  REQUIRE(filter.error() == error);
  filter.SetOnReset({});
  filter.Start();
  REQUIRE_FALSE(filter.error());
  REQUIRE_NOTHROW(filter.Reset());
  REQUIRE_NOTHROW(filter.ConsumeVideo(Input(Video())));
}

TEST_CASE("Destruction stops an active session even when Stop throws",
          "[media_filter]") {
  int stops = 0;
  {
    AvMediaFilter filter;
    filter.SetOnStop([&] {
      ++stops;
      throw std::runtime_error("Stop failed");
    });
    filter.Start();
  }
  REQUIRE(stops == 1);
}

TEST_CASE("Audio processing completes while the video callback is blocked",
          "[media_filter]") {
  std::promise<void> video_entered;
  auto entered = video_entered.get_future();
  std::promise<void> release_video;
  auto released = release_video.get_future().share();
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    video_entered.set_value();
    released.wait();
    return frame.Ref();
  });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.Start();
  std::exception_ptr video_error;
  std::thread video([&] {
    try {
      filter.ProcessVideo(Input(Video()));
    } catch (...) {
      video_error = std::current_exception();
    }
  });
  const auto video_started = entered.wait_for(2s);
  std::promise<void> audio_completed;
  auto audio_completion = audio_completed.get_future();
  std::exception_ptr audio_error;
  std::thread audio([&] {
    try {
      filter.ProcessAudio(Input(Audio()));
    } catch (...) {
      audio_error = std::current_exception();
    }
    audio_completed.set_value();
  });
  const auto audio_while_video_blocked = audio_completion.wait_for(2s);
  release_video.set_value();
  video.join();
  audio.join();
  REQUIRE(video_started == std::future_status::ready);
  REQUIRE(audio_while_video_blocked == std::future_status::ready);
  REQUIRE_FALSE(video_error);
  REQUIRE_FALSE(audio_error);
}

TEST_CASE("Stop waits for both concurrent callbacks before OnStop",
          "[media_filter]") {
  std::promise<void> video_entered;
  std::promise<void> audio_entered;
  auto video_entry = video_entered.get_future();
  auto audio_entry = audio_entered.get_future();
  std::promise<void> release_video;
  std::promise<void> release_audio;
  auto video_release = release_video.get_future().share();
  auto audio_release = release_audio.get_future().share();
  std::atomic<int> completed_callbacks = 0;
  int callbacks_when_stopped = -1;
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    video_entered.set_value();
    video_release.wait();
    ++completed_callbacks;
    return frame.Ref();
  });
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    audio_entered.set_value();
    audio_release.wait();
    ++completed_callbacks;
    return frame.Ref();
  });
  filter.SetOnStop(
      [&] { callbacks_when_stopped = completed_callbacks.load(); });
  filter.Start();
  std::exception_ptr video_error;
  std::exception_ptr audio_error;
  std::exception_ptr stop_error;
  std::thread video([&] {
    try {
      filter.ProcessVideo(Input(Video()));
    } catch (...) {
      video_error = std::current_exception();
    }
  });
  const auto video_started = video_entry.wait_for(2s);
  std::thread audio([&] {
    try {
      filter.ProcessAudio(Input(Audio()));
    } catch (...) {
      audio_error = std::current_exception();
    }
  });
  const auto audio_started = audio_entry.wait_for(2s);
  std::promise<void> stop_attempted;
  auto stop_attempt = stop_attempted.get_future();
  std::promise<void> stop_completed;
  auto stop_completion = stop_completed.get_future();
  std::thread stop([&] {
    stop_attempted.set_value();
    try {
      filter.Stop();
    } catch (...) {
      stop_error = std::current_exception();
    }
    stop_completed.set_value();
  });
  stop_attempt.wait();
  const auto both_blocked = stop_completion.wait_for(40ms);
  bool new_process_rejected = false;
  std::exception_ptr new_process_error;
  std::promise<void> new_process_completed;
  auto new_process_completion = new_process_completed.get_future();
  std::thread new_process([&] {
    try {
      filter.ProcessAudio(AvDecoderReadResult{});
    } catch (const std::logic_error&) {
      new_process_rejected = true;
    } catch (...) {
      new_process_error = std::current_exception();
    }
    new_process_completed.set_value();
  });
  const auto rejected_while_stopping = new_process_completion.wait_for(2s);
  release_video.set_value();
  video.join();
  const auto audio_still_blocked = stop_completion.wait_for(40ms);
  release_audio.set_value();
  audio.join();
  stop.join();
  new_process.join();
  REQUIRE(video_started == std::future_status::ready);
  REQUIRE(audio_started == std::future_status::ready);
  REQUIRE(both_blocked == std::future_status::timeout);
  REQUIRE(audio_still_blocked == std::future_status::timeout);
  REQUIRE(rejected_while_stopping == std::future_status::ready);
  REQUIRE(new_process_rejected);
  REQUIRE_FALSE(new_process_error);
  REQUIRE_FALSE(video_error);
  REQUIRE_FALSE(audio_error);
  REQUIRE_FALSE(stop_error);
  REQUIRE(callbacks_when_stopped == 2);
}

TEST_CASE("A failing callback leaves the concurrent callback able to finish",
          "[media_filter]") {
  std::promise<void> audio_entered;
  auto audio_entry = audio_entered.get_future();
  std::promise<void> release_audio;
  auto audio_release = release_audio.get_future().share();
  AvMediaFilter filter;
  filter.SetOnAudio([&](const ffmpeg::Frame& frame) {
    audio_entered.set_value();
    audio_release.wait();
    return frame.Ref();
  });
  filter.SetOnVideo([](const ffmpeg::Frame&) -> ffmpeg::Frame {
    throw std::runtime_error("Video callback failed");
  });
  filter.Start();
  std::exception_ptr audio_error;
  std::exception_ptr video_error;
  std::exception_ptr stop_error;
  std::thread audio([&] {
    try {
      filter.ProcessAudio(Input(Audio()));
    } catch (...) {
      audio_error = std::current_exception();
    }
  });
  const auto audio_started = audio_entry.wait_for(2s);
  std::promise<void> video_completed;
  auto video_completion = video_completed.get_future();
  std::thread video([&] {
    try {
      filter.ProcessVideo(Input(Video()));
    } catch (...) {
      video_error = std::current_exception();
    }
    video_completed.set_value();
  });
  const auto video_failed_while_audio_blocked = video_completion.wait_for(2s);
  std::promise<void> stop_completed;
  auto stop_completion = stop_completed.get_future();
  std::thread stop([&] {
    try {
      filter.Stop();
    } catch (...) {
      stop_error = std::current_exception();
    }
    stop_completed.set_value();
  });
  const auto stop_while_audio_blocked = stop_completion.wait_for(40ms);
  release_audio.set_value();
  audio.join();
  video.join();
  stop.join();
  REQUIRE(audio_started == std::future_status::ready);
  REQUIRE(video_failed_while_audio_blocked == std::future_status::ready);
  REQUIRE(stop_while_audio_blocked == std::future_status::timeout);
  REQUIRE_FALSE(audio_error);
  CheckException<std::runtime_error>(video_error, "Video callback failed");
  CheckException<std::runtime_error>(filter.error(), "Video callback failed");
  REQUIRE_FALSE(stop_error);
  filter.SetOnAudio({});
  REQUIRE_NOTHROW(filter.Start());
  REQUIRE_FALSE(filter.error());
  REQUIRE_NOTHROW(filter.ProcessAudio(Input(Audio())));
}

TEST_CASE("CUDA business filters retain hardware surfaces and input timing",
          "[.][media_filter][cuda]") {
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  std::unique_ptr<AVBufferRef, void (*)(AVBufferRef*)> frames(
      av_hwframe_ctx_alloc(device.get()),
      [](AVBufferRef* reference) { av_buffer_unref(&reference); });
  REQUIRE(frames != nullptr);
  auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = AV_PIX_FMT_NV12;
  context->width = 128;
  context->height = 128;
  REQUIRE(av_hwframe_ctx_init(frames.get()) >= 0);
  ffmpeg::Frame frame;
  REQUIRE(av_hwframe_get_buffer(frames.get(), frame.get(), 0) >= 0);
  const auto* surface = frame->data[0];
  const auto* frame_context = frame->hw_frames_ctx->data;

  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& input) { return input.Ref(); });
  filter.Start();
  auto output = filter.ProcessVideo(Input(std::move(frame)));
  CheckTiming(output, 12000, 48000);
  REQUIRE((*output.frame)->format == AV_PIX_FMT_CUDA);
  REQUIRE((*output.frame)->data[0] == surface);
  REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
  output = filter.ProcessVideo(std::move(output));
  filter.Stop();
  CheckTiming(output, 12000, 48000);
  REQUIRE((*output.frame)->data[0] == surface);
  REQUIRE((*output.frame)->hw_frames_ctx->data == frame_context);
}

TEST_CASE("CUDA video callbacks reject missing hardware context before calling",
          "[media_filter]") {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_CUDA;
  bool called = false;
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    called = true;
    return input.Ref();
  });
  filter.Start();
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input(std::move(frame))),
                    std::invalid_argument);
  REQUIRE_FALSE(called);
  REQUIRE(filter.error());
  filter.Stop();
}

TEST_CASE("CUDA video callbacks bind frame context and restore the caller",
          "[.][media_filter][cuda]") {
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  ffmpeg::HwDeviceContext other(ffmpeg::HwDeviceType::kCuda);
  const auto* native_device =
      reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
  const auto* native_other =
      reinterpret_cast<const AVHWDeviceContext*>(other.get()->data);
  const auto expected =
      static_cast<const AVCUDADeviceContext*>(native_device->hwctx)->cuda_ctx;
  const auto previous =
      static_cast<const AVCUDADeviceContext*>(native_other->hwctx)->cuda_ctx;
  REQUIRE(expected != previous);
  std::unique_ptr<AVBufferRef, void (*)(AVBufferRef*)> frames(
      av_hwframe_ctx_alloc(device.get()),
      [](AVBufferRef* reference) { av_buffer_unref(&reference); });
  REQUIRE(frames != nullptr);
  auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = AV_PIX_FMT_NV12;
  context->width = 128;
  context->height = 128;
  REQUIRE(av_hwframe_ctx_init(frames.get()) >= 0);
  ffmpeg::Frame frame;
  REQUIRE(av_hwframe_get_buffer(frames.get(), frame.get(), 0) >= 0);
  const TestCudaDriver driver;
  bool fail = false;
  bool switch_context = false;
  CUcontext observed = nullptr;
  CUcontext switched = nullptr;
  CUcontext returned = nullptr;
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    if (driver.current(&observed) != CUDA_SUCCESS) {
      throw std::runtime_error("Cannot query callback CUDA context");
    }
    if (switch_context) {
      if (driver.set(previous) != CUDA_SUCCESS ||
          driver.current(&switched) != CUDA_SUCCESS ||
          driver.set(expected) != CUDA_SUCCESS ||
          driver.current(&returned) != CUDA_SUCCESS) {
        throw std::runtime_error("Cannot switch callback CUDA context");
      }
    }
    if (fail) throw std::runtime_error("Video callback failed");
    return input.Ref();
  });
  filter.Start();

  // A fresh scheduler-like worker starts without a CUDA context. Binding must
  // cover the callback and restore this empty state afterward.
  auto worker = std::async(std::launch::async, [&] {
    CUcontext before = nullptr;
    CUcontext after = nullptr;
    const auto queried_before = driver.current(&before);
    auto output = filter.ProcessVideo(Input(frame.Ref()));
    const auto queried_after = driver.current(&after);
    return queried_before == CUDA_SUCCESS && queried_after == CUDA_SUCCESS &&
           before == nullptr && after == before && output.frame.has_value();
  });
  REQUIRE(worker.get());
  REQUIRE(observed == expected);

  // A caller may already own a different current context. Both consumption
  // and throwing callbacks must preserve that exact context.
  REQUIRE(driver.push(previous) == CUDA_SUCCESS);
  struct RestoreContext final {
    const TestCudaDriver& driver;
    ~RestoreContext() {
      CUcontext ignored = nullptr;
      driver.pop(&ignored);
    }
  } restore{driver};
  // Business processing may temporarily use another context. Rebinding the
  // frame context before conversion must preserve the caller's context stack.
  switch_context = true;
  REQUIRE_NOTHROW(filter.ConsumeVideo(Input(frame.Ref())));
  REQUIRE(observed == expected);
  REQUIRE(switched == previous);
  REQUIRE(returned == expected);
  CUcontext restored = nullptr;
  REQUIRE(driver.current(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == previous);
  switched = nullptr;
  returned = nullptr;
  fail = true;
  REQUIRE_THROWS_MATCHES(filter.ProcessVideo(Input(frame.Ref())),
                         std::runtime_error,
                         Catch::Matchers::Message("Video callback failed"));
  REQUIRE(observed == expected);
  REQUIRE(switched == previous);
  REQUIRE(returned == expected);
  REQUIRE(driver.current(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == previous);
  CheckException<std::runtime_error>(filter.error(), "Video callback failed");
  filter.Stop();
}

TEST_CASE("CUDA lifecycle callbacks retain session context and restore callers",
          "[.][media_filter][cuda]") {
  // 0: success, 1: reset failure, 2: stop failure, 3: video failure.
  auto failure = GENERATE(0, 1, 2, 3);
  const auto different_caller_context = GENERATE(false, true);
  CAPTURE(failure, different_caller_context);
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  ffmpeg::HwDeviceContext other(ffmpeg::HwDeviceType::kCuda);
  const auto native_context = [](const ffmpeg::HwDeviceContext& hardware) {
    const auto* native =
        reinterpret_cast<const AVHWDeviceContext*>(hardware.get()->data);
    return static_cast<const AVCUDADeviceContext*>(native->hwctx)->cuda_ctx;
  };
  const auto expected = native_context(device);
  const auto previous =
      different_caller_context ? native_context(other) : nullptr;
  const auto cuda_frame = [](ffmpeg::HwDeviceContext& hardware) {
    std::unique_ptr<AVBufferRef, void (*)(AVBufferRef*)> frames(
        av_hwframe_ctx_alloc(hardware.get()),
        [](AVBufferRef* reference) { av_buffer_unref(&reference); });
    if (!frames) throw std::runtime_error("Cannot allocate CUDA frame context");
    auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
    context->format = AV_PIX_FMT_CUDA;
    context->sw_format = AV_PIX_FMT_NV12;
    context->width = 128;
    context->height = 128;
    ffmpeg::Frame frame;
    if (av_hwframe_ctx_init(frames.get()) < 0 ||
        av_hwframe_get_buffer(frames.get(), frame.get(), 0) < 0) {
      throw std::runtime_error("Cannot allocate CUDA frame");
    }
    return frame;
  };
  const TestCudaDriver driver;
  CUcontext reset_context = nullptr;
  CUcontext stop_context = nullptr;
  int stops = 0;
  AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    if (failure == 3) throw std::runtime_error("Video callback failed");
    return input.Ref();
  });
  filter.SetOnReset([&] {
    if (driver.current(&reset_context) != CUDA_SUCCESS) {
      throw std::runtime_error("Cannot query reset CUDA context");
    }
    if (failure == 1) throw std::runtime_error("Reset callback failed");
  });
  filter.SetOnStop([&] {
    ++stops;
    if (driver.current(&stop_context) != CUDA_SUCCESS) {
      throw std::runtime_error("Cannot query stop CUDA context");
    }
    if (failure == 2) throw std::runtime_error("Stop callback failed");
  });
  // Lifecycle calls run on a fresh thread, with either no current context or
  // a different context. Check restoration after both return and exception.
  const auto invoke = [&](auto operation) {
    return std::async(
               std::launch::async,
               [&, operation] {
                 if (driver.set(previous) != CUDA_SUCCESS) {
                   throw std::runtime_error("Cannot set caller CUDA context");
                 }
                 std::exception_ptr error;
                 try {
                   operation();
                 } catch (...) {
                   error = std::current_exception();
                 }
                 CUcontext after = nullptr;
                 const auto queried = driver.current(&after);
                 const auto cleared = driver.set(nullptr);
                 return std::make_pair(error, queried == CUDA_SUCCESS &&
                                                  after == previous &&
                                                  cleared == CUDA_SUCCESS);
               })
        .get();
  };
  REQUIRE(av_buffer_get_ref_count(device.get()) == 1);
  filter.Start();
  if (failure == 3) {
    REQUIRE_THROWS_MATCHES(filter.ConsumeVideo(Input(cuda_frame(device))),
                           std::runtime_error,
                           Catch::Matchers::Message("Video callback failed"));
  } else {
    filter.ConsumeVideo(Input(cuda_frame(device)));
  }
  // No input frame or frame pool remains. The session must own its device.
  REQUIRE(av_buffer_get_ref_count(device.get()) == 2);
  if (failure != 3) {
    const auto reset = invoke([&] { filter.Reset(); });
    REQUIRE(reset.second);
    REQUIRE(reset_context == expected);
    if (failure == 1) {
      CheckException<std::runtime_error>(reset.first, "Reset callback failed");
      CheckException<std::runtime_error>(filter.error(),
                                         "Reset callback failed");
    } else {
      REQUIRE_FALSE(reset.first);
      REQUIRE_NOTHROW(filter.ConsumeVideo(Input(cuda_frame(device))));
    }
    REQUIRE(av_buffer_get_ref_count(device.get()) == 2);
  }
  const auto stopped = invoke([&] { filter.Stop(); });
  REQUIRE(stopped.second);
  REQUIRE(stop_context == expected);
  if (failure == 2) {
    CheckException<std::runtime_error>(stopped.first, "Stop callback failed");
  } else {
    REQUIRE_FALSE(stopped.first);
  }
  REQUIRE(av_buffer_get_ref_count(device.get()) == 1);
  REQUIRE_NOTHROW(filter.Stop());
  REQUIRE(stops == 1);

  // A session with no CUDA video processing must not inherit the old context.
  failure = 0;
  filter.Start();
  const auto empty_reset = invoke([&] { filter.Reset(); });
  REQUIRE_FALSE(empty_reset.first);
  REQUIRE(empty_reset.second);
  REQUIRE(reset_context == previous);
  const auto empty_stop = invoke([&] { filter.Stop(); });
  REQUIRE_FALSE(empty_stop.first);
  REQUIRE(empty_stop.second);
  REQUIRE(stop_context == previous);

  // The next CUDA session can use another context without retaining the old
  // device, even if the previous session ended through a callback exception.
  filter.Start();
  filter.ConsumeVideo(Input(cuda_frame(other)));
  REQUIRE(av_buffer_get_ref_count(device.get()) == 1);
  REQUIRE(av_buffer_get_ref_count(other.get()) == 2);
  const auto restarted_stop = invoke([&] { filter.Stop(); });
  REQUIRE_FALSE(restarted_stop.first);
  REQUIRE(restarted_stop.second);
  REQUIRE(stop_context == native_context(other));
  REQUIRE(av_buffer_get_ref_count(other.get()) == 1);
}

}  // namespace
