#include "mw/streamer/output/output.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace mw::streamer;

template <typename T>
std::future<T> Ready(T value) {
  std::promise<T> promise;
  promise.set_value(std::move(value));
  return promise.get_future();
}

class TestFrameSink final : public FrameSink {
 public:
  ~TestFrameSink() override {
    if (on_destroy_) on_destroy_();
  }
  bool Initialize(const AvEncoderConfig& format) noexcept override {
    ++initializes_;
    if (on_initialize_) on_initialize_(format);
    return !initialize_error_;
  }
  std::future<bool> Send(OutputFrame frame) override {
    ++sends_;
    frames_.push_back(std::move(frame));
    return on_send_ ? on_send_() : Ready(true);
  }
  bool has_synchronization_clock() const noexcept override { return clock_; }

  bool initialize_error_ = false;
  bool clock_ = false;
  std::atomic<int> initializes_{0};
  std::atomic<int> sends_{0};
  std::function<void(const AvEncoderConfig&)> on_initialize_;
  std::function<std::future<bool>()> on_send_;
  std::function<void()> on_destroy_;
  std::vector<OutputFrame> frames_;
};

AvEncoderConfig Format() {
  AvEncoderConfig format;
  format.width = 1920;
  format.height = 1080;
  format.video_pixel_format = AV_PIX_FMT_BGRA;
  return format;
}

TEST_CASE("Output fans out before waiting for frame sinks",
          "[output][fanout]") {
  auto first = std::make_shared<TestFrameSink>();
  auto second = std::make_shared<TestFrameSink>();
  std::promise<bool> accepted;
  std::atomic<bool> released{false};
  const auto release = [&] {
    if (!released.exchange(true)) accepted.set_value(true);
  };
  first->on_send_ = [&] { return accepted.get_future(); };
  second->on_send_ = [&] {
    release();
    return Ready(true);
  };
  Output output({first, second});
  REQUIRE(output.Start(Format()));
  std::atomic<bool> cancelled{false};
  auto operation = std::async(std::launch::async,
                              [&] { return output.Send({}, &cancelled); });
  const bool ready = operation.wait_for(2s) == std::future_status::ready;
  if (!ready) cancelled = true;
  REQUIRE(operation.wait_for(2s) == std::future_status::ready);
  CHECK(operation.get());
  REQUIRE(ready);
  CHECK(first->sends_ == 1);
  CHECK(second->sends_ == 1);
}

TEST_CASE("Output retains media buffers with independent frame headers",
          "[output][ownership]") {
  auto first = std::make_shared<TestFrameSink>();
  auto second = std::make_shared<TestFrameSink>();
  Output output({first, second});
  REQUIRE(output.Start(Format()));
  OutputFrame frame;
  frame.frame.video.emplace();
  auto* video = frame.frame.video->get();
  video->format = AV_PIX_FMT_GRAY8;
  video->width = 8;
  video->height = 8;
  video->pts = 17;
  REQUIRE(av_frame_get_buffer(video, 0) == 0);
  frame.frame_number = 42;
  frame.audio_sample_index = 67200;
  frame.audio_samples = 1600;
  output.Send(frame);
  REQUIRE(first->frames_.size() == 1);
  REQUIRE(second->frames_.size() == 1);
  auto* first_video = first->frames_.front().frame.video->get();
  auto* second_video = second->frames_.front().frame.video->get();
  CHECK(first_video != video);
  CHECK(second_video != first_video);
  CHECK(first_video->data[0] == video->data[0]);
  CHECK(second_video->data[0] == video->data[0]);
  first_video->pts = 999;
  CHECK(video->pts == 17);
  CHECK(second_video->pts == 17);
  CHECK(second->frames_.front().frame_number == 42);
  CHECK(second->frames_.front().audio_sample_index == 67200);
  CHECK(second->frames_.front().audio_samples == 1600);
}

TEST_CASE("Output isolates rejected and throwing frame sinks",
          "[output][failure]") {
  auto failing = std::make_shared<TestFrameSink>();
  auto healthy = std::make_shared<TestFrameSink>();
  SECTION("rejected frame") {
    failing->on_send_ = [] { return Ready(false); };
  }
  SECTION("synchronous exception") {
    failing->on_send_ = []() -> std::future<bool> {
      throw std::runtime_error("send failure");
    };
  }
  SECTION("future exception") {
    failing->on_send_ = [] {
      std::promise<bool> promise;
      promise.set_exception(
          std::make_exception_ptr(std::runtime_error("async send failure")));
      return promise.get_future();
    };
  }
  Output output({failing, healthy});
  REQUIRE(output.Start(Format()));
  output.Send({});
  output.Send({});
  CHECK(failing->sends_ == 1);
  CHECK(healthy->sends_ == 2);
  CHECK(output.status().frame_sinks[0].state == FrameSinkState::kFailed);
  CHECK(output.status().frame_sinks[1].state == FrameSinkState::kRunning);
}

TEST_CASE("Output requires every frame sink to initialize and retries failures",
          "[output][failure][clock][lifecycle]") {
  auto failing = std::make_shared<TestFrameSink>();
  auto healthy = std::make_shared<TestFrameSink>();
  auto later = std::make_shared<TestFrameSink>();
  failing->initialize_error_ = true;
  healthy->clock_ = true;
  Output output({healthy, failing, later});
  CHECK_FALSE(output.Start(Format()));
  CHECK(output.status().state == OutputState::kStopped);
  CHECK_FALSE(output.HasSynchronizationClock());
  CHECK(output.status().frame_sinks[1].state == FrameSinkState::kFailed);
  CHECK(output.status().frame_sinks[1].error == nullptr);
  CHECK(healthy->initializes_ == 1);
  CHECK(failing->initializes_ == 1);
  CHECK(later->initializes_ == 1);
  CHECK_THROWS_AS(output.Send({}), std::logic_error);
  CHECK(healthy->sends_ == 0);
  CHECK(later->sends_ == 0);

  CHECK_FALSE(output.Start(Format()));
  CHECK(healthy->initializes_ == 1);
  CHECK(failing->initializes_ == 2);
  CHECK(later->initializes_ == 1);
  failing->initialize_error_ = false;
  REQUIRE(output.Start(Format()));
  CHECK(output.status().state == OutputState::kRunning);
  CHECK(output.status().frame_sinks[1].state == FrameSinkState::kRunning);
  CHECK(output.HasSynchronizationClock());
  CHECK(healthy->initializes_ == 1);
  CHECK(failing->initializes_ == 3);
  CHECK(later->initializes_ == 1);
  CHECK(output.Send({}));
  CHECK(healthy->sends_ == 1);
  CHECK(failing->sends_ == 1);
  CHECK(later->sends_ == 1);
}

TEST_CASE("Output retains initialization failures until destruction",
          "[output][failure][ownership]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  frame_sink->initialize_error_ = true;
  std::weak_ptr<FrameSink> weak = frame_sink;
  bool destroyed = false;
  frame_sink->on_destroy_ = [&] { destroyed = true; };
  {
    Output output({frame_sink});
    CHECK_FALSE(output.Start(Format()));
    frame_sink.reset();
    CHECK_FALSE(weak.expired());
    CHECK_FALSE(destroyed);
  }
  CHECK(weak.expired());
  CHECK(destroyed);
}

TEST_CASE("Output clock follows active frame sinks", "[output][clock]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  frame_sink->clock_ = true;
  frame_sink->on_send_ = [] { return Ready(false); };
  Output output({frame_sink});
  REQUIRE(output.Start(Format()));
  CHECK(output.HasSynchronizationClock());
  output.Send({});
  CHECK_FALSE(output.HasSynchronizationClock());
}

TEST_CASE("Output cancellation preserves frame sinks and bounded pending work",
          "[output][cancel]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  std::promise<bool> accepted;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  frame_sink->on_send_ = [&] {
    entered.set_value();
    return accepted.get_future();
  };
  Output output({frame_sink});
  REQUIRE(output.Start(Format()));
  std::atomic<bool> cancelled{false};
  auto operation = std::async(std::launch::async,
                              [&] { return output.Send({}, &cancelled); });
  const bool started = entered_future.wait_for(2s) == std::future_status::ready;
  cancelled = true;
  REQUIRE(operation.wait_for(2s) == std::future_status::ready);
  REQUIRE(started);
  CHECK_FALSE(operation.get());
  CHECK(frame_sink->sends_ == 1);
  CHECK_FALSE(output.Send({}, &cancelled));
  CHECK(frame_sink->sends_ == 1);
  CHECK(output.status().frame_sinks[0].state == FrameSinkState::kRunning);
  accepted.set_value(true);
  frame_sink->on_send_ = [] { return Ready(true); };
  cancelled = false;
  CHECK(output.Send({}, &cancelled));
  CHECK(frame_sink->sends_ == 2);
  REQUIRE(output.Start(Format()));
  CHECK(frame_sink->initializes_ == 1);
}

TEST_CASE("Output owns frame sinks until release and preserves external owners",
          "[output][ownership]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  std::weak_ptr<FrameSink> weak = frame_sink;
  bool destroyed = false;
  frame_sink->on_destroy_ = [&] { destroyed = true; };
  {
    Output output({frame_sink});
    REQUIRE(output.Start(Format()));
    frame_sink.reset();
    CHECK_FALSE(weak.expired());
    CHECK(output.Send({}));
  }
  CHECK(weak.expired());
  CHECK(destroyed);

  auto external = std::make_shared<TestFrameSink>();
  {
    Output output({external});
    REQUIRE(output.Start(Format()));
  }
  CHECK(external.use_count() == 1);
  CHECK(external->initializes_ == 1);
}

TEST_CASE("Output releases failed frame sink ownership", "[output][failure]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  frame_sink->on_send_ = [] { return Ready(false); };
  std::weak_ptr<FrameSink> weak = frame_sink;
  Output output({frame_sink});
  REQUIRE(output.Start(Format()));
  frame_sink.reset();
  CHECK(output.Send({}));
  CHECK(weak.expired());
  CHECK(output.status().frame_sinks[0].state == FrameSinkState::kFailed);
  CHECK_FALSE(output.Start(Format()));
  CHECK(output.status().state == OutputState::kStopped);
  CHECK_THROWS_AS(output.Send({}), std::logic_error);
}

TEST_CASE("Output destruction does not wait for outstanding admission",
          "[output][cancel][ownership]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  std::promise<bool> accepted;
  frame_sink->on_send_ = [&] { return accepted.get_future(); };
  {
    Output output({frame_sink});
    REQUIRE(output.Start(Format()));
    std::atomic<bool> cancelled{false};
    frame_sink->on_send_ = [&] {
      cancelled = true;
      return accepted.get_future();
    };
    CHECK_FALSE(output.Send({}, &cancelled));
  }
  CHECK(frame_sink.use_count() == 1);
  accepted.set_value(true);
}

TEST_CASE("Output rejects invalid collections and channel formats",
          "[output][validation]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  CHECK_THROWS_AS(Output({nullptr}), std::invalid_argument);
  CHECK_THROWS_AS(Output({frame_sink, frame_sink}), std::invalid_argument);
  Output output;
  CHECK_THROWS_AS(output.Send({}), std::logic_error);
  CHECK_FALSE(output.Start({}));
  auto format = Format();
  SECTION("width") { format.width = 0; }
  SECTION("height") { format.height = 0; }
  SECTION("pixel format") { format.video_pixel_format = AV_PIX_FMT_NONE; }
  SECTION("hardware format") { format.video_pixel_format = AV_PIX_FMT_CUDA; }
  SECTION("fps") { format.fps.den = 0; }
  SECTION("sample rate") { format.audio_sample_rate = 0; }
  SECTION("sample format") { format.audio_sample_format = AV_SAMPLE_FMT_NONE; }
  SECTION("audio layout") { format.audio_channel_layout = {}; }
  CHECK_FALSE(output.Start(format));
}

TEST_CASE("Output reuses an equivalent format without initializing again",
          "[output][lifecycle]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  Output output({frame_sink});
  auto format = Format();
  REQUIRE(output.Start(format));
  format.fps = {60, 2};
  format.video_encoder_name = "another encoder";
  REQUIRE(output.Start(format));
  CHECK(frame_sink->initializes_ == 1);
  CHECK(output.Send({}));
  format.width = 1280;
  CHECK_FALSE(output.Start(format));
  CHECK(output.Send({}));
}

TEST_CASE("Output rejects invalid and deferred admission futures",
          "[output][failure]") {
  auto frame_sink = std::make_shared<TestFrameSink>();
  SECTION("invalid") {
    frame_sink->on_send_ = [] { return std::future<bool>{}; };
  }
  SECTION("deferred") {
    frame_sink->on_send_ = [] {
      return std::async(std::launch::deferred, [] { return true; });
    };
  }
  Output output({frame_sink});
  REQUIRE(output.Start(Format()));
  CHECK(output.Send({}));
  CHECK(output.status().frame_sinks[0].state == FrameSinkState::kFailed);
}

TEST_CASE("Output with no frame sinks remains running", "[output][lifecycle]") {
  Output output;
  REQUIRE(output.Start(Format()));
  CHECK_FALSE(output.HasSynchronizationClock());
  CHECK(output.Send({}));
  CHECK(output.status().state == OutputState::kRunning);
}

}  // namespace
