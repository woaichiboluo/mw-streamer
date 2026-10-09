#include <srt/srt.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../encoder/encoder_test_support.h"
#include "Common/MediaSource.h"
#include "Common/config.h"
#include "Network/Session.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Rtmp/RtmpProtocol.h"
#include "Rtmp/amf.h"
#include "Rtsp/RtspMediaSource.h"
#include "Rtsp/RtspSession.h"
#include "Util/NoticeCenter.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/remuxer/async_remuxer.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::AsyncRemuxer;

class Runtime final {
 public:
  explicit Runtime(bool debug = false) {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    if (debug) {
      config.log.modules = "zlm:trace;streamer:debug";
      config.log.modules_size = std::strlen(config.log.modules);
      config.log.rotating_file_enabled = 1;
      config.log.rotating_file_path = "async-remuxer-network-debug.log";
      config.log.rotating_file_path_size =
          std::strlen(config.log.rotating_file_path);
    }
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_;
};

std::string Name() {
  static std::atomic<unsigned> next{0};
  return "async_remux_network_" + std::to_string(++next);
}

struct Encoded {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
  std::vector<std::int64_t> clocks;
};

Encoded Encode() {
  encoder_test::Capture capture;
  mw::streamer::Encoder encoder;
  capture.Bind(encoder);
  auto config = encoder_test::Config();
  config.gop_size = 10;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(
      config, {encoder_test::VideoStream(), encoder_test::AudioStream()}, cpu);
  REQUIRE(encoder.SubmitAudio(encoder_test::AudioFrame(48000)));
  for (int index = 0; index < 50; ++index)
    REQUIRE(encoder.SubmitVideo(encoder_test::VideoFrame(index)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.callback_error.empty());
  std::vector<std::size_t> order;
  for (std::size_t index = 0; index < capture.packets.size(); ++index)
    order.push_back(index);
  std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
    return capture.dts_ns[left] < capture.dts_ns[right];
  });
  Encoded output;
  output.streams = std::move(capture.streams);
  for (const auto index : order) {
    output.packets.push_back(std::move(capture.packets[index]));
    output.clocks.push_back(capture.dts_ns[index]);
  }
  return output;
}

// Replay actual encoded GOPs while increasing both native media ticks and the
// synchronized ordering clock. Condition-variable pacing is cancellable.
class Replay final {
 public:
  Replay(AsyncRemuxer& remuxer, const Encoded& encoded)
      : remuxer_(remuxer), encoded_(encoded), thread_([this] { Run(); }) {}
  ~Replay() { Stop(); }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  bool valid() const noexcept { return valid_.load(); }

 private:
  void Run() noexcept {
    try {
      const auto start = std::chrono::steady_clock::now();
      for (std::int64_t cycle = 0;; ++cycle) {
        const auto shift = cycle * 1000000000LL;
        for (std::size_t index = 0; index < encoded_.packets.size(); ++index) {
          if (cycle > 0 && encoded_.packets[index]->stream_index == 7 &&
              encoded_.packets[index]->dts < 0)
            continue;
          const auto delay =
              encoded_.clocks[index] - encoded_.clocks.front() + shift;
          {
            std::unique_lock<std::mutex> lock(mutex_);
            if (changed_.wait_until(lock,
                                    start + std::chrono::nanoseconds(delay),
                                    [&] { return stopped_; }))
              return;
          }
          auto packet = encoded_.packets[index].Ref();
          const auto ticks = av_rescale_q(shift, encoder_test::kNanoseconds,
                                          packet->time_base);
          packet->pts += ticks;
          packet->dts += ticks;
          if (!remuxer_.SubmitPacket(packet, encoded_.clocks[index] + shift)) {
            valid_ = false;
            return;
          }
        }
      }
    } catch (...) {
      valid_ = false;
    }
  }

  AsyncRemuxer& remuxer_;
  const Encoded& encoded_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_ = false;
  std::atomic<bool> valid_{true};
  std::thread thread_;
};

struct RtmpState {
  struct Stream {
    std::string name;
    unsigned connections = 0;
    std::vector<std::uint32_t> stamps;
    std::vector<std::string> video;
  };
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Stream> streams;
  std::string error;
};

// ZLM's SDK checkout has no RtmpSession. Use its real wire protocol for the
// receiver handshake, then inspect the received FLV video messages.
class RtmpSession final : public toolkit::Session,
                          public mediakit::RtmpProtocol {
 public:
  explicit RtmpSession(const toolkit::Socket::Ptr& socket) : Session(socket) {}
  void Configure(const std::shared_ptr<RtmpState>& state) { state_ = state; }
  const std::string& name() const { return name_; }
  void onRecv(const toolkit::Buffer::Ptr& buffer) override {
    try {
      onParseRtmp(buffer->data(), buffer->size());
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->error = error.what();
      state_->changed.notify_all();
    }
  }
  void onError(const toolkit::SockException&) override {}
  void onManager() override {}

 protected:
  void onSendRawData(toolkit::Buffer::Ptr buffer) override {
    send(std::move(buffer));
  }
  void onRtmpChunk(mediakit::RtmpPacket::Ptr packet) override {
    if (packet->type_id == MSG_CMD) {
      AMFDecoder decoder(packet->buffer, 0);
      const auto command = decoder.load<std::string>();
      const auto transaction = decoder.load<double>();
      if (command == "connect") {
        sendChunkSize(4096);
        AMFValue properties(AMF_OBJECT);
        properties.set("fmsVer", "FMS/3,5,7,7009");
        properties.set("capabilities", 31);
        AMFValue info(AMF_OBJECT);
        info.set("level", "status");
        info.set("code", "NetConnection.Connect.Success");
        AMFEncoder reply;
        reply << "_result" << transaction << properties << info;
        sendRtmp(MSG_CMD, STREAM_CONTROL, reply.data(), 0, CHUNK_SYSTEM);
      } else if (command == "createStream") {
        AMFEncoder reply;
        reply << "_result" << transaction << nullptr << STREAM_MEDIA;
        sendRtmp(MSG_CMD, STREAM_CONTROL, reply.data(), 0, CHUNK_SYSTEM);
      } else if (command == "publish") {
        decoder.load<AMFValue>();
        name_ = decoder.load<std::string>();
        {
          std::lock_guard<std::mutex> lock(state_->mutex);
          auto stream = std::find_if(
              state_->streams.begin(), state_->streams.end(),
              [&](const auto& value) { return value.name == name_; });
          if (stream == state_->streams.end()) {
            state_->streams.push_back({name_, 0, {}, {}});
            stream = state_->streams.end() - 1;
          }
          ++stream->connections;
        }
        state_->changed.notify_all();
        AMFValue info(AMF_OBJECT);
        info.set("level", "status");
        info.set("code", "NetStream.Publish.Start");
        AMFEncoder reply;
        reply << "onStatus" << 0 << nullptr << info;
        sendRtmp(MSG_CMD, STREAM_MEDIA, reply.data(), 0, CHUNK_SYSTEM);
      }
    } else if (packet->type_id == MSG_VIDEO && !packet->isConfigFrame()) {
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto stream = std::find_if(
            state_->streams.begin(), state_->streams.end(),
            [&](const auto& value) { return value.name == name_; });
        if (stream != state_->streams.end()) {
          stream->stamps.push_back(packet->time_stamp);
          stream->video.push_back(packet->toString());
        }
      }
      state_->changed.notify_all();
    }
  }

 private:
  std::shared_ptr<RtmpState> state_;
  std::string name_;
};

class RtmpReceiver final {
 public:
  RtmpReceiver()
      : state_(std::make_shared<RtmpState>()),
        poller_(toolkit::EventPollerPool::Instance().getPoller()),
        server_(std::make_shared<toolkit::TcpServer>(poller_)) {
    server_->start<RtmpSession>(0, "127.0.0.1", 1024,
                                [this](std::shared_ptr<RtmpSession>& session) {
                                  session->Configure(state_);
                                  sessions_.push_back(session);
                                });
  }
  ~RtmpReceiver() {
    poller_->sync([&] {
      for (const auto& weak : sessions_)
        if (auto session = weak.lock()) session->shutdown();
      sessions_.clear();
      server_.reset();
    });
  }
  std::string Url(const std::string& stream) const {
    return "rtmp://127.0.0.1:" + std::to_string(server_->getPort()) +
           "/network/" + stream;
  }
  bool Wait(const std::string& name, std::size_t packets,
            unsigned connections = 1) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(lock, 8s, [&] {
      const auto stream =
          std::find_if(state_->streams.begin(), state_->streams.end(),
                       [&](const auto& value) { return value.name == name; });
      return stream != state_->streams.end() &&
             stream->connections >= connections &&
             stream->stamps.size() >= packets;
    });
  }
  RtmpState::Stream Get(const std::string& name) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto stream =
        std::find_if(state_->streams.begin(), state_->streams.end(),
                     [&](const auto& value) { return value.name == name; });
    if (stream == state_->streams.end())
      throw std::runtime_error("RTMP stream absent");
    return *stream;
  }
  void Disconnect(const std::string& name) {
    poller_->sync([&] {
      for (const auto& weak : sessions_)
        if (auto session = weak.lock(); session && session->name() == name)
          session->shutdown();
    });
  }
  std::string error() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->error;
  }

 private:
  std::shared_ptr<RtmpState> state_;
  toolkit::EventPoller::Ptr poller_;
  toolkit::TcpServer::Ptr server_;
  std::vector<std::weak_ptr<RtmpSession>> sessions_;
};

class RtspServer final {
 public:
  explicit RtspServer(std::uint16_t port = 0)
      : poller_(toolkit::EventPollerPool::Instance().getPoller()),
        server_(std::make_shared<toolkit::TcpServer>(poller_)) {
    server_->start<mediakit::RtspSession>(port, "127.0.0.1");
  }
  ~RtspServer() {
    poller_->sync([&] { server_.reset(); });
  }
  std::uint16_t port() const { return server_->getPort(); }
  std::string Url(const std::string& stream) const {
    return "rtsp://127.0.0.1:" + std::to_string(port()) + "/network/" + stream;
  }

 private:
  toolkit::EventPoller::Ptr poller_;
  toolkit::TcpServer::Ptr server_;
};

class RegisteredSource final {
 public:
  RegisteredSource(std::string app, std::string stream)
      : app_(std::move(app)), stream_(std::move(stream)) {
    toolkit::NoticeCenter::Instance().addListener(
        this, mediakit::Broadcast::kBroadcastMediaChanged,
        [this](const bool& registered, mediakit::MediaSource& source) {
          if (!registered || source.getSchema() != RTSP_SCHEMA ||
              source.getMediaTuple().app != app_ ||
              source.getMediaTuple().stream != stream_)
            return;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            source_ = std::dynamic_pointer_cast<mediakit::RtspMediaSource>(
                source.shared_from_this());
          }
          changed_.notify_all();
        });
  }
  ~RegisteredSource() {
    toolkit::NoticeCenter::Instance().delListener(this);
    if (poller_)
      poller_->sync([&] {
        reader_.reset();
        source_.reset();
      });
  }
  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 8s, [&] { return bool(source_); });
  }
  void Read() {
    auto source = source_;
    poller_ = source->getOwnerPoller();
    poller_->sync([&] {
      reader_ = source->getRing()->attach(poller_);
      reader_->setReadCB(
          [this](const mediakit::RtspMediaSource::RingDataType& packets) {
            {
              std::lock_guard<std::mutex> lock(mutex_);
              packets->for_each([&](const auto& packet) {
                if (packet->type == mediakit::TrackVideo) ++video_packets_;
              });
            }
            changed_.notify_all();
          });
    });
  }
  bool WaitPackets() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 8s, [&] { return video_packets_ >= 5; });
  }

 private:
  std::string app_, stream_;
  std::mutex mutex_;
  std::condition_variable changed_;
  mediakit::RtspMediaSource::Ptr source_;
  toolkit::EventPoller::Ptr poller_;
  mediakit::RtspMediaSource::RingType::RingReader::Ptr reader_;
  std::size_t video_packets_ = 0;
};

class SrtReceiver final {
 public:
  SrtReceiver() {
    if (srt_startup() != 0) throw std::runtime_error("srt_startup failed");
    listener_ = srt_create_socket();
    try {
      const auto live = SRTT_LIVE;
      const bool asynchronous = false;
      Set(SRTO_TRANSTYPE, live);
      Set(SRTO_RCVSYN, asynchronous);
      Set(SRTO_SNDSYN, asynchronous);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (srt_bind(listener_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == SRT_ERROR ||
          srt_listen(listener_, 1) == SRT_ERROR)
        throw std::runtime_error(srt_getlasterror_str());
      int size = sizeof(address);
      if (srt_getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                          &size) == SRT_ERROR)
        throw std::runtime_error(srt_getlasterror_str());
      port_ = ntohs(address.sin_port);
      thread_ = std::thread([this] { Receive(); });
    } catch (...) {
      if (listener_ != SRT_INVALID_SOCK) srt_close(listener_);
      srt_cleanup();
      throw;
    }
  }
  ~SrtReceiver() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
    thread_.join();
    srt_close(listener_);
    srt_cleanup();
  }
  std::string Url() const {
    return "srt://127.0.0.1:" + std::to_string(port_) +
           "?streamid=#!::r=network/srt,m=publish";
  }
  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 8s,
                             [&] { return messages_ >= 5 || !error_.empty(); });
  }
  bool valid() {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_ >= 5 && valid_ && error_.empty();
  }

 private:
  template <class T>
  void Set(SRT_SOCKOPT option, const T& value) {
    if (srt_setsockflag(listener_, option, &value, sizeof(value)) == SRT_ERROR)
      throw std::runtime_error(srt_getlasterror_str());
  }
  bool Stopped() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 2ms, [&] { return stopped_; });
  }
  void Receive() {
    SRTSOCKET peer = SRT_INVALID_SOCK;
    while (!Stopped()) {
      if (peer == SRT_INVALID_SOCK) {
        peer = srt_accept(listener_, nullptr, nullptr);
        if (peer == SRT_INVALID_SOCK) {
          if (srt_getlasterror(nullptr) == SRT_EASYNCRCV) continue;
          break;
        }
      }
      char data[1456];
      const auto size = srt_recvmsg(peer, data, sizeof(data));
      if (size > 0) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++messages_;
          valid_ &= size % 188 == 0;
          for (int offset = 0; offset < size; offset += 188)
            valid_ &= static_cast<unsigned char>(data[offset]) == 0x47;
        }
        changed_.notify_all();
      } else if (srt_getlasterror(nullptr) != SRT_EASYNCRCV) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          error_ = srt_getlasterror_str();
        }
        changed_.notify_all();
        break;
      }
    }
    if (peer != SRT_INVALID_SOCK) srt_close(peer);
  }

  SRTSOCKET listener_ = SRT_INVALID_SOCK;
  std::uint16_t port_ = 0;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread thread_;
  bool stopped_ = false, valid_ = true;
  std::size_t messages_ = 0;
  std::string error_;
};

class DecoderInput final {
 public:
  explicit DecoderInput(const std::string& url) : input_(Config()) {
    input_.SetOnFrame([this](int, const ffmpeg::Frame& frame) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame->width > 0) {
          ++videos_;
          valid_ &= frame->width == 64 && frame->height == 64 && frame->data[0];
        } else {
          ++audios_;
          valid_ &= frame->sample_rate == 48000 && frame->nb_samples > 0;
        }
      }
      changed_.notify_all();
    });
    input_.SetOnStateChanged(
        [this](mw::streamer::InputState state, int, std::string_view error) {
          if (state == mw::streamer::InputState::kFailed) {
            {
              std::lock_guard<std::mutex> lock(mutex_);
              error_ = error;
            }
            changed_.notify_all();
          }
        });
    input_.Start(url);
  }
  ~DecoderInput() { input_.Stop(); }
  bool Wait(std::size_t videos = 3) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 8s,
                             [&] {
                               return (videos_ >= videos && audios_ > 0) ||
                                      !error_.empty();
                             }) &&
           valid_ && error_.empty();
  }
  std::size_t videos() {
    std::lock_guard<std::mutex> lock(mutex_);
    return videos_;
  }

 private:
  static mw::streamer::FfmpegInputConfig Config() {
    mw::streamer::FfmpegInputConfig config;
    config.auto_reconnect = false;
    config.open_timeout = 6s;
    config.read_timeout = 3s;
    return config;
  }
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t videos_ = 0, audios_ = 0;
  bool valid_ = true;
  std::string error_;
  mw::streamer::FfmpegInput input_;
};

TEST_CASE("动态RTMP RTSP SRT推流共享编码源且断线仅重连故障目标",
          "[remuxer][async][network][push]") {
  Runtime runtime(true);
  const auto encoded = Encode();
  RtmpReceiver rtmp;
  RtspServer rtsp;
  std::mutex errors_mutex;
  std::vector<std::string> errors;
  AsyncRemuxer remuxer;
  remuxer.SetOnError(
      [&](std::string_view target, int code, std::string_view message) {
        std::lock_guard<std::mutex> lock(errors_mutex);
        errors.push_back(std::string(target) + " code=" + std::to_string(code) +
                         " " + std::string(message));
      });
  remuxer.Start(encoded.streams);
  const auto first_name = Name();
  const auto second_name = Name();
  const auto first_url = rtmp.Url(first_name);
  const auto second_url = rtmp.Url(second_name);
  CHECK(remuxer.AddPushUrl(first_url) == first_url);
  Replay replay(remuxer, encoded);
  REQUIRE(rtmp.Wait(first_name, 25));
  const auto before = rtmp.Get(first_name);
  CHECK(remuxer.AddPushUrl(first_url) == first_url);
  CHECK(remuxer.AddPushUrl(second_url) == second_url);
  REQUIRE(rtmp.Wait(second_name, 10));
  const auto second = rtmp.Get(second_name);
  REQUIRE_FALSE(second.stamps.empty());
  CHECK(second.stamps.front() > before.stamps.front());
  CHECK(rtmp.Get(first_name).connections == 1);
  bool shared_payload = false;
  const auto first = rtmp.Get(first_name);
  for (std::size_t left = 0; left < first.stamps.size(); ++left)
    for (std::size_t right = 0; right < second.stamps.size(); ++right)
      if (first.stamps[left] == second.stamps[right]) {
        CHECK(first.video[left] == second.video[right]);
        shared_payload = true;
      }
  CHECK(shared_payload);

  const auto rtsp_name = Name();
  RegisteredSource received("network", rtsp_name);
  remuxer.AddPushUrl(rtsp.Url(rtsp_name));
  const auto rtsp_received = received.Wait();
  {
    std::lock_guard<std::mutex> lock(errors_mutex);
    std::string diagnostics;
    for (const auto& error : errors) diagnostics += error + '\n';
    INFO(diagnostics);
    INFO("RTSP target=" << rtsp.Url(rtsp_name));
    INFO("SDK diagnostics: async-remuxer-network-debug.log");
    REQUIRE(rtsp_received);
  }
  received.Read();
  REQUIRE(received.WaitPackets());
  SrtReceiver srt;
  remuxer.AddPushUrl(srt.Url());
  REQUIRE(srt.Wait());
  REQUIRE(srt.valid());

  const auto healthy_before = rtmp.Get(second_name);
  const auto disconnected_before = rtmp.Get(first_name);
  rtmp.Disconnect(first_name);
  REQUIRE(rtmp.Wait(second_name, healthy_before.stamps.size() + 10));
  REQUIRE(rtmp.Wait(first_name, disconnected_before.stamps.size() + 10, 2));
  const auto healthy_after = rtmp.Get(second_name);
  CHECK(healthy_after.connections == 1);
  CHECK(
      std::is_sorted(healthy_after.stamps.begin(), healthy_after.stamps.end()));
  CHECK(rtmp.Get(first_name).stamps.back() > disconnected_before.stamps.back());
  CHECK(rtmp.error().empty());
  replay.Stop();
  CHECK(replay.valid());
  remuxer.Stop();
}

TEST_CASE("本地RTSP发布重命名同源并共享监听且冲突不破坏已有流",
          "[remuxer][async][network][rtsp][lifecycle]") {
  Runtime runtime;
  const auto encoded = Encode();
  std::uint16_t port;
  {
    RtspServer reserve;
    port = reserve.port();
  }
  const auto app = Name();
  const auto first_name = Name();
  const auto second_name = Name();
  AsyncRemuxer first, second;
  first.Start(encoded.streams);
  second.Start(encoded.streams);
  first.AddRtspPublish(app, first_name, "127.0.0.1", port);
  CHECK_THROWS_AS(second.AddRtspPublish(app, first_name, "127.0.0.1", port),
                  std::invalid_argument);
  second.AddRtspPublish(app, second_name, "127.0.0.1", port);
  CHECK_THROWS(first.AddRtspPublish(app, Name(), "127.0.0.1", port));
  Replay first_replay(first, encoded);
  Replay second_replay(second, encoded);
  const auto base =
      "rtsp://127.0.0.1:" + std::to_string(port) + "/" + app + "/";
  {
    DecoderInput first_input(base + first_name);
    DecoderInput second_input(base + second_name);
    REQUIRE(first_input.Wait());
    REQUIRE(second_input.Wait());
    const auto healthy_count = second_input.videos();
    first_replay.Stop();
    first.Stop();
    REQUIRE(second_input.Wait(healthy_count + 5));
    {
      DecoderInput late_input(base + second_name);
      REQUIRE(late_input.Wait());
    }
  }
  second_replay.Stop();
  second.Stop();
  CHECK(first_replay.valid());
  CHECK(second_replay.valid());
  // All pulling clients and both publishers have released the shared server.
  REQUIRE_NOTHROW(RtspServer{port});
}

TEST_CASE("运行中添加本地RTSP发布保留媒体源身份和时间线",
          "[remuxer][async][network][rtsp][dynamic]") {
  Runtime runtime;
  const auto encoded = Encode();
  RtmpReceiver rtmp;
  AsyncRemuxer remuxer;
  remuxer.Start(encoded.streams);
  const auto target = Name();
  remuxer.AddPushUrl(rtmp.Url(target));
  Replay replay(remuxer, encoded);
  REQUIRE(rtmp.Wait(target, 25));
  mediakit::MediaSource::Ptr source;
  mediakit::MediaSource::for_each_media(
      [&](const auto& value) { source = value; }, RTSP_SCHEMA, DEFAULT_VHOST,
      "mw_remux");
  REQUIRE(source != nullptr);
  const auto old_tuple = source->getMediaTuple();
  std::uint16_t port;
  {
    RtspServer reserve;
    port = reserve.port();
  }
  const auto app = Name();
  const auto stream = Name();
  remuxer.AddRtspPublish(app, stream, "127.0.0.1", port);
  CHECK(mediakit::MediaSource::find(RTSP_SCHEMA, DEFAULT_VHOST, app, stream)
            .get() == source.get());
  CHECK(mediakit::MediaSource::find(RTSP_SCHEMA, DEFAULT_VHOST, old_tuple.app,
                                    old_tuple.stream) == nullptr);
  const auto rtmp_before = rtmp.Get(target);
  {
    DecoderInput input("rtsp://127.0.0.1:" + std::to_string(port) + "/" + app +
                       "/" + stream);
    REQUIRE(input.Wait());
    REQUIRE(rtmp.Wait(target, rtmp_before.stamps.size() + 5));
    std::uint32_t stamp = 0;
    const auto poller = source->getOwnerPoller();
    poller->sync([&] { stamp = source->getTimeStamp(mediakit::TrackVideo); });
    CHECK(stamp > rtmp_before.stamps.front());
    CHECK(rtmp.Get(target).connections == 1);
  }
  source.reset();
  replay.Stop();
  CHECK(replay.valid());
  remuxer.Stop();
  REQUIRE_NOTHROW(RtspServer{port});
}

}  // namespace
