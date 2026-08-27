#include "mw/output/internal/output_sink_worker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::ffmpeg::Frame;
using mw::streamer::output::OutputSink;
using mw::streamer::output::internal::OutputSinkWorker;

Frame CreateVideoFrame(std::int64_t pts) {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 64;
  frame->height = 32;
  frame->pts = pts;
  frame->duration = 1;
  frame->time_base = {1, 25};
  REQUIRE(av_frame_get_buffer(frame.get(), 32) >= 0);
  frame->data[0][0] = 42;
  return frame;
}

Frame CreateAudioFrame(std::int64_t pts) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 16;
  frame->pts = pts;
  frame->duration = 16;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  reinterpret_cast<float*>(frame->data[0])[0] = 0.25F;
  return frame;
}

struct LifecycleState {
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  std::thread::id worker_thread;
  std::thread::id stop_thread;
};

class TestOutputSink final : public OutputSink {
 public:
  using OnAudio = std::function<void(Frame)>;
  using OnVideo = std::function<void(Frame)>;

  TestOutputSink(std::shared_ptr<LifecycleState> lifecycle,
                 OnAudio on_audio = {}, OnVideo on_video = {})
      : lifecycle_(std::move(lifecycle)),
        on_audio_(std::move(on_audio)),
        on_video_(std::move(on_video)) {}

  void Start() override {
    lifecycle_->starts.fetch_add(1, std::memory_order_relaxed);
    lifecycle_->worker_thread = std::this_thread::get_id();
  }

  void WriteAudio(Frame frame) override {
    if (on_audio_) {
      on_audio_(std::move(frame));
    }
  }

  void WriteVideo(Frame frame) override {
    if (on_video_) {
      on_video_(std::move(frame));
    }
  }

  void Stop() noexcept override {
    lifecycle_->stops.fetch_add(1, std::memory_order_relaxed);
    lifecycle_->stop_thread = std::this_thread::get_id();
  }

 private:
  std::shared_ptr<LifecycleState> lifecycle_;
  OnAudio on_audio_;
  OnVideo on_video_;
};

class FailingStartSink final : public OutputSink {
 public:
  explicit FailingStartSink(std::shared_ptr<LifecycleState> lifecycle)
      : lifecycle_(std::move(lifecycle)) {}

  void Start() override {
    lifecycle_->starts.fetch_add(1, std::memory_order_relaxed);
    lifecycle_->worker_thread = std::this_thread::get_id();
    throw std::runtime_error("output sink start failed");
  }

  void WriteAudio(Frame) override {}
  void WriteVideo(Frame) override {}

  void Stop() noexcept override {
    lifecycle_->stops.fetch_add(1, std::memory_order_relaxed);
    lifecycle_->stop_thread = std::this_thread::get_id();
  }

 private:
  std::shared_ptr<LifecycleState> lifecycle_;
};

void CheckLifecycle(const LifecycleState& lifecycle) {
  CHECK(lifecycle.starts.load(std::memory_order_relaxed) == 1);
  CHECK(lifecycle.stops.load(std::memory_order_relaxed) == 1);
  CHECK(lifecycle.stop_thread == lifecycle.worker_thread);
}

}  // namespace

TEST_CASE("OutputSinkWorker在独立线程转交可持有的音视频帧") {
  const auto caller_thread = std::this_thread::get_id();
  auto lifecycle = std::make_shared<LifecycleState>();
  std::thread::id callback_thread;
  std::int64_t video_pts = AV_NOPTS_VALUE;
  std::int64_t audio_pts = AV_NOPTS_VALUE;
  std::uint32_t audio_channels = 0;
  std::optional<Frame> retained_video;
  std::optional<Frame> retained_audio;
  std::uint8_t* video_data = nullptr;
  std::uint8_t* audio_data = nullptr;
  auto output = std::make_unique<TestOutputSink>(
      lifecycle,
      [&](Frame frame) {
        callback_thread = std::this_thread::get_id();
        audio_pts = frame->pts;
        audio_channels = frame->ch_layout.nb_channels;
        retained_audio.emplace(std::move(frame));
      },
      [&](Frame frame) {
        callback_thread = std::this_thread::get_id();
        video_pts = frame->pts;
        retained_video.emplace(std::move(frame));
      });
  OutputSinkWorker worker(4, std::move(output));
  worker.Start();

  {
    auto video = CreateVideoFrame(7);
    auto audio = CreateAudioFrame(32);
    video_data = video->data[0];
    audio_data = audio->data[0];
    REQUIRE(worker.WriteVideo(video));
    REQUIRE(worker.WriteAudio(audio));
  }
  worker.RequestFinish();
  worker.Stop();

  CHECK(callback_thread != caller_thread);
  CHECK(callback_thread == lifecycle->worker_thread);
  CHECK(video_pts == 7);
  CHECK(audio_pts == 32);
  CHECK(audio_channels == 2);
  REQUIRE(retained_video);
  REQUIRE(retained_audio);
  REQUIRE((*retained_video)->buf[0] != nullptr);
  REQUIRE((*retained_audio)->buf[0] != nullptr);
  CHECK((*retained_video)->data[0] == video_data);
  CHECK((*retained_audio)->data[0] == audio_data);
  CHECK((*retained_video)->data[0][0] == 42);
  CHECK(reinterpret_cast<float*>((*retained_audio)->data[0])[0] == 0.25F);
  CHECK(av_buffer_get_ref_count((*retained_video)->buf[0]) >= 1);
  CHECK(av_buffer_get_ref_count((*retained_audio)->buf[0]) >= 1);
  CHECK(worker.dropped_frames() == 0);
  CheckLifecycle(*lifecycle);
}

TEST_CASE("OutputSinkWorker满队列时丢弃本分支积压并接收当前帧") {
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::vector<std::int64_t> pts;
  auto lifecycle = std::make_shared<LifecycleState>();
  auto output = std::make_unique<TestOutputSink>(
      lifecycle, TestOutputSink::OnAudio{}, [&](Frame frame) {
        pts.push_back(frame->pts);
        if (pts.size() == 1) {
          entered.set_value();
          release_future.wait();
        }
      });
  OutputSinkWorker worker(2, std::move(output));
  worker.Start();

  REQUIRE(worker.WriteVideo(CreateVideoFrame(1)));
  REQUIRE(entered_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(worker.WriteVideo(CreateVideoFrame(2)));
  REQUIRE(worker.WriteVideo(CreateVideoFrame(3)));
  REQUIRE(worker.WriteVideo(CreateVideoFrame(4)));
  release.set_value();
  worker.RequestFinish();
  worker.Stop();

  REQUIRE(pts.size() == 2);
  CHECK(pts[0] == 1);
  CHECK(pts[1] == 4);
  CHECK(worker.dropped_frames() == 2);
  CheckLifecycle(*lifecycle);
}

TEST_CASE("OutputSinkWorker音频到达满队列时丢弃陈旧音视频") {
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::vector<std::string> delivered;
  auto lifecycle = std::make_shared<LifecycleState>();
  auto output = std::make_unique<TestOutputSink>(
      lifecycle,
      [&](Frame frame) {
        delivered.push_back("audio:" + std::to_string(frame->pts));
      },
      [&](Frame frame) {
        delivered.push_back("video:" + std::to_string(frame->pts));
        if (delivered.size() == 1) {
          entered.set_value();
          release_future.wait();
        }
      });
  OutputSinkWorker worker(2, std::move(output));
  worker.Start();

  REQUIRE(worker.WriteVideo(CreateVideoFrame(1)));
  REQUIRE(entered_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(worker.WriteAudio(CreateAudioFrame(2)));
  REQUIRE(worker.WriteVideo(CreateVideoFrame(3)));
  REQUIRE(worker.WriteAudio(CreateAudioFrame(4)));
  release.set_value();
  worker.RequestFinish();
  worker.Stop();

  CHECK(delivered == std::vector<std::string>{"video:1", "audio:4"});
  CHECK(worker.dropped_frames() == 2);
  CheckLifecycle(*lifecycle);
}

TEST_CASE("OutputSinkWorker慢分支拥塞不影响其他分支") {
  std::promise<void> slow_entered;
  auto slow_entered_future = slow_entered.get_future();
  std::promise<void> release_slow;
  auto release_slow_future = release_slow.get_future().share();
  std::vector<std::int64_t> slow_pts;
  std::vector<std::int64_t> fast_pts;
  auto slow_lifecycle = std::make_shared<LifecycleState>();
  auto fast_lifecycle = std::make_shared<LifecycleState>();

  auto slow_sink = std::make_unique<TestOutputSink>(
      slow_lifecycle, TestOutputSink::OnAudio{}, [&](Frame frame) {
        slow_pts.push_back(frame->pts);
        if (slow_pts.size() == 1) {
          slow_entered.set_value();
          release_slow_future.wait();
        }
      });
  auto fast_sink = std::make_unique<TestOutputSink>(
      fast_lifecycle, TestOutputSink::OnAudio{},
      [&](Frame frame) { fast_pts.push_back(frame->pts); });
  OutputSinkWorker slow_worker(2, std::move(slow_sink));
  OutputSinkWorker fast_worker(8, std::move(fast_sink));
  slow_worker.Start();
  fast_worker.Start();

  auto first = CreateVideoFrame(1);
  REQUIRE(slow_worker.WriteVideo(first));
  REQUIRE(fast_worker.WriteVideo(first));
  REQUIRE(slow_entered_future.wait_for(1s) == std::future_status::ready);
  for (std::int64_t pts = 2; pts <= 4; ++pts) {
    auto frame = CreateVideoFrame(pts);
    REQUIRE(slow_worker.WriteVideo(frame));
    REQUIRE(fast_worker.WriteVideo(frame));
  }

  fast_worker.RequestFinish();
  fast_worker.Stop();
  CHECK(fast_pts == std::vector<std::int64_t>{1, 2, 3, 4});
  CHECK(fast_worker.dropped_frames() == 0);

  release_slow.set_value();
  slow_worker.RequestFinish();
  slow_worker.Stop();
  CHECK(slow_pts == std::vector<std::int64_t>{1, 4});
  CHECK(slow_worker.dropped_frames() == 2);
  CheckLifecycle(*slow_lifecycle);
  CheckLifecycle(*fast_lifecycle);
}

TEST_CASE("OutputSinkWorker的Abort清空积压并等待活动回调") {
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::vector<std::int64_t> pts;
  auto lifecycle = std::make_shared<LifecycleState>();
  auto output = std::make_unique<TestOutputSink>(
      lifecycle, TestOutputSink::OnAudio{}, [&](Frame frame) {
        pts.push_back(frame->pts);
        if (pts.size() == 1) {
          entered.set_value();
          release_future.wait();
        }
      });
  OutputSinkWorker worker(4, std::move(output));
  worker.Start();

  REQUIRE(worker.WriteVideo(CreateVideoFrame(1)));
  REQUIRE(entered_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(worker.WriteVideo(CreateVideoFrame(2)));
  REQUIRE(worker.WriteVideo(CreateVideoFrame(3)));

  auto abort = std::async(std::launch::async, [&worker]() { worker.Abort(); });
  CHECK(abort.wait_for(20ms) == std::future_status::timeout);
  release.set_value();
  REQUIRE(abort.wait_for(1s) == std::future_status::ready);

  CHECK(pts == std::vector<std::int64_t>{1});
  CHECK(worker.dropped_frames() == 2);
  CHECK_FALSE(worker.WriteVideo(CreateVideoFrame(4)));
  CheckLifecycle(*lifecycle);
}

TEST_CASE("OutputSinkWorker报告启动完成和媒体写入失败") {
  SECTION("RequestFinish排空后报告完成") {
    auto lifecycle = std::make_shared<LifecycleState>();
    std::atomic<bool> ready = false;
    std::atomic<bool> completed = false;
    std::atomic<bool> failed = false;
    auto output = std::make_unique<TestOutputSink>(lifecycle);
    OutputSinkWorker worker(2, std::move(output),
                            {[&ready]() { ready.store(true); },
                             [&completed]() { completed.store(true); },
                             [&failed](const char*) { failed.store(true); }});

    worker.Start();
    REQUIRE(worker.WriteVideo(CreateVideoFrame(1)));
    worker.RequestFinish();
    worker.Stop();

    CHECK(ready.load());
    CHECK(completed.load());
    CHECK_FALSE(failed.load());
    CheckLifecycle(*lifecycle);
  }

  SECTION("媒体写入异常报告失败且不报告完成") {
    auto lifecycle = std::make_shared<LifecycleState>();
    std::promise<std::string> failed;
    auto failure = failed.get_future();
    std::atomic<bool> completed = false;
    auto output = std::make_unique<TestOutputSink>(
        lifecycle, TestOutputSink::OnAudio{},
        [](Frame) { throw std::runtime_error("output sink failed"); });
    OutputSinkWorker worker(
        2, std::move(output),
        {{},
         [&completed]() { completed.store(true); },
         [&failed](const char* error) { failed.set_value(error); }});

    worker.Start();
    REQUIRE(worker.WriteVideo(CreateVideoFrame(1)));
    REQUIRE(failure.wait_for(1s) == std::future_status::ready);
    worker.Stop();

    CHECK(failure.get() == "output sink failed");
    CHECK_FALSE(completed.load());
    CheckLifecycle(*lifecycle);
  }

  SECTION("Sink启动异常报告失败并执行一次Stop") {
    auto lifecycle = std::make_shared<LifecycleState>();
    std::promise<std::string> failed;
    auto failure = failed.get_future();
    std::atomic<bool> ready = false;
    std::atomic<bool> completed = false;
    auto output = std::make_unique<FailingStartSink>(lifecycle);
    OutputSinkWorker worker(
        2, std::move(output),
        {[&ready]() { ready.store(true); },
         [&completed]() { completed.store(true); },
         [&failed](const char* error) { failed.set_value(error); }});

    worker.Start();
    REQUIRE(failure.wait_for(1s) == std::future_status::ready);
    worker.Stop();

    CHECK(failure.get() == "output sink start failed");
    CHECK_FALSE(ready.load());
    CHECK_FALSE(completed.load());
    CHECK_FALSE(worker.WriteVideo(CreateVideoFrame(1)));
    CheckLifecycle(*lifecycle);
  }
}

TEST_CASE("OutputSinkWorker校验构造和生命周期边界") {
  auto lifecycle = std::make_shared<LifecycleState>();
  CHECK_THROWS_AS(
      OutputSinkWorker(0, std::make_unique<TestOutputSink>(lifecycle)),
      std::invalid_argument);
  CHECK_THROWS_AS(OutputSinkWorker(1, std::unique_ptr<OutputSink>{}),
                  std::invalid_argument);

  OutputSinkWorker worker(1, std::make_unique<TestOutputSink>(lifecycle));
  CHECK_FALSE(worker.WriteAudio(CreateAudioFrame(1)));
  CHECK_FALSE(worker.WriteVideo(CreateVideoFrame(1)));
  worker.Start();
  CHECK_THROWS_AS(worker.Start(), std::logic_error);
  worker.RequestFinish();
  CHECK_FALSE(worker.WriteAudio(CreateAudioFrame(2)));
  worker.Stop();
  worker.Stop();
  CheckLifecycle(*lifecycle);
}
