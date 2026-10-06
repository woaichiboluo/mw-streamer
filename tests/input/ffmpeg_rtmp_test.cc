#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Network/Session.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Rtmp/RtmpProtocol.h"
#include "Rtmp/amf.h"
#include "mw/streamer/input/ffmpeg_input.h"

namespace {

using namespace std::chrono_literals;

struct ConnectState {
  std::mutex mutex;
  std::condition_variable changed;
  bool received = false;
  AMFValue codecs;
  std::string error;
  std::string media;
  std::vector<std::weak_ptr<toolkit::Session>> sessions;
};

// Exercise a real handshake and optionally serve a finite legacy HEVC stream.
class ConnectSession final : public toolkit::Session,
                             public mediakit::RtmpProtocol {
 public:
  explicit ConnectSession(const toolkit::Socket::Ptr& socket)
      : Session(socket) {}

  void Configure(std::shared_ptr<ConnectState> state) {
    state_ = std::move(state);
  }

  void onRecv(const toolkit::Buffer::Ptr& buffer) override {
    try {
      onParseRtmp(buffer->data(), buffer->size());
    } catch (const std::exception& error) {
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->error = error.what();
      }
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
    if (packet->type_id != MSG_CMD) return;
    AMFDecoder decoder(packet->buffer, 0);
    const auto name = decoder.load<std::string>();
    const auto transaction = decoder.load<double>();
    if (name == "connect") {
      const auto command = decoder.load<AMFValue>();
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->codecs = command["fourCcList"];
        state_->received = true;
      }
      state_->changed.notify_all();
      if (state_->media.empty()) return;
      sendChunkSize(4096);
      AMFValue properties(AMF_OBJECT);
      properties.set("fmsVer", "FMS/3,5,7,7009");
      properties.set("capabilities", 31);
      AMFValue info(AMF_OBJECT);
      info.set("level", "status");
      info.set("code", "NetConnection.Connect.Success");
      info.set("description", "Connection succeeded");
      info.set("objectEncoding", 0);
      AMFEncoder response;
      response << "_result" << transaction << properties << info;
      sendRtmp(MSG_CMD, STREAM_CONTROL, response.data(), 0, CHUNK_SYSTEM);
    } else if (name == "createStream" && !state_->media.empty()) {
      AMFEncoder response;
      response << "_result" << transaction << nullptr << STREAM_MEDIA;
      sendRtmp(MSG_CMD, STREAM_CONTROL, response.data(), 0, CHUNK_SYSTEM);
    } else if (name == "play" && !state_->media.empty()) {
      sendUserControl(CONTROL_STREAM_BEGIN, STREAM_MEDIA);
      SendStatus("NetStream.Play.Start");
      SendMedia();
      SendStatus("NetStream.Play.Stop");
    }
  }

 private:
  void SendStatus(const char* code) {
    AMFValue info(AMF_OBJECT);
    info.set("level", "status");
    info.set("code", code);
    AMFEncoder response;
    response << "onStatus" << 0 << nullptr << info;
    sendRtmp(MSG_CMD, STREAM_MEDIA, response.data(), 0, CHUNK_SYSTEM);
  }

  void SendMedia() {
    AMFValue metadata(AMF_ECMA_ARRAY);
    metadata.set("videocodecid", 12);
    metadata.set("audiocodecid", 10);
    metadata.set("width", 64);
    metadata.set("height", 64);
    metadata.set("framerate", 10);
    metadata.set("audiosamplerate", 48000);
    metadata.set("duration", 2);
    AMFEncoder encoded;
    encoded << "onMetaData" << metadata;
    sendRtmp(MSG_DATA, STREAM_MEDIA, encoded.data(), 0, CHUNK_SYSTEM);

    const auto* bytes = reinterpret_cast<const uint8_t*>(state_->media.data());
    for (size_t offset = 13; offset + 15 <= state_->media.size();) {
      const uint32_t size = (bytes[offset + 1] << 16) |
                            (bytes[offset + 2] << 8) | bytes[offset + 3];
      if (offset + size + 15 > state_->media.size()) {
        throw std::runtime_error("Invalid legacy FLV fixture");
      }
      const uint8_t type = bytes[offset];
      if (type != MSG_AUDIO && type != MSG_VIDEO) {
        throw std::runtime_error("Unexpected legacy FLV fixture tag");
      }
      if (type == MSG_VIDEO && (bytes[offset + 11] & 0x8f) != 12) {
        throw std::runtime_error("Fixture must use legacy CodecID 12");
      }
      const uint32_t timestamp =
          (static_cast<uint32_t>(bytes[offset + 7]) << 24) |
          (bytes[offset + 4] << 16) | (bytes[offset + 5] << 8) |
          bytes[offset + 6];
      sendRtmp(type, STREAM_MEDIA, state_->media.substr(offset + 11, size),
               timestamp, type == MSG_VIDEO ? CHUNK_VIDEO : CHUNK_AUDIO);
      offset += size + 15;
    }
  }

  std::shared_ptr<ConnectState> state_;
};

class ConnectServer final {
 public:
  explicit ConnectServer(bool serve_legacy_media = false)
      : state_(std::make_shared<ConnectState>()),
        poller_(toolkit::EventPollerPool::Instance().getPoller()),
        server_(std::make_shared<toolkit::TcpServer>(poller_)) {
    if (serve_legacy_media) {
      std::ifstream file(std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) +
                             "/h265_aac_codec12.flv",
                         std::ios::binary);
      if (!file) throw std::runtime_error("Cannot open legacy FLV fixture");
      state_->media.assign(std::istreambuf_iterator<char>(file), {});
    }
    server_->start<ConnectSession>(
        0, "127.0.0.1", 1024,
        [state = state_](std::shared_ptr<ConnectSession>& session) {
          session->Configure(state);
          std::lock_guard<std::mutex> lock(state->mutex);
          state->sessions.push_back(session);
        });
  }

  ~ConnectServer() {
    std::vector<std::shared_ptr<toolkit::Session>> sessions;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      for (const auto& weak : state_->sessions) {
        if (auto session = weak.lock()) sessions.push_back(std::move(session));
      }
    }
    for (auto& session : sessions) {
      const auto session_poller = session->getPoller();
      session_poller->sync([session = std::move(session)]() mutable {
        session->shutdown();
        session.reset();
      });
    }
    poller_->sync([this] { server_.reset(); });
  }

  std::string url() const {
    return "rtmp://127.0.0.1:" + std::to_string(server_->getPort()) +
           "/live/hevc";
  }

  bool WaitForConnect() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(
        lock, 5s, [&] { return state_->received || !state_->error.empty(); });
  }

  const std::shared_ptr<ConnectState>& state() const { return state_; }

 private:
  std::shared_ptr<ConnectState> state_;
  toolkit::EventPoller::Ptr poller_;
  toolkit::TcpServer::Ptr server_;
};

}  // namespace

TEST_CASE("FFmpeg RTMP input advertises Enhanced RTMP HEVC support",
          "[input][ffmpeg][rtmp]") {
  ConnectServer server;
  mw::streamer::FfmpegInputConfig config;
  config.auto_reconnect = false;
  config.open_timeout = 10s;
  mw::streamer::FfmpegInput input(config);
  input.Start(server.url());
  const bool received = server.WaitForConnect();
  // No connect response is sent; Stop must interrupt the pending open.
  const auto stopping = std::chrono::steady_clock::now();
  input.Stop();
  CHECK(std::chrono::steady_clock::now() - stopping < 2s);
  CHECK(input.state() == mw::streamer::InputState::kStopped);
  REQUIRE(received);

  std::lock_guard<std::mutex> lock(server.state()->mutex);
  INFO(server.state()->error);
  REQUIRE(server.state()->error.empty());
  REQUIRE(server.state()->received);
  REQUIRE(server.state()->codecs.type() == AMF_STRICT_ARRAY);
  AMFValue expected_codecs(AMF_STRICT_ARRAY);
  expected_codecs.add("hvc1");
  AMFEncoder expected;
  expected << expected_codecs;
  AMFEncoder actual;
  actual << server.state()->codecs;
  CHECK(actual.data() == expected.data());
}

TEST_CASE("FFmpeg RTMP input decodes legacy HEVC CodecID 12",
          "[input][ffmpeg][rtmp][codec12]") {
  ConnectServer server(true);
  mw::streamer::FfmpegInputConfig config;
  config.auto_reconnect = false;
  config.open_timeout = 5s;
  config.read_timeout = 3s;
  mw::streamer::FfmpegInput input(config);
  std::mutex mutex;
  std::condition_variable changed;
  int video_index = -1;
  int audio_index = -1;
  AVRational video_time_base{0, 1};
  std::vector<int64_t> video_pts;
  size_t audio_frames = 0;
  bool valid_video = true;
  bool valid_audio = true;
  bool ended = false;
  bool finished = false;
  std::string error;
  input.SetOnReady([&](const auto& streams) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& stream : streams) {
      const auto* parameters = stream.codec_parameters.get();
      if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        video_index = stream.stream_index;
        video_time_base = stream.time_base;
      } else if (parameters->codec_id == AV_CODEC_ID_AAC) {
        audio_index = stream.stream_index;
      }
    }
  });
  input.SetOnFrame([&](int index, const auto& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index == video_index) {
      valid_video &=
          frame->width == 64 && frame->height == 64 &&
          frame->data[0] != nullptr &&
          av_cmp_q(frame->time_base, AVRational{1, 1000000000}) == 0 &&
          frame->pkt_dts == AV_NOPTS_VALUE;
      video_pts.push_back(frame->pts);
    } else if (index == audio_index) {
      valid_audio &= frame->sample_rate == 48000 && frame->nb_samples > 0 &&
                     frame->data[0] != nullptr;
      ++audio_frames;
    }
  });
  input.SetOnStateChanged([&](auto state, int, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == mw::streamer::InputState::kEnded ||
        state == mw::streamer::InputState::kFailed) {
      ended = state == mw::streamer::InputState::kEnded;
      error = message;
      finished = true;
      changed.notify_all();
    }
  });
  input.Start(server.url());
  bool completed;
  {
    std::unique_lock<std::mutex> lock(mutex);
    completed = changed.wait_for(lock, 8s, [&] { return finished; });
  }
  input.Stop();
  REQUIRE(completed);
  INFO(error);
  CHECK(finished);
  CHECK(ended);
  CHECK(error.empty());
  REQUIRE(video_index >= 0);
  REQUIRE(audio_index >= 0);
  CHECK(valid_video);
  CHECK(valid_audio);
  REQUIRE(video_pts.size() == 20);
  CHECK(audio_frames >= 94);
  REQUIRE(video_time_base.num > 0);
  for (size_t i = 1; i < video_pts.size(); ++i) {
    const auto elapsed = (video_pts[i] - video_pts[i - 1]) / 1000000000.0;
    CHECK(elapsed == Catch::Approx(0.1));
  }
  std::lock_guard<std::mutex> lock(server.state()->mutex);
  INFO(server.state()->error);
  CHECK(server.state()->error.empty());
}
