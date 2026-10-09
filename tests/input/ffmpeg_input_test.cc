#include "mw/streamer/input/ffmpeg_input.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "Network/Session.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputMode;
using mw::streamer::InputState;
namespace ffmpeg = mw::streamer::ffmpeg;

constexpr AVRational kNanoseconds{1, 1000000000};

std::string SamplePath(std::string_view name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/" + std::string(name);
}

class TemporaryMediaFile final {
 public:
  TemporaryMediaFile()
      : path_(std::filesystem::temp_directory_path() /
              ("mw-streamer-loop-" +
               std::to_string(Clock::now().time_since_epoch().count()) +
               ".mp4")) {
    Restore();
  }
  ~TemporaryMediaFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string path() const { return path_.string(); }
  void Clear() {
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot truncate temporary media");
  }
  void Restore() {
    std::filesystem::copy_file(
        SamplePath("seek_tail_h264.mp4"), path_,
        std::filesystem::copy_options::overwrite_existing);
  }

 private:
  std::filesystem::path path_;
};

struct ServerState {
  std::mutex mutex;
  std::condition_variable changed;
  std::string flv;
  std::string content_type;
  enum class Response {
    kMedia,
    kTruncatedMedia,
    kFailure,
    kStall,
    kStallAfterMedia
  } response;
  struct RequestResponse {
    Response response;
    std::string media;
    std::string content_type;
  };
  std::vector<RequestResponse> sequence;
  size_t requests = 0;
  std::vector<std::weak_ptr<toolkit::Session>> sessions;
};

// Exact-length media ends normally; a truncated response simulates read errors.
class HttpSession final : public toolkit::Session {
 public:
  explicit HttpSession(const toolkit::Socket::Ptr& socket) : Session(socket) {}
  void Configure(std::shared_ptr<ServerState> state) {
    state_ = std::move(state);
  }
  void onRecv(const toolkit::Buffer::Ptr& buffer) override {
    if (responded_) return;
    request_.append(buffer->data(), buffer->size());
    if (request_.find("\r\n\r\n") == std::string::npos) return;
    responded_ = true;
    ServerState::Response response;
    std::string media;
    std::string content_type;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      ++state_->requests;
      response = state_->response;
      media = state_->flv;
      content_type = state_->content_type;
      if (!state_->sequence.empty()) {
        const auto& selected = state_->sequence[std::min(
            state_->requests - 1, state_->sequence.size() - 1)];
        response = selected.response;
        media = selected.media;
        content_type = selected.content_type;
      }
    }
    state_->changed.notify_all();
    if (response == ServerState::Response::kStall) return;
    if (response == ServerState::Response::kFailure) {
      send(
          "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n");
      return;
    }
    if (response == ServerState::Response::kStallAfterMedia) {
      send("HTTP/1.1 200 OK\r\nContent-Type: " + content_type +
           "\r\nConnection: close\r\n\r\n" + media);
      return;
    }
    close_after_send_ = response == ServerState::Response::kTruncatedMedia;
    const auto declared_size = media.size() + (close_after_send_ ? 1024 : 0);
    send("HTTP/1.1 200 OK\r\nContent-Type: " + content_type +
         "\r\nContent-Length: " + std::to_string(declared_size) +
         "\r\nConnection: close\r\n\r\n" + media);
    if (close_after_send_ && !isSocketBusy()) safeShutdown();
  }
  void onFlush() override {
    if (close_after_send_) safeShutdown();
  }
  void onError(const toolkit::SockException&) override {}
  void onManager() override {}

 private:
  std::shared_ptr<ServerState> state_;
  std::string request_;
  bool responded_ = false;
  bool close_after_send_ = false;
};

class HttpServer final {
 public:
  explicit HttpServer(ServerState::Response response,
                      uint32_t video_timestamp_offset_ms = 0,
                      std::string_view fixture = "h264_aac.flv")
      : state_(std::make_shared<ServerState>()),
        poller_(toolkit::EventPollerPool::Instance().getPoller()),
        server_(std::make_shared<toolkit::TcpServer>(poller_)) {
    state_->response = response;
    state_->content_type =
        fixture == "h264_aac.flv" ? "video/x-flv" : "video/mp2t";
    std::ifstream file(SamplePath(fixture), std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open media fixture");
    state_->flv.assign(std::istreambuf_iterator<char>(file), {});
    if (video_timestamp_offset_ms != 0) {
      ShiftVideoTimestamps(video_timestamp_offset_ms);
    }
    if (response == ServerState::Response::kStallAfterMedia &&
        fixture == "h264_aac.flv") {
      ExtendMedia();
    }
    server_->start<HttpSession>(
        0, "127.0.0.1", 1024,
        [state = state_](std::shared_ptr<HttpSession>& session) {
          session->Configure(state);
          std::lock_guard<std::mutex> lock(state->mutex);
          state->sessions.push_back(session);
        });
    url_ =
        "http://127.0.0.1:" + std::to_string(server_->getPort()) + "/live.flv";
  }
  ~HttpServer() {
    std::vector<std::shared_ptr<toolkit::Session>> sessions;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      for (const auto& weak : state_->sessions) {
        if (auto session = weak.lock()) sessions.push_back(std::move(session));
      }
    }
    for (const auto& session : sessions) {
      session->getPoller()->sync([session] { session->shutdown(); });
    }
    poller_->sync([this] { server_.reset(); });
  }
  const std::string& url() const { return url_; }
  void SetResponseSequence(
      const std::vector<std::pair<ServerState::Response, std::string>>&
          sequence) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->sequence.clear();
    for (const auto& entry : sequence) {
      if (entry.second.empty()) {
        // Valid metadata and codec initialization, with no decodable packets.
        std::string headers = state_->flv.substr(0, 13);
        for (size_t offset = 13; offset + 15 <= state_->flv.size();) {
          const auto* bytes =
              reinterpret_cast<const uint8_t*>(state_->flv.data());
          const size_t size = (bytes[offset + 1] << 16) |
                              (bytes[offset + 2] << 8) | bytes[offset + 3];
          if (offset + 15 + size > state_->flv.size()) {
            throw std::runtime_error("Invalid FLV fixture tag");
          }
          if (bytes[offset] == 18 ||
              (size >= 2 && bytes[offset + 12] == 0 &&
               ((bytes[offset] == 8 && bytes[offset + 11] >> 4 == 10) ||
                (bytes[offset] == 9 && (bytes[offset + 11] & 15) == 7)))) {
            headers.append(state_->flv, offset, 15 + size);
          }
          offset += 15 + size;
        }
        state_->sequence.push_back(
            {entry.first, std::move(headers), "application/octet-stream"});
        continue;
      }
      std::ifstream file(SamplePath(entry.second), std::ios::binary);
      if (!file) throw std::runtime_error("Cannot open response fixture");
      std::string media(std::istreambuf_iterator<char>(file), {});
      state_->sequence.push_back(
          {entry.first, std::move(media), "application/octet-stream"});
    }
  }
  size_t requests() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->requests;
  }
  bool WaitForRequest() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(lock, 5s,
                                    [&] { return state_->requests > 0; });
  }
  bool HasNoNewRequests(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    const auto count = state_->requests;
    return !state_->changed.wait_for(lock, duration,
                                     [&] { return state_->requests != count; });
  }

 private:
  void ShiftVideoTimestamps(uint32_t shift) {
    for (size_t offset = 13; offset + 15 <= state_->flv.size();) {
      const auto* bytes = reinterpret_cast<const uint8_t*>(state_->flv.data());
      const uint32_t size = (bytes[offset + 1] << 16) |
                            (bytes[offset + 2] << 8) | bytes[offset + 3];
      if (offset + 15 + size > state_->flv.size()) {
        throw std::runtime_error("Invalid FLV fixture tag");
      }
      if (bytes[offset] == 9) {
        const uint32_t timestamp = (bytes[offset + 7] << 24) |
                                   (bytes[offset + 4] << 16) |
                                   (bytes[offset + 5] << 8) | bytes[offset + 6];
        const uint32_t shifted = timestamp + shift;
        state_->flv[offset + 4] = static_cast<char>(shifted >> 16);
        state_->flv[offset + 5] = static_cast<char>(shifted >> 8);
        state_->flv[offset + 6] = static_cast<char>(shifted);
        state_->flv[offset + 7] = static_cast<char>(shifted >> 24);
      }
      offset += 15 + size;
    }
  }

  // Repeat the fixture's AV tags with shifted DTS. Eight seconds gives probing
  // enough media to finish before the connection deliberately stops sending.
  void ExtendMedia() {
    const auto original = state_->flv;
    for (uint32_t repetition = 1; repetition <= 3; ++repetition) {
      for (size_t offset = 13; offset + 15 <= original.size();) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(original.data());
        const uint32_t size = (bytes[offset + 1] << 16) |
                              (bytes[offset + 2] << 8) | bytes[offset + 3];
        if (offset + 15 + size > original.size()) {
          throw std::runtime_error("Invalid FLV fixture tag");
        }
        if (bytes[offset] == 8 || bytes[offset] == 9) {
          auto tag = original.substr(offset, 15 + size);
          const uint32_t timestamp =
              (bytes[offset + 7] << 24) | (bytes[offset + 4] << 16) |
              (bytes[offset + 5] << 8) | bytes[offset + 6];
          const auto shifted = timestamp + repetition * 2000;
          tag[4] = static_cast<char>(shifted >> 16);
          tag[5] = static_cast<char>(shifted >> 8);
          tag[6] = static_cast<char>(shifted);
          tag[7] = static_cast<char>(shifted >> 24);
          state_->flv += tag;
        }
        offset += 15 + size;
      }
    }
  }

  std::shared_ptr<ServerState> state_;
  toolkit::EventPoller::Ptr poller_;
  toolkit::TcpServer::Ptr server_;
  std::string url_;
};

struct FrameObservation {
  int stream_index;
  double media_time;
  Clock::time_point received;
  AVMediaType type;
  int64_t pts;
  size_t generation;
  std::thread::id thread;
  int64_t timestamp_ns;
  int64_t duration_ns;
};

struct Observation {
  std::vector<InputState> states;
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<FrameObservation> frames;
  std::vector<std::thread::id> media_threads;
  std::vector<Clock::time_point> ready_times;
  std::vector<std::thread::id> state_threads;
  size_t ready = 0;
  size_t callbacks = 0;
  bool valid = true;
  int error = 0;
  std::string message;
};

class Observer final {
 public:
  void TrackLoops() { track_loops_ = true; }
  void SetFrameHook(std::function<void(const ffmpeg::Frame&)> hook) {
    frame_hook_ = std::move(hook);
  }
  void SetReadyHook(std::function<void()> hook) {
    ready_hook_ = std::move(hook);
  }
  void Attach(FfmpegInput& input) {
    input.SetOnReady([this](const std::vector<ffmpeg::StreamInfo>& streams) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++value_.callbacks;
        ++value_.ready;
        value_.ready_times.push_back(Clock::now());
        value_.streams = streams;
        for (const auto& stream : streams) {
          value_.valid &= stream.codec_parameters.get() &&
                          stream.stream_index >= 0 &&
                          stream.time_base.num > 0 && stream.time_base.den > 0;
        }
      }
      changed_.notify_all();
      if (ready_hook_) ready_hook_();
    });
    input.SetOnFrame([this](int index, const ffmpeg::Frame& frame) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++value_.callbacks;
        const auto stream = std::find_if(
            value_.streams.begin(), value_.streams.end(),
            [index](const auto& info) { return info.stream_index == index; });
        if (stream == value_.streams.end() || !frame.get() || !frame->data[0] ||
            !frame->buf[0] || frame->pts == AV_NOPTS_VALUE ||
            av_cmp_q(frame->time_base, kNanoseconds) != 0 ||
            frame->pkt_dts != AV_NOPTS_VALUE) {
          value_.valid = false;
        } else {
          const auto type = stream->codec_parameters.get()->codec_type;
          const auto offset =
              frame->pts - av_rescale_q(frame->best_effort_timestamp,
                                        stream->time_base, kNanoseconds);
          if (track_loops_ && loop_offset_ && *loop_offset_ != offset) {
            ++generation_;
            value_.media_threads.push_back(std::this_thread::get_id());
          }
          loop_offset_ = offset;
          value_.frames.push_back(
              {index,
               static_cast<double>(frame->best_effort_timestamp) *
                   av_q2d(stream->time_base),
               Clock::now(), type, frame->best_effort_timestamp, generation_,
               std::this_thread::get_id(), frame->pts, frame->duration});
          if (type == AVMEDIA_TYPE_VIDEO && !retained_) {
            retained_.emplace(frame.Ref());
            // Retain one visible row, independent of allocation padding.
            retained_bytes_.assign(frame->data[0],
                                   frame->data[0] + frame->width);
          }
        }
      }
      changed_.notify_all();
      if (frame_hook_) frame_hook_(frame);
    });
    input.SetOnStateChanged(
        [this](InputState state, int error, std::string_view message) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            ++value_.callbacks;
            if (state == InputState::kConnected) {
              ++generation_;
              loop_offset_.reset();
              value_.media_threads.push_back(std::this_thread::get_id());
            }
            value_.states.push_back(state);
            value_.state_threads.push_back(std::this_thread::get_id());
            value_.error = error;
            value_.message = message;
          }
          changed_.notify_all();
        });
  }
  template <class Predicate>
  bool Wait(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return predicate(value_); });
  }
  bool WaitState(InputState state) {
    return Wait([state](const auto& value) {
      return std::find(value.states.begin(), value.states.end(), state) !=
             value.states.end();
    });
  }
  Observation snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }
  bool HasNoNewCallbacks(std::chrono::milliseconds duration = 150ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto count = value_.callbacks;
    return !changed_.wait_for(lock, duration,
                              [&] { return value_.callbacks != count; });
  }
  bool RetainedFrameIsValid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retained_ && retained_->get()->buf[0] &&
           std::equal(retained_bytes_.begin(), retained_bytes_.end(),
                      retained_->get()->data[0]);
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Observation value_;
  std::optional<ffmpeg::Frame> retained_;
  std::vector<uint8_t> retained_bytes_;
  std::function<void(const ffmpeg::Frame&)> frame_hook_;
  std::function<void()> ready_hook_;
  bool track_loops_ = false;
  size_t generation_ = 0;
  std::optional<int64_t> loop_offset_;
};

// Pauses delivery after recording a video frame. Destruction releases the
// callback before Input is destroyed; the callback owns its shared state.
class CallbackPause final {
 public:
  CallbackPause() : state_(std::make_shared<State>()) {}
  ~CallbackPause() { Release(); }
  std::function<void(const ffmpeg::Frame&)> hook() const {
    return [state = state_](const ffmpeg::Frame& frame) {
      if (frame->width <= 0) return;
      std::unique_lock<std::mutex> lock(state->mutex);
      if (!state->armed || frame->best_effort_timestamp < state->at) {
        return;
      }
      state->armed = false;
      state->released = false;
      ++state->pauses;
      state->changed.notify_all();
      state->changed.wait(lock, [&] { return state->released; });
    };
  }
  void Arm(int64_t media_pts) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->at = media_pts;
    state_->armed = true;
  }
  bool Wait(size_t pauses, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout,
                                    [&] { return state_->pauses >= pauses; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->released = true;
    state_->changed.notify_all();
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    int64_t at = 0;
    bool armed = false;
    bool released = false;
    size_t pauses = 0;
  };
  std::shared_ptr<State> state_;
};

size_t StateCount(const Observation& value, InputState state) {
  return static_cast<size_t>(
      std::count(value.states.begin(), value.states.end(), state));
}

int64_t MediaPtsNs(const Observation& value, const FrameObservation& frame) {
  const auto stream = std::find_if(
      value.streams.begin(), value.streams.end(), [&](const auto& information) {
        return information.stream_index == frame.stream_index;
      });
  REQUIRE(stream != value.streams.end());
  return av_rescale_q(frame.pts, stream->time_base, kNanoseconds);
}

void CheckTimestampMapping(const Observation& value, size_t generation) {
  const auto first = std::find_if(value.frames.begin(), value.frames.end(),
                                  [generation](const auto& frame) {
                                    return frame.generation == generation;
                                  });
  REQUIRE(first != value.frames.end());
  const auto offset = first->timestamp_ns - MediaPtsNs(value, *first);
  for (const auto& frame : value.frames) {
    if (frame.generation != generation) continue;
    CHECK(frame.timestamp_ns - MediaPtsNs(value, frame) == offset);
    CHECK(frame.duration_ns > 1000000);
  }
}

FfmpegInputConfig RetryConfig(int retries = -1,
                              std::chrono::milliseconds interval = 30ms) {
  FfmpegInputConfig config;
  config.retry_interval = interval;
  config.max_retries = retries;
  return config;
}

struct ReferenceFrames {
  std::vector<int64_t> pts;
  AVRational time_base{0, 1};
  double end_time = 0;
  size_t drained_frames = 0;
  size_t key_frames = 0;
  int has_b_frames = 0;
};

// Direct, unpaced libavformat/libavcodec decode is the independent EOF oracle.
ReferenceFrames DecodeReference(
    const std::string& path, AVMediaType type = AVMEDIA_TYPE_VIDEO,
    std::optional<std::chrono::milliseconds> seek = std::nullopt) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "open reference");
  auto close = [](AVFormatContext* context) { avformat_close_input(&context); };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "inspect reference");
  const AVCodec* codec = nullptr;
  const int index = av_find_best_stream(raw, type, -1, -1, &codec, 0);
  ffmpeg::FfmpegException::throwIfError(index, "select reference track");
  ffmpeg::CodecContext context(codec);
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_to_context(context.get(),
                                    raw->streams[index]->codecpar),
      "configure reference");
  context.get()->pkt_timebase = raw->streams[index]->time_base;
  ffmpeg::FfmpegException::throwIfError(
      avcodec_open2(context.get(), codec, nullptr), "open reference decoder");
  ReferenceFrames result;
  result.time_base = raw->streams[index]->time_base;
  result.has_b_frames = context.get()->has_b_frames;
  if (seek) {
    int seek_index =
        av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (seek_index < 0) seek_index = index;
    const int64_t start =
        raw->start_time == AV_NOPTS_VALUE ? 0 : raw->start_time;
    const int64_t timestamp =
        av_rescale_q(start + seek->count() * 1000, AVRational{1, AV_TIME_BASE},
                     raw->streams[seek_index]->time_base);
    ffmpeg::FfmpegException::throwIfError(
        av_seek_frame(raw, seek_index, timestamp, AVSEEK_FLAG_BACKWARD),
        "seek reference");
    avcodec_flush_buffers(context.get());
  }
  ffmpeg::Frame frame;
  const auto receive = [&](bool draining) {
    while (true) {
      const int error = avcodec_receive_frame(context.get(), frame.get());
      if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return;
      ffmpeg::FfmpegException::throwIfError(error, "decode reference");
      result.pts.push_back(frame->best_effort_timestamp);
      const double duration =
          frame->duration != 0
              ? static_cast<double>(frame->duration) * av_q2d(result.time_base)
              : (type == AVMEDIA_TYPE_AUDIO
                     ? static_cast<double>(frame->nb_samples) /
                           frame->sample_rate
                     : av_q2d(av_inv_q(raw->streams[index]->avg_frame_rate)));
      result.end_time = std::max(
          result.end_time, static_cast<double>(frame->best_effort_timestamp) *
                                   av_q2d(result.time_base) +
                               duration);
      result.drained_frames += draining;
      result.key_frames += (frame->flags & AV_FRAME_FLAG_KEY) != 0;
      frame.Unref();
    }
  };
  ffmpeg::Packet packet;
  int error;
  while ((error = av_read_frame(raw, packet.get())) >= 0) {
    if (packet->stream_index == index) {
      ffmpeg::FfmpegException::throwIfError(
          avcodec_send_packet(context.get(), packet.get()), "send reference");
      receive(false);
    }
    packet.Unref();
  }
  if (error != AVERROR_EOF) {
    ffmpeg::FfmpegException::throwIfError(error, "read reference");
  }
  ffmpeg::FfmpegException::throwIfError(
      avcodec_send_packet(context.get(), nullptr), "drain reference");
  receive(true);
  return result;
}

}  // namespace

TEST_CASE("FFmpeg input schedules both tracks and drains B frames at EOF") {
  const auto reference = DecodeReference(SamplePath("h265_aac.mp4"));
  REQUIRE(reference.has_b_frames > 0);
  REQUIRE(reference.drained_frames > 0);
  Observer observer;
  auto config = FfmpegInputConfig{};
  config.video_decoder_name = "hevc";
  config.audio_decoder_name = "aac";
  FfmpegInput input(config);
  observer.Attach(input);
  CHECK(input.state() == InputState::kIdle);
  input.Start(SamplePath("h265_aac.mp4"));
  REQUIRE(observer.WaitState(InputState::kEnded));
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  REQUIRE(value.streams.size() == 2);
  CHECK(value.streams[0].time_base.num == 1);
  CHECK(value.streams[0].time_base.den == 10240);
  CHECK(value.streams[1].time_base.num == 1);
  CHECK(value.streams[1].time_base.den == 48000);
  std::vector<FrameObservation> video;
  std::vector<FrameObservation> audio;
  for (const auto& frame : value.frames) {
    (frame.type == AVMEDIA_TYPE_VIDEO ? video : audio).push_back(frame);
  }
  REQUIRE(video.size() == reference.pts.size());
  REQUIRE(audio.size() > 80);
  for (size_t i = 0; i < video.size(); ++i) {
    CHECK(video[i].pts == reference.pts[i]);
  }
  CheckTimestampMapping(value, 1);
  const auto video_elapsed = std::chrono::duration<double>(
                                 video.back().received - video.front().received)
                                 .count();
  CHECK(video_elapsed >= 1.5);
  const auto& first = value.frames.front();
  for (const auto& frame : value.frames) {
    const double wall =
        std::chrono::duration<double>(frame.received - first.received).count();
    CHECK(std::abs(wall - (frame.media_time - first.media_time)) < 0.4);
  }
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  input.Stop();
  CHECK(observer.RetainedFrameIsValid());
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg input seeks locally without rebasing media PTS or waiting") {
  Observer observer;
  FfmpegInput input;
  CallbackPause pause;
  pause.Arm(0);
  observer.SetFrameHook(pause.hook());
  observer.Attach(input);
  input.Start(SamplePath("seek_h264_aac.mp4"));
  REQUIRE(pause.Wait(1));
  size_t final_begin = 0;
  ReferenceFrames final_video;
  ReferenceFrames final_audio;
  size_t phase = 0;
  for (const auto position : {6500ms, 2500ms, 4500ms, 9500ms}) {
    CAPTURE(position.count());
    const auto video = DecodeReference(SamplePath("seek_h264_aac.mp4"),
                                       AVMEDIA_TYPE_VIDEO, position);
    const auto audio = DecodeReference(SamplePath("seek_h264_aac.mp4"),
                                       AVMEDIA_TYPE_AUDIO, position);
    REQUIRE_FALSE(video.pts.empty());
    REQUIRE_FALSE(audio.pts.empty());
    const auto begin = observer.snapshot().frames.size();
    pause.Arm(video.pts.front());
    // Both requests are queued while the media callback is held. Only the
    // newest position should execute when the media worker resumes.
    if (phase == 0) input.Seek(2500ms);
    const auto requested = Clock::now();
    input.Seek(position);
    pause.Release();
    REQUIRE(pause.Wait(++phase + 1, 1s));
    const auto value = observer.snapshot();
    std::vector<FrameObservation> delivered;
    for (size_t i = begin; i < value.frames.size(); ++i) {
      if (value.frames[i].type == AVMEDIA_TYPE_VIDEO) {
        delivered.push_back(value.frames[i]);
      }
    }
    REQUIRE(delivered.size() == 1);
    CHECK(delivered.front().pts == video.pts.front());
    const auto previous = std::find_if(
        value.frames.rbegin() +
            static_cast<std::vector<FrameObservation>::difference_type>(
                value.frames.size() - begin),
        value.frames.rend(),
        [](const auto& frame) { return frame.type == AVMEDIA_TYPE_VIDEO; });
    REQUIRE(previous != value.frames.rend());
    CHECK(delivered.front().timestamp_ns - previous->timestamp_ns ==
          MediaPtsNs(value, delivered.front()) - MediaPtsNs(value, *previous));
    if (position == 2500ms) {
      CHECK(delivered.front().timestamp_ns < previous->timestamp_ns);
    }
    CHECK(delivered.front().received - requested < 400ms);
    CHECK(delivered.front().thread == value.media_threads.front());
    CHECK(value.ready == 1);
    CHECK(StateCount(value, InputState::kConnecting) == 1);
    CHECK(StateCount(value, InputState::kConnected) == 1);
    CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
    CHECK(StateCount(value, InputState::kFailed) == 0);
    if (position == 9500ms) {
      final_begin = begin;
      final_video = video;
      final_audio = audio;
    }
  }
  REQUIRE(final_video.has_b_frames > 0);
  REQUIRE(final_video.drained_frames > 0);
  pause.Release();
  REQUIRE(observer.WaitState(InputState::kEnded));
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CheckTimestampMapping(value, 1);
  std::vector<int64_t> video_pts;
  std::vector<int64_t> audio_pts;
  const auto& first = value.frames[final_begin];
  for (size_t i = final_begin; i < value.frames.size(); ++i) {
    const auto& frame = value.frames[i];
    (frame.type == AVMEDIA_TYPE_VIDEO ? video_pts : audio_pts)
        .push_back(frame.pts);
    CHECK(frame.thread == value.media_threads.front());
    CHECK(
        std::abs(std::chrono::duration<double>(frame.received - first.received)
                     .count() -
                 (frame.media_time - first.media_time)) < 0.4);
  }
  CHECK(video_pts == final_video.pts);
  CHECK(audio_pts == final_audio.pts);
  // Seek does not resurrect an input whose media worker has already ended.
  input.Seek(1s);
  CHECK(observer.HasNoNewCallbacks());
  CHECK(input.state() == InputState::kEnded);
  input.Stop();
  CHECK(observer.RetainedFrameIsValid());
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg inputs use preparation anchors on a shared epoch",
          "[input][timing][startup]") {
  Observer first_observer;
  Observer delayed_observer;
  FfmpegInput first_input;
  FfmpegInput delayed_input;
  first_observer.Attach(first_input);
  delayed_observer.SetReadyHook([] { std::this_thread::sleep_for(200ms); });
  delayed_observer.Attach(delayed_input);
  first_input.Start(SamplePath("h265_aac.mp4"));
  REQUIRE(first_observer.Wait(
      [](const auto& value) { return !value.frames.empty(); }));
  first_input.Stop();
  delayed_input.Start(SamplePath("h265_aac.mp4"));
  REQUIRE(delayed_observer.Wait(
      [](const auto& value) { return !value.frames.empty(); }));
  delayed_input.Stop();
  const auto first = first_observer.snapshot();
  const auto delayed = delayed_observer.snapshot();
  REQUIRE(first.valid);
  REQUIRE(delayed.valid);
  REQUIRE(first.ready_times.size() == 1);
  REQUIRE(delayed.ready_times.size() == 1);
  CheckTimestampMapping(first, 1);
  CheckTimestampMapping(delayed, 1);
  CHECK(first.frames.front().media_time == delayed.frames.front().media_time);
  CHECK(delayed.frames.front().received - delayed.ready_times.front() >= 200ms);
  const auto timestamp_gap =
      delayed.frames.front().timestamp_ns - first.frames.front().timestamp_ns;
  const auto delivery_gap =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          delayed.frames.front().received - first.frames.front().received)
          .count();
  // Both runs use the same epoch and media origin. Ready initialization must
  // advance the first output timestamp along with its actual delivery time.
  CHECK(std::abs(delivery_gap - timestamp_gap) < 100000000);
}

TEST_CASE("FFmpeg input Seek validates local active input and clears on Stop") {
  Observer observer;
  FfmpegInput input;
  observer.Attach(input);
  CHECK_THROWS_AS(input.Seek(-1ms), std::invalid_argument);
  input.Seek(6s);
  CHECK(input.state() == InputState::kIdle);
  HttpServer server(ServerState::Response::kStall);
  input.Start(server.url());
  REQUIRE(server.WaitForRequest());
  CHECK_THROWS_AS(input.Seek(1s), std::invalid_argument);
  input.Stop();
  input.Seek(6s);
  CHECK(input.state() == InputState::kStopped);
  CallbackPause pause;
  pause.Arm(0);
  observer.SetFrameHook(pause.hook());
  input.Start(SamplePath("seek_h264_aac.mp4"));
  REQUIRE(pause.Wait(1));
  input.Seek(6s);
  pause.Release();
  input.Stop();
  CHECK(observer.HasNoNewCallbacks());
  const auto stopped = observer.snapshot();
  input.Start(SamplePath("seek_h264_aac.mp4"));
  REQUIRE(observer.Wait([&](const auto& value) {
    return value.frames.size() > stopped.frames.size();
  }));
  input.Stop();
  const auto restarted = observer.snapshot();
  const auto first = std::find_if(
      restarted.frames.begin() +
          static_cast<std::vector<FrameObservation>::difference_type>(
              stopped.frames.size()),
      restarted.frames.end(),
      [](const auto& frame) { return frame.type == AVMEDIA_TYPE_VIDEO; });
  REQUIRE(first != restarted.frames.end());
  CHECK(first->media_time == 0);
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg local Seek interrupts the EOF loop duration wait") {
  const auto reference = DecodeReference(SamplePath("seek_tail_h264.mp4"));
  REQUIRE(reference.pts.size() == 1);
  REQUIRE(reference.end_time == 2);
  Observer observer;
  FfmpegInputConfig config;
  config.loop = true;
  FfmpegInput input(config);
  observer.Attach(input);
  std::vector<std::uint64_t> generations;
  input.SetOnPacket([&](std::uint64_t generation, const auto&) {
    generations.push_back(generation);
  });
  input.Start(SamplePath("seek_tail_h264.mp4"));
  REQUIRE(observer.Wait(
      [](const auto& value) { return value.frames.size() == 1; }));
  // The only frame has a two-second duration. With its callback returned and
  // no more data to decode, the media worker is waiting at the loop boundary.
  REQUIRE(observer.HasNoNewCallbacks(20ms));
  const auto requested = Clock::now();
  input.Seek(0ms);
  REQUIRE(observer.Wait(
      [](const auto& value) { return value.frames.size() >= 2; }, 1s));
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.valid);
  REQUIRE(value.frames.size() == 2);
  CHECK(generations == std::vector<std::uint64_t>{0, 1});
  CHECK(value.frames[1].pts == reference.pts.front());
  CHECK(value.frames[1].timestamp_ns == value.frames[0].timestamp_ns);
  CHECK(value.frames[1].duration_ns == 2000000000LL);
  CHECK(value.frames[1].received - requested < 400ms);
  CHECK(value.frames[1].thread == value.frames[0].thread);
  CHECK(value.ready == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 1);
  CHECK(StateCount(value, InputState::kConnected) == 1);
  CHECK(StateCount(value, InputState::kEnded) == 0);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg input loops complete local audio video and B frames") {
  const auto video = DecodeReference(SamplePath("h265_aac.mp4"));
  const auto audio =
      DecodeReference(SamplePath("h265_aac.mp4"), AVMEDIA_TYPE_AUDIO);
  REQUIRE(video.has_b_frames > 0);
  REQUIRE(video.drained_frames > 0);
  REQUIRE_FALSE(audio.pts.empty());
  const double media_begin = std::min(
      static_cast<double>(video.pts.front()) * av_q2d(video.time_base),
      static_cast<double>(audio.pts.front()) * av_q2d(audio.time_base));
  const double duration =
      std::max(video.end_time, audio.end_time) - media_begin;
  REQUIRE(duration >= 2.0);
  Observer observer;
  FfmpegInputConfig config;
  config.loop = true;
  config.max_retries = 0;
  FfmpegInput input(config);
  observer.TrackLoops();
  observer.SetReadyHook([] { std::this_thread::sleep_for(60ms); });
  observer.Attach(input);
  std::vector<std::uint64_t> generations;
  input.SetOnPacket([&](std::uint64_t generation, const auto&) {
    generations.push_back(generation);
  });
  input.Start(SamplePath("h265_aac.mp4"));
  REQUIRE(observer.Wait([](const auto& value) {
    return value.ready == 1 && !value.frames.empty() &&
           value.frames.back().generation == 3;
  }));
  input.Stop();
  const auto value = observer.snapshot();
  generations.erase(std::unique(generations.begin(), generations.end()),
                    generations.end());
  CHECK(generations == std::vector<std::uint64_t>{0, 1, 2});
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 1);
  CHECK(StateCount(value, InputState::kConnected) == 1);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  CHECK(StateCount(value, InputState::kEnded) == 0);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(StateCount(value, InputState::kStopped) == 1);
  REQUIRE(value.media_threads.size() == 3);
  CHECK(std::all_of(value.media_threads.begin(), value.media_threads.end(),
                    [&](const auto& thread) {
                      return thread == value.media_threads.front();
                    }));
  std::vector<Clock::time_point> beginnings;
  std::vector<int64_t> first_timestamps;
  for (size_t generation = 1; generation <= 3; ++generation) {
    CAPTURE(generation);
    std::vector<FrameObservation> frames;
    std::vector<int64_t> video_pts;
    std::vector<int64_t> audio_pts;
    for (const auto& frame : value.frames) {
      CHECK(frame.thread == value.media_threads.front());
      if (frame.generation != generation) continue;
      frames.push_back(frame);
      (frame.type == AVMEDIA_TYPE_VIDEO ? video_pts : audio_pts)
          .push_back(frame.pts);
    }
    REQUIRE_FALSE(frames.empty());
    beginnings.push_back(frames.front().received);
    first_timestamps.push_back(frames.front().timestamp_ns);
    CheckTimestampMapping(value, generation);
    if (generation == 3) continue;
    CHECK(video_pts == video.pts);
    CHECK(audio_pts == audio.pts);
    for (const auto& frame : frames) {
      const double elapsed = std::chrono::duration<double>(
                                 frame.received - frames.front().received)
                                 .count();
      CHECK(std::abs(elapsed - (frame.media_time - frames.front().media_time)) <
            0.4);
    }
  }
  for (size_t i = 1; i < beginnings.size(); ++i) {
    const double elapsed =
        std::chrono::duration<double>(beginnings[i] - beginnings[i - 1])
            .count();
    CHECK(elapsed >= duration - 0.02);
    CHECK(elapsed < duration + 0.8);
    const auto absolute_end_ns = static_cast<int64_t>(
        std::llround(std::max(video.end_time, audio.end_time) * 1000000000));
    CHECK(std::abs(first_timestamps[i] - first_timestamps[i - 1] -
                   absolute_end_ns) <= 2);
  }
  CHECK(input.state() == InputState::kStopped);
  CHECK(observer.RetainedFrameIsValid());
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg input delivers a leading track without waiting for its PTS") {
  const auto reference_video = DecodeReference(SamplePath("h264_aac.flv"));
  const auto reference_audio =
      DecodeReference(SamplePath("h264_aac.flv"), AVMEDIA_TYPE_AUDIO);
  HttpServer server(ServerState::Response::kMedia, 5000);
  Observer observer;
  FfmpegInput input(FfmpegInputConfig{});
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitState(InputState::kEnded));
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(server.requests() == 1);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  std::vector<FrameObservation> video;
  std::vector<FrameObservation> audio;
  for (const auto& frame : value.frames) {
    (frame.type == AVMEDIA_TYPE_VIDEO ? video : audio).push_back(frame);
  }
  REQUIRE(video.size() == reference_video.pts.size());
  REQUIRE(audio.size() == reference_audio.pts.size());
  CHECK(video.front().media_time - audio.front().media_time > 4.9);
  const auto first_frame_gap =
      std::chrono::duration<double>(video.front().received -
                                    audio.front().received)
          .count();
  CHECK(std::abs(first_frame_gap) < 0.4);
  CHECK(video.front().timestamp_ns - audio.front().timestamp_ns ==
        MediaPtsNs(value, video.front()) - MediaPtsNs(value, audio.front()));
  CheckTimestampMapping(value, 1);
  for (size_t i = 0; i < video.size(); ++i) {
    CHECK(video[i].pts == reference_video.pts[i] + 5000);
  }
  for (size_t i = 0; i < audio.size(); ++i) {
    CHECK(audio[i].pts == reference_audio.pts[i]);
  }
  input.Stop();
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE(
    "FFmpeg input keeps audio moving while waiting for a video keyframe") {
  const auto reference = DecodeReference(SamplePath("mpeg2_nonkey_aac.ts"));
  REQUIRE(reference.pts.size() == 79);
  REQUIRE(reference.key_frames == 0);
  HttpServer server(ServerState::Response::kStallAfterMedia, 0,
                    "mpeg2_nonkey_aac.ts");
  Observer observer;
  auto config = FfmpegInputConfig{};
  config.read_timeout = 5s;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.Wait(
      [](const auto& value) {
        return value.ready == 1 && value.frames.size() >= 4;
      },
      2s));
  const auto value = observer.snapshot();
  CHECK(value.valid);
  REQUIRE(value.streams.size() == 2);
  CHECK(input.state() == InputState::kConnected);
  CHECK(std::all_of(
      value.frames.begin(), value.frames.end(),
      [](const auto& frame) { return frame.type == AVMEDIA_TYPE_AUDIO; }));
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(server.requests() == 1);
  const auto stop_started = Clock::now();
  input.Stop();
  CHECK(Clock::now() - stop_started < 1s);
  CHECK(input.state() == InputState::kStopped);
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg input stops scheduled delivery and restarts") {
  Observer observer;
  FfmpegInput input(FfmpegInputConfig{});
  observer.Attach(input);
  input.Start(SamplePath());
  CHECK_THROWS_AS(input.Start(SamplePath()), std::logic_error);
  REQUIRE(observer.Wait(
      [](const auto& value) { return value.frames.size() >= 4; }));
  const auto start = Clock::now();
  input.Stop();
  CHECK(Clock::now() - start < 1s);
  CHECK(input.state() == InputState::kStopped);
  const auto stopped = observer.snapshot();
  CHECK(stopped.frames.size() < 100);
  CHECK(StateCount(stopped, InputState::kStopped) == 1);
  CHECK(observer.RetainedFrameIsValid());
  CHECK(observer.HasNoNewCallbacks());
  input.Stop();
  CHECK(observer.snapshot().callbacks == stopped.callbacks);
  input.Start(SamplePath("h265_video.mp4"));
  REQUIRE(observer.Wait([&](const auto& value) {
    return value.ready == 2 && value.frames.size() > stopped.frames.size();
  }));
  input.Stop();
  const auto restarted = observer.snapshot();
  CHECK(StateCount(restarted, InputState::kStopped) == 2);
  REQUIRE(restarted.streams.size() == 1);
  CHECK(restarted.streams.front().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_HEVC);
  CHECK(StateCount(restarted, InputState::kFailed) == 0);
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg input reports first failure and respects retry limits") {
  HttpServer server(ServerState::Response::kFailure);
  Observer observer;
  auto config = RetryConfig(2);
  SECTION("automatic reconnect disabled") { config.auto_reconnect = false; }
  SECTION("zero retries") { config.max_retries = 0; }
  SECTION("two retries") {}
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitState(InputState::kFailed));
  const auto value = observer.snapshot();
  const size_t retries =
      config.auto_reconnect ? static_cast<size_t>(config.max_retries) : 0;
  CHECK(value.ready == 0);
  CHECK(value.frames.empty());
  CHECK(value.error < 0);
  CHECK_FALSE(value.message.empty());
  CHECK(StateCount(value, InputState::kConnecting) == retries + 1);
  CHECK(StateCount(value, InputState::kWaitingRetry) == retries);
  CHECK(StateCount(value, InputState::kFailed) == 1);
  CHECK(server.requests() == retries + 1);
  CHECK(server.HasNoNewRequests(100ms));
  input.Stop();
}

TEST_CASE("FFmpeg input can cancel a blocked HTTP open") {
  HttpServer server(ServerState::Response::kStall);
  Observer observer;
  auto config = RetryConfig();
  config.open_timeout = 30s;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(server.WaitForRequest());
  const auto start = Clock::now();
  input.Stop();
  CHECK(Clock::now() - start < 2s);
  CHECK(input.state() == InputState::kStopped);
  CHECK(StateCount(observer.snapshot(), InputState::kWaitingRetry) == 0);
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE("FFmpeg input cancels retry waits and destruction is silent") {
  HttpServer server(ServerState::Response::kFailure);
  Observer observer;
  auto input = std::make_unique<FfmpegInput>(RetryConfig(-1, 30s));
  observer.Attach(*input);
  input->Start(server.url());
  REQUIRE(observer.WaitState(InputState::kWaitingRetry));
  const auto callbacks = observer.snapshot().callbacks;
  const auto start = Clock::now();
  SECTION("Stop notifies once") {
    input->Stop();
    CHECK(StateCount(observer.snapshot(), InputState::kStopped) == 1);
  }
  SECTION("destruction has no callbacks") {
    input.reset();
    CHECK(observer.snapshot().callbacks == callbacks);
  }
  CHECK(Clock::now() - start < 1s);
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.requests() == 1);
}

TEST_CASE("FFmpeg input reopens after two read errors and resets budget") {
  HttpServer server(ServerState::Response::kTruncatedMedia);
  Observer observer;
  FfmpegInput input(RetryConfig(1));
  observer.Attach(input);
  std::vector<std::uint64_t> generations;
  input.SetOnPacket([&](std::uint64_t generation, const auto&) {
    generations.push_back(generation);
  });
  input.Start(server.url());
  REQUIRE(observer.Wait(
      [](const auto& value) {
        return value.ready == 1 &&
               StateCount(value, InputState::kConnected) == 3 &&
               !value.frames.empty() && value.frames.back().generation == 3;
      },
      12s));
  input.Stop();
  const auto value = observer.snapshot();
  generations.erase(std::unique(generations.begin(), generations.end()),
                    generations.end());
  CHECK(generations == std::vector<std::uint64_t>{0, 1, 2});
  CHECK(value.valid);
  CHECK(value.streams.size() == 2);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 2);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(server.requests() == 3);
  for (size_t generation = 1; generation <= 3; ++generation) {
    const auto first = std::find_if(value.frames.begin(), value.frames.end(),
                                    [generation](const auto& frame) {
                                      return frame.generation == generation;
                                    });
    REQUIRE(first != value.frames.end());
    CHECK(std::abs(first->media_time - value.frames.front().media_time) <
          0.001);
  }
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE(
    "FFmpeg input delivers each attempt on Media and waits on Reconnect") {
  HttpServer server(ServerState::Response::kTruncatedMedia);
  const auto video = DecodeReference(SamplePath("h264_aac.flv"));
  const auto audio =
      DecodeReference(SamplePath("h264_aac.flv"), AVMEDIA_TYPE_AUDIO);
  REQUIRE_FALSE(video.pts.empty());
  REQUIRE_FALSE(audio.pts.empty());
  Observer observer;
  FfmpegInput input(RetryConfig(1, 1s));
  observer.Attach(input);
  input.Start(server.url());
  const bool disconnected_twice = observer.Wait([](const auto& value) {
    return StateCount(value, InputState::kWaitingRetry) >= 2 ||
           StateCount(value, InputState::kFailed) != 0;
  });
  const auto read_failure = observer.snapshot();
  CHECK(read_failure.error == AVERROR(EIO));
  CHECK(read_failure.message.find("av_read_frame") != std::string::npos);
  const auto stop_started = Clock::now();
  input.Stop();
  CHECK(Clock::now() - stop_started < 1s);
  const auto stopped = observer.snapshot();
  INFO(stopped.message);
  REQUIRE(disconnected_twice);
  REQUIRE(stopped.ready == 1);
  REQUIRE(StateCount(stopped, InputState::kWaitingRetry) == 2);
  REQUIRE(StateCount(stopped, InputState::kFailed) == 0);
  CHECK(StateCount(stopped, InputState::kEnded) == 0);
  CHECK(input.state() == InputState::kStopped);
  CHECK(stopped.valid);
  CHECK(server.requests() == 2);

  const auto threads_for_state = [](const Observation& value,
                                    InputState state) {
    std::vector<std::thread::id> threads;
    for (size_t i = 0; i < value.states.size(); ++i) {
      if (value.states[i] == state) threads.push_back(value.state_threads[i]);
    }
    return threads;
  };
  CHECK(threads_for_state(stopped, InputState::kConnecting) ==
        stopped.media_threads);
  CHECK(threads_for_state(stopped, InputState::kConnected) ==
        stopped.media_threads);
  const auto reconnect_threads =
      threads_for_state(stopped, InputState::kWaitingRetry);
  // Each Reconnect starts while the previous Media is alive. The next Media
  // starts while that Reconnect is alive. Different Media attempts may reuse
  // an OS thread ID, so only compare threads with overlapping lifetimes.
  for (size_t generation = 1; generation <= 2; ++generation) {
    CAPTURE(generation);
    const auto media_thread = stopped.media_threads[generation - 1];
    CHECK(media_thread != std::this_thread::get_id());
    CHECK(reconnect_threads[generation - 1] != media_thread);
    if (generation == 2) CHECK(reconnect_threads.front() != media_thread);
    std::vector<int64_t> video_pts;
    std::vector<int64_t> audio_pts;
    for (const auto& frame : stopped.frames) {
      if (frame.generation != generation) continue;
      CHECK(frame.thread == media_thread);
      (frame.type == AVMEDIA_TYPE_VIDEO ? video_pts : audio_pts)
          .push_back(frame.pts);
    }
    REQUIRE_FALSE(video_pts.empty());
    REQUIRE_FALSE(audio_pts.empty());
    REQUIRE(video_pts.size() <= video.pts.size());
    REQUIRE(audio_pts.size() <= audio.pts.size());
    CHECK(std::equal(video_pts.begin(), video_pts.end(), video.pts.begin()));
    CHECK(std::equal(audio_pts.begin(), audio_pts.end(), audio.pts.begin()));
    CheckTimestampMapping(stopped, generation);
  }

  const auto reconnect_first =
      std::find_if(stopped.frames.begin(), stopped.frames.end(),
                   [](const auto& frame) { return frame.generation == 2; });
  REQUIRE(reconnect_first != stopped.frames.end());
  const auto reconnect_gap =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          reconnect_first->received - stopped.frames.front().received)
          .count();
  const auto reconnect_timestamp_gap =
      reconnect_first->timestamp_ns - stopped.frames.front().timestamp_ns;
  CHECK(reconnect_timestamp_gap >= 1000000000);
  CHECK(std::abs(reconnect_timestamp_gap - reconnect_gap) < 100000000);

  // Observe longer than the retry delay: Stop must cancel the pending reopen,
  // with neither late delivery nor another HTTP request after it returns.
  CHECK(server.HasNoNewRequests(1100ms));
  CHECK(observer.snapshot().callbacks == stopped.callbacks);
  CHECK(observer.snapshot().frames.size() == stopped.frames.size());
  CHECK(observer.HasNoNewCallbacks());

  input.Start(server.url());
  const bool restarted = observer.Wait([](const auto& value) {
    if (StateCount(value, InputState::kFailed) != 0) return true;
    bool video_ready = false;
    bool audio_ready = false;
    for (const auto& frame : value.frames) {
      if (frame.generation != 3) continue;
      video_ready |= frame.type == AVMEDIA_TYPE_VIDEO;
      audio_ready |= frame.type == AVMEDIA_TYPE_AUDIO;
    }
    return video_ready && audio_ready;
  });
  input.Stop();
  const auto resumed = observer.snapshot();
  INFO(resumed.message);
  REQUIRE(restarted);
  REQUIRE(StateCount(resumed, InputState::kFailed) == 0);
  REQUIRE(resumed.ready == 2);
  CheckTimestampMapping(resumed, 3);
  CHECK(threads_for_state(resumed, InputState::kConnecting) ==
        resumed.media_threads);
  CHECK(threads_for_state(resumed, InputState::kConnected) ==
        resumed.media_threads);
  CHECK(StateCount(resumed, InputState::kStopped) == 2);
  CHECK(server.requests() == 3);
  CHECK(resumed.valid);
  for (const auto& frame : resumed.frames) {
    if (frame.generation == 3) CHECK(frame.thread == resumed.media_threads[2]);
  }
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE("FFmpeg network EOF ends or loops without error reconnect") {
  HttpServer server(ServerState::Response::kMedia);
  Observer observer;
  FfmpegInputConfig config;
  SECTION("loop disabled") {}
  SECTION("loop enabled") { config.loop = true; }
  FfmpegInput input(config);
  if (config.loop) observer.TrackLoops();
  observer.Attach(input);
  input.Start(server.url());
  if (config.loop) {
    REQUIRE(observer.Wait([](const auto& value) {
      return value.ready == 1 && !value.frames.empty() &&
             value.frames.back().generation == 2;
    }));
  } else {
    REQUIRE(observer.WaitState(InputState::kEnded));
  }
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 1);
  CHECK(StateCount(value, InputState::kConnected) == 1);
  CHECK(StateCount(value, InputState::kEnded) == (config.loop ? 0 : 1));
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(server.requests() == (config.loop ? 2 : 1));
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE("FFmpeg input missing file does not retry") {
  Observer observer;
  FfmpegInput input;
  observer.Attach(input);
  input.Start(SamplePath("missing-file.mp4"));
  REQUIRE(observer.WaitState(InputState::kFailed));
  CHECK(StateCount(observer.snapshot(), InputState::kConnecting) == 1);
  CHECK(StateCount(observer.snapshot(), InputState::kWaitingRetry) == 0);
  input.Stop();
}

TEST_CASE("FFmpeg local loop reopening recovers through the retry policy") {
  TemporaryMediaFile file;
  Observer observer;
  auto config = RetryConfig(2, 100ms);
  config.loop = true;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(file.path());
  REQUIRE(
      observer.Wait([](const auto& value) { return !value.frames.empty(); }));
  // The prepared two-second frame remains valid, while reopening now fails.
  file.Clear();
  REQUIRE(observer.WaitState(InputState::kWaitingRetry));
  const auto waiting = observer.snapshot();
  CHECK(waiting.ready == 1);
  CHECK(waiting.frames.size() == 1);
  file.Restore();
  REQUIRE(observer.Wait([](const auto& value) {
    return value.frames.size() >= 2 ||
           StateCount(value, InputState::kFailed) != 0;
  }));
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(value.frames.size() == 2);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 2);
  CHECK(StateCount(value, InputState::kConnected) == 2);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE(
    "FFmpeg network loop retries failed reopening without another Ready") {
  HttpServer server(ServerState::Response::kMedia);
  server.SetResponseSequence({
      {ServerState::Response::kMedia, "h264_aac.flv"},
      {ServerState::Response::kFailure, "h264_aac.flv"},
      {ServerState::Response::kMedia, "h264_aac.flv"},
  });
  Observer observer;
  auto config = RetryConfig(1);
  config.loop = true;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.Wait([](const auto& value) {
    return (!value.frames.empty() && value.frames.back().generation == 2) ||
           StateCount(value, InputState::kFailed) != 0;
  }));
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 2);
  CHECK(StateCount(value, InputState::kConnected) == 2);
  CHECK(StateCount(value, InputState::kFailed) == 0);
  CHECK(server.requests() == 3);
  CHECK(observer.HasNoNewCallbacks());
}

TEST_CASE("FFmpeg empty EOF loop stops at the configured retry budget") {
  HttpServer server(ServerState::Response::kMedia);
  server.SetResponseSequence({{ServerState::Response::kMedia, ""}});
  Observer observer;
  auto config = RetryConfig(1);
  config.loop = true;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitState(InputState::kFailed));
  const auto failed = observer.snapshot();
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.ready == 1);
  CHECK(value.frames.empty());
  CHECK(failed.error == AVERROR_EOF);
  CHECK(failed.message.find("未解码出音视频帧") != std::string::npos);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 2);
  CHECK(StateCount(value, InputState::kConnected) == 0);
  CHECK(server.requests() == 2);
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE(
    "FFmpeg loop configuration changes fail before delivering new media") {
  std::string first = "h264_video.mp4";
  std::string changed;
  SECTION("selected track count") { changed = "h264_aac.mp4"; }
  SECTION("video codec") { changed = "h265_video.mp4"; }
  SECTION("same codec with different dimensions") {
    first = "h265_video.mp4";
    changed = "h265_cuda.mp4";
  }
  HttpServer server(ServerState::Response::kMedia);
  server.SetResponseSequence({
      {ServerState::Response::kMedia, first},
      {ServerState::Response::kMedia, changed},
  });
  Observer observer;
  auto config = RetryConfig();
  config.loop = true;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitState(InputState::kFailed));
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 1);
  CHECK(StateCount(value, InputState::kConnected) == 1);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  CHECK(server.requests() == 2);
  CHECK(value.frames.size() == DecodeReference(SamplePath(first)).pts.size());
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE("FFmpeg read retry rejects changed tracks before delivering frames") {
  HttpServer server(ServerState::Response::kTruncatedMedia);
  server.SetResponseSequence({
      {ServerState::Response::kTruncatedMedia, "h264_aac.flv"},
      {ServerState::Response::kMedia, "h265_aac.mp4"},
  });
  const auto video = DecodeReference(SamplePath("h264_aac.flv"));
  const auto audio =
      DecodeReference(SamplePath("h264_aac.flv"), AVMEDIA_TYPE_AUDIO);
  Observer observer;
  FfmpegInput input(RetryConfig());
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitState(InputState::kFailed));
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 1);
  CHECK(StateCount(value, InputState::kConnecting) == 2);
  CHECK(StateCount(value, InputState::kConnected) == 1);
  CHECK(server.requests() == 2);
  const auto retry = std::find(value.states.begin(), value.states.end(),
                               InputState::kWaitingRetry);
  REQUIRE(retry != value.states.end());
  CHECK(std::find(retry, value.states.end(), InputState::kConnected) ==
        value.states.end());
  std::vector<int64_t> video_pts;
  std::vector<int64_t> audio_pts;
  for (const auto& frame : value.frames) {
    CHECK(frame.generation == 1);
    (frame.type == AVMEDIA_TYPE_VIDEO ? video_pts : audio_pts)
        .push_back(frame.pts);
  }
  REQUIRE_FALSE(video_pts.empty());
  REQUIRE_FALSE(audio_pts.empty());
  REQUIRE(video_pts.size() <= video.pts.size());
  REQUIRE(audio_pts.size() <= audio.pts.size());
  CHECK(std::equal(video_pts.begin(), video_pts.end(), video.pts.begin()));
  CHECK(std::equal(audio_pts.begin(), audio_pts.end(), audio.pts.begin()));
  CHECK(observer.HasNoNewCallbacks());
  CHECK(server.HasNoNewRequests(100ms));
}

TEST_CASE("FFmpeg input fails explicitly when Packet byte cap is exceeded") {
  Observer observer;
  auto config = FfmpegInputConfig{};
  config.max_packet_buffer_bytes = 1;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(SamplePath());
  REQUIRE(observer.WaitState(InputState::kFailed));
  const auto value = observer.snapshot();
  CHECK(value.error < 0);
  CHECK_FALSE(value.message.empty());
  CHECK(value.ready == 1);
  CHECK(value.frames.empty());
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  input.Stop();
}

TEST_CASE("FFmpeg packet-only input reconnects and checks the stream contract",
          "[packet]") {
  AVFormatContext* reference = nullptr;
  REQUIRE(avformat_open_input(&reference, SamplePath("h264_aac.flv").c_str(),
                              nullptr, nullptr) == 0);
  REQUIRE(avformat_find_stream_info(reference, nullptr) >= 0);
  std::size_t expected_packets = 0;
  ffmpeg::Packet packet;
  while (av_read_frame(reference, packet.get()) >= 0) {
    if (packet->size > 0) ++expected_packets;
    packet.Unref();
  }
  avformat_close_input(&reference);
  const auto first_cycle_packets = expected_packets;

  HttpServer server(ServerState::Response::kMedia);
  auto config = RetryConfig(1);
  config.mode = InputMode::kRemux;
  InputState terminal = InputState::kEnded;
  std::size_t expected_retries = 1;
  SECTION("read failure resumes packets without another Ready") {
    server.SetResponseSequence({
        {ServerState::Response::kTruncatedMedia, "h264_aac.flv"},
        {ServerState::Response::kMedia, "h264_aac.flv"},
    });
    expected_packets *= 2;
  }
  SECTION("changed loop tracks fail before delivering changed packets") {
    config.loop = true;
    server.SetResponseSequence({
        {ServerState::Response::kMedia, "h264_aac.flv"},
        {ServerState::Response::kMedia, "h265_aac.mp4"},
    });
    terminal = InputState::kFailed;
    expected_retries = 0;
  }
  Observer observer;
  std::atomic<std::size_t> packets{0};
  std::vector<std::uint64_t> generations;
  FfmpegInput input(config);
  observer.Attach(input);
  input.SetOnPacket([&](std::uint64_t generation, const auto&) {
    generations.push_back(generation);
    ++packets;
  });
  input.Start(server.url());
  REQUIRE(observer.WaitState(terminal));
  input.Stop();
  const auto value = observer.snapshot();
  CHECK(value.ready == 1);
  CHECK(value.frames.empty());
  CHECK(packets.load() == expected_packets);
  std::vector<std::uint64_t> expected_generations(expected_packets, 0);
  if (expected_retries) {
    std::fill(expected_generations.begin() +
                  static_cast<std::vector<std::uint64_t>::difference_type>(
                      first_cycle_packets),
              expected_generations.end(), 1);
  }
  CHECK(generations == expected_generations);
  CHECK(StateCount(value, InputState::kWaitingRetry) == expected_retries);
  CHECK(server.requests() == 2);
  CHECK(observer.HasNoNewCallbacks());
  const auto stopped_packets = packets.load();
  CHECK(server.HasNoNewRequests(50ms));
  CHECK(packets.load() == stopped_packets);
}

TEST_CASE("FFmpeg input validates configuration and URLs") {
  auto config = FfmpegInputConfig{};
  SECTION("retry count") { config.max_retries = -2; }
  SECTION("retry interval") { config.retry_interval = 0ms; }
  SECTION("open timeout") { config.open_timeout = 0ms; }
  SECTION("read timeout") { config.read_timeout = 0ms; }
  SECTION("packet bytes") { config.max_packet_buffer_bytes = 0; }
  CHECK_THROWS_AS(FfmpegInput(config), std::invalid_argument);
  FfmpegInput input;
  CHECK_THROWS_AS(input.Start(""), std::invalid_argument);
  CHECK_THROWS_AS(input.Start(" \t\n"), std::invalid_argument);
  CHECK(input.state() == InputState::kIdle);
}

TEST_CASE("FFmpeg input times out stalled reads after a successful open") {
  HttpServer server(ServerState::Response::kStallAfterMedia);
  Observer observer;
  auto config = RetryConfig();
  config.auto_reconnect = false;
  config.open_timeout = 5s;
  config.read_timeout = 300ms;
  FfmpegInput input(config);
  observer.Attach(input);
  input.Start(server.url());
  REQUIRE(observer.WaitState(InputState::kConnected));
  REQUIRE(observer.WaitState(InputState::kFailed));
  const auto value = observer.snapshot();
  CHECK(value.valid);
  CHECK(value.ready == 1);
  CHECK(value.frames.size() > 100);
  CHECK(value.error < 0);
  CHECK(value.message.find("av_read_frame") != std::string::npos);
  CHECK(StateCount(value, InputState::kWaitingRetry) == 0);
  input.Stop();
  CHECK(observer.HasNoNewCallbacks());
}

// Run explicitly with [cuda] on a machine with an NVIDIA GPU and driver.
TEST_CASE("CUDA Input Seek keeps GPU frames and the external device",
          "[.][input][ffmpeg][cuda]") {
  std::optional<ffmpeg::HwDeviceContext> device;
  device.emplace(ffmpeg::HwDeviceType::kCuda);
  const auto* native =
      reinterpret_cast<const AVHWDeviceContext*>(device->get()->data);
  std::mutex mutex;
  Observation observed;
  std::vector<ffmpeg::Frame> retained;
  FfmpegInput input(*device);
  device.reset();
  CallbackPause pause;
  pause.Arm(0);
  const auto gate = pause.hook();
  input.SetOnReady([&](const auto& streams) {
    std::lock_guard<std::mutex> lock(mutex);
    ++observed.ready;
    observed.streams = streams;
    observed.media_threads.push_back(std::this_thread::get_id());
  });
  input.SetOnStateChanged([&](InputState state, int, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex);
    observed.states.push_back(state);
    observed.message = message;
  });
  input.SetOnFrame([&, gate](int index, const auto& frame) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto stream =
          std::find_if(observed.streams.begin(), observed.streams.end(),
                       [index](const auto& information) {
                         return information.stream_index == index;
                       });
      if (stream == observed.streams.end()) {
        observed.valid = false;
      } else {
        const auto type = stream->codec_parameters.get()->codec_type;
        observed.frames.push_back(
            {index,
             static_cast<double>(frame->best_effort_timestamp) *
                 av_q2d(stream->time_base),
             Clock::now(), type, frame->best_effort_timestamp, observed.ready,
             std::this_thread::get_id(), frame->pts, frame->duration});
        observed.valid &= frame->pts != AV_NOPTS_VALUE &&
                          av_cmp_q(frame->time_base, kNanoseconds) == 0 &&
                          frame->pkt_dts == AV_NOPTS_VALUE;
        if (type == AVMEDIA_TYPE_VIDEO) {
          observed.valid &=
              frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx;
          if (frame->hw_frames_ctx) {
            const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
                frame->hw_frames_ctx->data);
            observed.valid &= pool->device_ctx == native;
          }
          retained.push_back(frame.Ref());
        } else {
          observed.valid &= frame->nb_samples > 0 && !frame->hw_frames_ctx;
        }
      }
    }
    gate(frame);
  });
  input.Start(SamplePath("seek_h264_aac.mp4"));
  REQUIRE(pause.Wait(1));
  size_t phase = 1;
  for (const auto position : {6500ms, 2500ms}) {
    const auto video = DecodeReference(SamplePath("seek_h264_aac.mp4"),
                                       AVMEDIA_TYPE_VIDEO, position);
    REQUIRE_FALSE(video.pts.empty());
    pause.Arm(video.pts.front());
    const auto requested = Clock::now();
    input.Seek(position);
    pause.Release();
    REQUIRE(pause.Wait(++phase, 1s));
    std::lock_guard<std::mutex> lock(mutex);
    const auto frame = std::find_if(
        observed.frames.rbegin(), observed.frames.rend(),
        [](const auto& value) { return value.type == AVMEDIA_TYPE_VIDEO; });
    REQUIRE(frame != observed.frames.rend());
    CHECK(frame->pts == video.pts.front());
    CHECK(frame->received - requested < 400ms);
  }
  pause.Release();
  input.Stop();
  std::lock_guard<std::mutex> lock(mutex);
  INFO(observed.message);
  CHECK(observed.valid);
  CheckTimestampMapping(observed, 1);
  CHECK(observed.ready == 1);
  CHECK(StateCount(observed, InputState::kConnecting) == 1);
  CHECK(StateCount(observed, InputState::kConnected) == 1);
  CHECK(StateCount(observed, InputState::kFailed) == 0);
  REQUIRE(retained.size() >= 3);
  for (const auto& frame : observed.frames) {
    CHECK(frame.thread == observed.media_threads.front());
  }
  for (const auto& frame : retained) {
    CHECK(av_cmp_q(frame->time_base, kNanoseconds) == 0);
    CHECK(frame->pkt_dts == AV_NOPTS_VALUE);
    ffmpeg::Frame downloaded;
    CHECK(av_hwframe_transfer_data(downloaded.get(), frame.get(), 0) == 0);
    CHECK(downloaded->width == frame->width);
    CHECK(downloaded->height == frame->height);
  }
}

TEST_CASE("CUDA Input shares its device across read retries and EOF loops",
          "[.][input][ffmpeg][cuda]") {
  bool loop = false;
  bool local = false;
  SECTION("read error reconnect") {}
  SECTION("network EOF loop") { loop = true; }
  SECTION("local EOF loop") {
    loop = true;
    local = true;
  }
  HttpServer server(loop ? ServerState::Response::kMedia
                         : ServerState::Response::kTruncatedMedia);
  std::optional<ffmpeg::HwDeviceContext> device;
  device.emplace(ffmpeg::HwDeviceType::kCuda);
  const auto* native =
      reinterpret_cast<const AVHWDeviceContext*>(device->get()->data);
  auto config = RetryConfig();
  config.loop = loop;
  config.video_decoder_name = "h264";
  config.audio_decoder_name = "aac";
  FfmpegInput input(*device, config);
  device.reset();

  std::mutex mutex;
  std::condition_variable changed;
  std::size_t generation = 0;
  std::size_t video_generation = 0;
  std::size_t audio_generation = 0;
  std::size_t callbacks = 0;
  std::size_t ready = 0;
  std::size_t retries = 0;
  std::size_t connects = 0;
  std::optional<int64_t> loop_offset;
  std::vector<std::thread::id> media_threads;
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Frame> retained;
  bool valid = true;
  bool failed = false;
  std::string error;
  input.SetOnReady([&](const auto& information) {
    std::lock_guard<std::mutex> lock(mutex);
    ++callbacks;
    ++ready;
    streams = information;
  });
  input.SetOnFrame([&](int index, const auto& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    ++callbacks;
    const auto stream = std::find_if(streams.begin(), streams.end(),
                                     [index](const auto& information) {
                                       return information.stream_index == index;
                                     });
    if (stream == streams.end()) {
      valid = false;
    } else {
      const auto offset =
          frame->pts - av_rescale_q(frame->best_effort_timestamp,
                                    stream->time_base, kNanoseconds);
      if (loop && loop_offset && *loop_offset != offset) {
        ++generation;
        media_threads.push_back(std::this_thread::get_id());
      }
      loop_offset = offset;
      if (stream->codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
        valid &= frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx;
        if (frame->hw_frames_ctx) {
          const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
              frame->hw_frames_ctx->data);
          valid &= pool->device_ctx == native;
        }
        if (video_generation != generation) {
          retained.push_back(frame.Ref());
          video_generation = generation;
        }
      } else {
        valid &= frame->nb_samples > 0 && !frame->hw_frames_ctx;
        audio_generation = generation;
      }
    }
    changed.notify_all();
  });
  input.SetOnStateChanged([&](InputState state, int, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex);
    ++callbacks;
    if (state == InputState::kConnected) {
      ++connects;
      ++generation;
      loop_offset.reset();
      media_threads.push_back(std::this_thread::get_id());
    }
    retries += state == InputState::kWaitingRetry;
    if (state == InputState::kFailed) {
      failed = true;
      error = message;
    }
    changed.notify_all();
  });
  input.Start(local ? SamplePath() : server.url());
  bool completed;
  {
    std::unique_lock<std::mutex> lock(mutex);
    completed = changed.wait_for(lock, 8s, [&] {
      return failed || (video_generation >= 2 && audio_generation >= 2);
    });
  }
  input.Stop();
  INFO(error);
  REQUIRE(completed);
  REQUIRE_FALSE(failed);
  CHECK(valid);
  CHECK(ready == 1);
  REQUIRE(retained.size() >= 2);
  if (loop) {
    CHECK(connects == 1);
    CHECK(retries == 0);
    CHECK(std::all_of(
        media_threads.begin(), media_threads.end(),
        [&](const auto& thread) { return thread == media_threads.front(); }));
  } else {
    CHECK(connects >= 2);
    CHECK(retries >= 1);
  }
  if (!local) CHECK(server.requests() >= 2);
  for (const auto& frame : retained) {
    ffmpeg::Frame downloaded;
    CHECK(av_hwframe_transfer_data(downloaded.get(), frame.get(), 0) == 0);
    CHECK(downloaded->width == frame->width);
    CHECK(downloaded->height == frame->height);
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    const auto stopped_callbacks = callbacks;
    CHECK_FALSE(changed.wait_for(
        lock, 150ms, [&] { return callbacks != stopped_callbacks; }));
  }
  CHECK(server.HasNoNewRequests(100ms));
}
