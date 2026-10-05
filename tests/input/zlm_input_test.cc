#include "mw/streamer/input/zlm_input.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Network/Session.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::InputState;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string SamplePath(std::string_view name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/" + std::string(name);
}

struct ServerState {
  std::mutex mutex;
  std::condition_variable condition;
  std::string flv;
  bool fail = false;
  size_t requests = 0;
  std::vector<std::weak_ptr<toolkit::Session>> sessions;
};

// A streaming HTTP response remains open until the test disconnects it.
class FlvSession final : public toolkit::Session {
 public:
  explicit FlvSession(const toolkit::Socket::Ptr& socket) : Session(socket) {}

  void Configure(std::shared_ptr<ServerState> state) {
    state_ = std::move(state);
  }

  void onRecv(const toolkit::Buffer::Ptr& buffer) override {
    if (responded_) {
      return;
    }
    request_.append(buffer->data(), buffer->size());
    if (request_.find("\r\n\r\n") == std::string::npos) {
      return;
    }
    responded_ = true;
    bool fail;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      ++state_->requests;
      fail = state_->fail;
    }
    state_->condition.notify_all();
    if (fail) {
      send(
          "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n");
      return;
    }
    send(std::string("HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\n"
                     "Connection: close\r\n\r\n") +
         state_->flv);
  }

  void onError(const toolkit::SockException&) override {}
  void onManager() override {}

 private:
  std::shared_ptr<ServerState> state_;
  std::string request_;
  bool responded_ = false;
};

class FlvServer final {
 public:
  explicit FlvServer(bool fail = false)
      : state_(std::make_shared<ServerState>()) {
    poller_ = toolkit::EventPollerPool::Instance().getPoller();
    std::ifstream sample(SamplePath("h264_aac.flv"), std::ios::binary);
    if (!sample) {
      throw std::runtime_error("Cannot open FLV test fixture");
    }
    state_->flv.assign(std::istreambuf_iterator<char>(sample), {});
    state_->fail = fail;
    server_ = std::make_shared<toolkit::TcpServer>(poller_);
    server_->start<FlvSession>(
        0, "127.0.0.1", 1024,
        [state = state_](std::shared_ptr<FlvSession>& session) {
          session->Configure(state);
          std::lock_guard<std::mutex> lock(state->mutex);
          state->sessions.push_back(session);
        });
    url_ =
        "http://127.0.0.1:" + std::to_string(server_->getPort()) + "/live.flv";
  }

  ~FlvServer() {
    Disconnect();
    poller_->sync([this] { server_.reset(); });
  }

  const std::string& url() const { return url_; }

  void Disconnect() {
    std::vector<std::shared_ptr<toolkit::Session>> sessions;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      for (const auto& weak_session : state_->sessions) {
        if (auto session = weak_session.lock()) {
          sessions.push_back(std::move(session));
        }
      }
    }
    for (const auto& session : sessions) {
      session->getPoller()->sync([session] { session->shutdown(); });
    }
  }

  size_t requests() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->requests;
  }

  bool HasNoNewRequests(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    const auto count = state_->requests;
    return !state_->condition.wait_for(
        lock, duration, [&] { return state_->requests != count; });
  }

 private:
  std::shared_ptr<ServerState> state_;
  toolkit::EventPoller::Ptr poller_;
  toolkit::TcpServer::Ptr server_;
  std::string url_;
};

struct Observation {
  std::vector<InputState> states;
  std::vector<ffmpeg::StreamInfo> streams;
  size_t ready = 0;
  size_t packets = 0;
  size_t callbacks = 0;
  size_t video_packets = 0;
  size_t audio_packets = 0;
  bool valid = true;
  int error_code = 0;
  std::string error_message;
};

class Observer final {
 public:
  void Attach(ZlmInput& input) {
    input.SetOnReady([this](const std::vector<ffmpeg::StreamInfo>& streams) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++observation_.callbacks;
        ++observation_.ready;
        observation_.streams = streams;
        for (const auto& stream : streams) {
          observation_.valid &= stream.codec_parameters.get() != nullptr &&
                                stream.stream_index >= 0 &&
                                stream.time_base.num == 1 &&
                                stream.time_base.den == 1000;
        }
      }
      condition_.notify_all();
    });
    input.SetOnPacket([this](const ffmpeg::Packet& packet) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++observation_.callbacks;
        ++observation_.packets;
        const auto* raw = packet.get();
        if (!raw || !raw->buf || !raw->data || raw->size <= 0 ||
            raw->pts == AV_NOPTS_VALUE || raw->dts == AV_NOPTS_VALUE ||
            raw->time_base.num != 1 || raw->time_base.den != 1000 ||
            observation_.ready == 0) {
          observation_.valid = false;
        } else {
          const auto stream =
              std::find_if(observation_.streams.begin(),
                           observation_.streams.end(), [raw](const auto& info) {
                             return info.stream_index == raw->stream_index;
                           });
          if (stream == observation_.streams.end()) {
            observation_.valid = false;
          } else if (stream->codec_parameters.get()->codec_type ==
                     AVMEDIA_TYPE_VIDEO) {
            ++observation_.video_packets;
          } else if (stream->codec_parameters.get()->codec_type ==
                     AVMEDIA_TYPE_AUDIO) {
            ++observation_.audio_packets;
          }
          if (!retained_packet_) {
            retained_packet_ = std::make_unique<ffmpeg::Packet>(packet.Ref());
            retained_bytes_.assign(raw->data, raw->data + raw->size);
          }
        }
      }
      condition_.notify_all();
    });
    input.SetOnStateChanged(
        [this](InputState state, int code, std::string_view message) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            ++observation_.callbacks;
            observation_.states.push_back(state);
            observation_.error_code = code;
            observation_.error_message = std::string(message);
          }
          condition_.notify_all();
        });
  }

  template <typename Predicate>
  bool Wait(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [&] { return predicate(observation_); });
  }

  bool WaitForState(InputState state) {
    return Wait([state](const auto& observed) {
      return std::find(observed.states.begin(), observed.states.end(), state) !=
             observed.states.end();
    });
  }

  Observation snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return observation_;
  }

  bool HasNoNewCallbacks(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto count = observation_.callbacks;
    return !condition_.wait_for(
        lock, duration, [&] { return observation_.callbacks != count; });
  }

  bool RetainedPacketIsValid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!retained_packet_) {
      return false;
    }
    const auto* packet = retained_packet_->get();
    return packet && packet->buf && packet->size == retained_bytes_.size() &&
           std::equal(retained_bytes_.begin(), retained_bytes_.end(),
                      packet->data);
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  Observation observation_;
  std::unique_ptr<ffmpeg::Packet> retained_packet_;
  std::vector<uint8_t> retained_bytes_;
};

size_t StateCount(const Observation& observation, InputState state) {
  return std::count(observation.states.begin(), observation.states.end(),
                    state);
}

ZlmInputConfig RetryConfig(int max_retries = -1,
                           std::chrono::milliseconds interval = 30ms) {
  ZlmInputConfig config;
  config.retry_interval = interval;
  config.max_retries = max_retries;
  return config;
}

}  // namespace

TEST_CASE("ZLM input delivers file streams and owned packets before EOF") {
  Observer observer;
  ZlmInput input;
  observer.Attach(input);
  CHECK(input.state() == InputState::kIdle);
  input.Start(SamplePath());
  REQUIRE(observer.WaitForState(InputState::kEnded));
  const auto observed = observer.snapshot();
  CHECK(observed.valid);
  CHECK(observed.ready == 1);
  REQUIRE(observed.streams.size() == 2);
  CHECK(observed.video_packets == 20);
  CHECK(observed.audio_packets == 95);
  CHECK(StateCount(observed, InputState::kConnecting) == 1);
  CHECK(StateCount(observed, InputState::kConnected) == 1);
  CHECK(StateCount(observed, InputState::kWaitingRetry) == 0);
  CHECK(input.state() == InputState::kEnded);
  input.Stop();
  CHECK(observer.RetainedPacketIsValid());
  CHECK(input.state() == InputState::kStopped);
  CHECK(observer.HasNoNewCallbacks(100ms));
}

TEST_CASE("ZLM input rejects duplicate starts and restarts after stop") {
  FlvServer server;
  Observer observer;
  ZlmInput input;
  observer.Attach(input);
  input.Start(server.url());
  CHECK_THROWS_AS(input.Start(server.url()), std::logic_error);
  REQUIRE(observer.Wait([](const auto& observed) {
    return observed.ready == 1 && observed.packets > 0;
  }));
  input.Stop();
  CHECK(input.state() == InputState::kStopped);
  const auto stopped = observer.snapshot();
  REQUIRE_FALSE(stopped.states.empty());
  CHECK(stopped.states.back() == InputState::kStopped);
  CHECK(StateCount(stopped, InputState::kStopped) == 1);
  CHECK(stopped.error_code == 0);
  CHECK(stopped.error_message.empty());
  CHECK(observer.HasNoNewCallbacks(100ms));
  input.Stop();
  CHECK(observer.snapshot().callbacks == stopped.callbacks);
  input.Start(server.url());
  REQUIRE(observer.Wait([](const auto& observed) {
    return observed.ready == 2 &&
           StateCount(observed, InputState::kConnected) == 2;
  }));
  CHECK(input.state() == InputState::kConnected);
  CHECK(observer.snapshot().valid);
  input.Stop();
  CHECK(StateCount(observer.snapshot(), InputState::kStopped) == 2);
  CHECK(observer.HasNoNewCallbacks(100ms));
}

TEST_CASE("ZLM input does not retry when automatic reconnect is disabled") {
  FlvServer server(true);
  Observer observer;
  auto config = RetryConfig();
  config.auto_reconnect = false;
  ZlmInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitForState(InputState::kFailed));
  const auto observed = observer.snapshot();
  CHECK(StateCount(observed, InputState::kConnecting) == 1);
  CHECK(StateCount(observed, InputState::kWaitingRetry) == 0);
  CHECK(observed.error_code != 0);
  CHECK_FALSE(observed.error_message.empty());
  CHECK(server.requests() == 1);
  CHECK(server.HasNoNewRequests(150ms));
  input.Stop();
}

TEST_CASE("ZLM input exhausts exactly the configured retry budget") {
  FlvServer server(true);
  Observer observer;
  ZlmInput input(RetryConfig(2));
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitForState(InputState::kFailed));
  const auto observed = observer.snapshot();
  CHECK(StateCount(observed, InputState::kConnecting) == 3);
  CHECK(StateCount(observed, InputState::kWaitingRetry) == 2);
  CHECK(StateCount(observed, InputState::kFailed) == 1);
  CHECK(server.requests() == 3);
  CHECK(server.HasNoNewRequests(150ms));
  input.Stop();
}

TEST_CASE("ZLM input zero retry budget prevents automatic retries") {
  FlvServer server(true);
  Observer observer;
  ZlmInput input(RetryConfig(0));
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitForState(InputState::kFailed));
  CHECK(StateCount(observer.snapshot(), InputState::kWaitingRetry) == 0);
  CHECK(server.requests() == 1);
  input.Stop();
}

TEST_CASE("ZLM input reconnects after two disconnects and resets its budget") {
  FlvServer server;
  Observer observer;
  ZlmInput input(RetryConfig(1));
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.Wait([](const auto& observed) {
    return observed.ready == 1 && observed.packets > 0;
  }));
  for (size_t attempt = 1; attempt <= 2; ++attempt) {
    const auto packets = observer.snapshot().packets;
    server.Disconnect();
    REQUIRE(observer.Wait([attempt, packets](const auto& observed) {
      return observed.ready == attempt + 1 && observed.packets > packets &&
             StateCount(observed, InputState::kConnected) == attempt + 1;
    }));
    CHECK(input.state() == InputState::kConnected);
  }
  const auto observed = observer.snapshot();
  CHECK(observed.valid);
  CHECK(StateCount(observed, InputState::kWaitingRetry) == 2);
  CHECK(StateCount(observed, InputState::kConnecting) == 3);
  CHECK(StateCount(observed, InputState::kFailed) == 0);
  CHECK(server.requests() == 3);
  input.Stop();
  CHECK(observer.RetainedPacketIsValid());
  CHECK(observer.HasNoNewCallbacks(100ms));
}

TEST_CASE("ZLM input stop cancels retry and silences callbacks") {
  FlvServer server(true);
  Observer observer;
  ZlmInput input(RetryConfig(-1, 200ms));
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitForState(InputState::kWaitingRetry));
  input.Stop();
  CHECK(input.state() == InputState::kStopped);
  CHECK(observer.HasNoNewCallbacks(350ms));
  CHECK(server.requests() == 1);
  CHECK(server.HasNoNewRequests(50ms));
}

TEST_CASE("ZLM input destruction cancels pending retry without callbacks") {
  FlvServer server(true);
  Observer observer;
  auto input = std::make_unique<ZlmInput>(RetryConfig(-1, 200ms));
  observer.Attach(*input);
  input->Start(server.url());
  REQUIRE(observer.WaitForState(InputState::kWaitingRetry));
  const auto callbacks = observer.snapshot().callbacks;
  input.reset();
  CHECK(observer.snapshot().callbacks == callbacks);
  CHECK(observer.HasNoNewCallbacks(350ms));
  CHECK(server.requests() == 1);
}

TEST_CASE("ZLM input missing local file fails without retry") {
  Observer observer;
  ZlmInput input(RetryConfig());
  observer.Attach(input);
  input.Start(SamplePath("missing-input-file.mp4"));
  REQUIRE(observer.WaitForState(InputState::kFailed));
  const auto observed = observer.snapshot();
  CHECK(observed.ready == 0);
  CHECK(observed.packets == 0);
  CHECK(StateCount(observed, InputState::kConnecting) == 1);
  CHECK(StateCount(observed, InputState::kWaitingRetry) == 0);
  input.Stop();
}

TEST_CASE("ZLM input rejects invalid config and empty URLs synchronously") {
  auto config = RetryConfig();
  config.max_retries = -2;
  CHECK_THROWS_AS(ZlmInput(config), std::invalid_argument);
  config = RetryConfig(-1, 0ms);
  CHECK_THROWS_AS(ZlmInput(config), std::invalid_argument);
  config = RetryConfig(-1, -1ms);
  CHECK_THROWS_AS(ZlmInput(config), std::invalid_argument);

  Observer observer;
  ZlmInput input;
  observer.Attach(input);
  CHECK_THROWS_AS(input.Start(""), std::invalid_argument);
  CHECK_THROWS_AS(input.Start(" \t\r\n"), std::invalid_argument);
  CHECK(input.state() == InputState::kIdle);
  CHECK(observer.snapshot().callbacks == 0);
}

TEST_CASE("ZLM input stop waits for an in-flight file callback") {
  Observer observer;
  ZlmInput input;
  observer.Attach(input);

  struct Gate {
    explicit Gate(ZlmInput& owner) : input(owner) {}

    ~Gate() {
      ReleaseAndJoin();
      try {
        input.Stop();
      } catch (...) {
        // Cleanup must release the callback even when an assertion throws.
      }
    }

    void ReleaseAndJoin() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
      }
      condition.notify_all();
      if (stopper.joinable()) {
        stopper.join();
      }
    }

    ZlmInput& input;
    std::mutex mutex;
    std::condition_variable condition;
    std::thread stopper;
    std::exception_ptr stop_error;
    bool entered = false;
    bool released = false;
    bool stop_started = false;
    bool stop_returned = false;
    size_t packets = 0;
  } gate(input);

  input.SetOnPacket([&gate](const ffmpeg::Packet&) {
    std::unique_lock<std::mutex> lock(gate.mutex);
    ++gate.packets;
    gate.entered = true;
    gate.condition.notify_all();
    gate.condition.wait(lock, [&gate] { return gate.released; });
  });
  input.Start(SamplePath());
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    REQUIRE(
        gate.condition.wait_for(lock, 8s, [&gate] { return gate.entered; }));
  }
  gate.stopper = std::thread([&gate] {
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.stop_started = true;
    }
    gate.condition.notify_all();
    try {
      gate.input.Stop();
    } catch (...) {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.stop_error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> lock(gate.mutex);
      gate.stop_returned = true;
    }
    gate.condition.notify_all();
  });

  bool returned_while_blocked;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    REQUIRE(gate.condition.wait_for(lock, 8s,
                                    [&gate] { return gate.stop_started; }));
    returned_while_blocked = gate.condition.wait_for(
        lock, 100ms, [&gate] { return gate.stop_returned; });
  }
  gate.ReleaseAndJoin();
  CHECK_FALSE(returned_while_blocked);
  CHECK(gate.stop_returned);
  CHECK_FALSE(gate.stop_error);
  CHECK(input.state() == InputState::kStopped);
  CHECK(observer.HasNoNewCallbacks(100ms));
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    const auto packets = gate.packets;
    CHECK_FALSE(gate.condition.wait_for(
        lock, 100ms, [&gate, packets] { return gate.packets != packets; }));
  }
}
