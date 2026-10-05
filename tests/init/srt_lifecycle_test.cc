#include <srt/srt.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mw/streamer/init/init.h"
#include "mw/streamer/input/zlm_input.h"

namespace {

using namespace std::chrono_literals;

class SrtLibrary final {
 public:
  SrtLibrary() {
    if (srt_startup() != 0) {
      throw std::runtime_error("Cannot initialize SRT sender library");
    }
  }
  ~SrtLibrary() { srt_cleanup(); }
};

// The sender uses native libsrt, never the streamer's private reactor or pools.
class SrtSender final {
 public:
  SrtSender() {
    std::ifstream sample(
        std::string(MW_STREAMER_INIT_TEST_DATA_DIR) + "/h264_aac.ts",
        std::ios::binary);
    if (!sample) {
      throw std::runtime_error("Cannot open TS test fixture");
    }
    data_.assign(std::istreambuf_iterator<char>(sample), {});
    listener_ = srt_create_socket();
    if (listener_ == SRT_INVALID_SOCK) {
      throw std::runtime_error(srt_getlasterror_str());
    }
    try {
      const auto live = SRTT_LIVE;
      const bool sender = true;
      const bool asynchronous = false;
      const int payload_size = 1316;
      SetOption(SRTO_TRANSTYPE, live);
      SetOption(SRTO_SENDER, sender);
      SetOption(SRTO_RCVSYN, asynchronous);
      SetOption(SRTO_SNDSYN, asynchronous);
      SetOption(SRTO_PAYLOADSIZE, payload_size);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (srt_bind(listener_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == SRT_ERROR ||
          srt_listen(listener_, 1) == SRT_ERROR) {
        throw std::runtime_error(srt_getlasterror_str());
      }
      int size = sizeof(address);
      if (srt_getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                          &size) == SRT_ERROR) {
        throw std::runtime_error(srt_getlasterror_str());
      }
      url_ = "srt://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) +
             "?streamid=#!::r=live/test";
      thread_ = std::thread([this] { Send(); });
    } catch (...) {
      srt_close(listener_);
      throw;
    }
  }

  ~SrtSender() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
    thread_.join();
    srt_close(listener_);
  }

  const std::string& url() const { return url_; }

  bool connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
  }

  std::string error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  template <typename Value>
  void SetOption(SRT_SOCKOPT option, const Value& value) {
    if (srt_setsockflag(listener_, option, &value, sizeof(value)) ==
        SRT_ERROR) {
      throw std::runtime_error(srt_getlasterror_str());
    }
  }

  bool WaitForStop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return stopped_; });
  }

  void RecordError() {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = srt_getlasterror_str();
  }

  void Send() {
    SRTSOCKET peer = SRT_INVALID_SOCK;
    while (!WaitForStop(10ms)) {
      peer = srt_accept(listener_, nullptr, nullptr);
      if (peer != SRT_INVALID_SOCK) {
        break;
      }
      if (srt_getlasterror(nullptr) != SRT_EASYNCRCV) {
        RecordError();
        return;
      }
    }
    if (peer == SRT_INVALID_SOCK) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connected_ = true;
    }
    for (size_t offset = 0; offset < data_.size();) {
      if (WaitForStop(1ms)) {
        break;
      }
      const auto size =
          static_cast<int>(std::min<size_t>(1316, data_.size() - offset));
      SRT_MSGCTRL control{};
      srt_msgctrl_init(&control);
      const auto sent =
          srt_sendmsg2(peer, data_.data() + offset, size, &control);
      if (sent == SRT_ERROR) {
        if (srt_getlasterror(nullptr) == SRT_EASYNCSND) {
          continue;
        }
        RecordError();
        break;
      }
      offset += static_cast<size_t>(sent);
    }
    // Keep the transport alive while the receiver finishes track discovery.
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return stopped_; });
    lock.unlock();
    srt_close(peer);
  }

  SRTSOCKET listener_ = SRT_INVALID_SOCK;
  std::string url_;
  std::vector<char> data_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_ = false;
  bool connected_ = false;
  std::string error_;
  std::thread thread_;
};

}  // namespace

TEST_CASE("streamer recreates its SRT reactor and receives real media twice") {
  for (int iteration = 0; iteration < 2; ++iteration) {
    CAPTURE(iteration);
    // The sender's libsrt startup reference outlives the streamer's shutdown.
    std::unique_ptr<SrtLibrary> sender_library;
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    std::unique_ptr<mw::streamer::MwStreamerContext,
                    decltype(&mw::streamer::Shutdown)>
        context(mw::streamer::Init(config), &mw::streamer::Shutdown);
    sender_library = std::make_unique<SrtLibrary>();
    {
      SrtSender sender;
      std::mutex mutex;
      std::condition_variable changed;
      size_t ready = 0;
      bool valid = true;
      bool audio = false;
      bool video = false;
      bool failed = false;
      std::string error;
      std::vector<mw::streamer::ffmpeg::StreamInfo> streams;
      mw::streamer::ZlmInputConfig input_config;
      input_config.auto_reconnect = false;
      mw::streamer::ZlmInput input(input_config);
      input.SetOnReady([&](const auto& information) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++ready;
          streams = information;
        }
        changed.notify_all();
      });
      input.SetOnPacket([&](const mw::streamer::ffmpeg::Packet& packet) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          valid &=
              ready == 1 && packet->buf && packet->data && packet->size > 0;
          const auto stream = std::find_if(
              streams.begin(), streams.end(), [&](const auto& information) {
                return information.stream_index == packet->stream_index;
              });
          if (stream == streams.end()) {
            valid = false;
          } else if (stream->codec_parameters.get()->codec_type ==
                     AVMEDIA_TYPE_AUDIO) {
            audio = true;
          } else if (stream->codec_parameters.get()->codec_type ==
                     AVMEDIA_TYPE_VIDEO) {
            video = true;
          }
        }
        changed.notify_all();
      });
      input.SetOnStateChanged(
          [&](mw::streamer::InputState state, int, std::string_view message) {
            if (state == mw::streamer::InputState::kFailed) {
              {
                std::lock_guard<std::mutex> lock(mutex);
                failed = true;
                error = std::string(message);
              }
              changed.notify_all();
            }
          });
      input.Start(sender.url());
      bool completed;
      {
        std::unique_lock<std::mutex> lock(mutex);
        completed = changed.wait_for(
            lock, 8s, [&] { return (ready == 1 && audio && video) || failed; });
      }
      input.Stop();
      INFO(error);
      INFO(sender.error());
      REQUIRE(completed);
      REQUIRE_FALSE(failed);
      REQUIRE(sender.connected());
      CHECK(ready == 1);
      CHECK(streams.size() == 2);
      CHECK(valid);
      CHECK(audio);
      CHECK(video);
    }
    mw::streamer::Shutdown(context.get());
    context.release();
    sender_library.reset();
  }
}
