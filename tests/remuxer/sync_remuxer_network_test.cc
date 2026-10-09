#include <fmt/format.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Rtsp/RtspSession.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/remuxer/sync_remuxer.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kMicroseconds{1, 1000000};

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_;
};

struct FormatCloser final {
  void operator()(AVFormatContext* format) const {
    avformat_close_input(&format);
  }
};

struct Media final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
  std::int64_t first_dts_us = std::numeric_limits<std::int64_t>::max();
  std::int64_t end_us = std::numeric_limits<std::int64_t>::min();
};

Media ReadMedia(const std::string& filename) {
  const auto path =
      std::string(MW_STREAMER_SYNC_REMUXER_TEST_DATA_DIR) + "/" + filename;
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) == 0);
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  REQUIRE(avformat_find_stream_info(raw, nullptr) >= 0);
  Media result;
  for (unsigned int index = 0; index < raw->nb_streams; ++index) {
    const auto* stream = raw->streams[index];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
        stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
      continue;
    result.streams.push_back({static_cast<int>(index),
                              ffmpeg::CodecParameters(*stream->codecpar),
                              stream->time_base});
  }
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    REQUIRE(status >= 0);
    const auto stream = std::find_if(
        result.streams.begin(), result.streams.end(), [&](const auto& value) {
          return value.stream_index == packet->stream_index;
        });
    if (stream == result.streams.end()) continue;
    REQUIRE(packet->dts != AV_NOPTS_VALUE);
    REQUIRE(packet->pts != AV_NOPTS_VALUE);
    packet->time_base = stream->time_base;
    result.first_dts_us =
        std::min(result.first_dts_us,
                 av_rescale_q(packet->dts, packet->time_base, kMicroseconds));
    result.end_us = std::max(
        result.end_us,
        av_rescale_q(std::max(packet->pts, packet->dts) + packet->duration,
                     packet->time_base, kMicroseconds));
    result.packets.push_back(std::move(packet));
  }
  REQUIRE(result.streams.size() == 2);
  REQUIRE_FALSE(result.packets.empty());
  return result;
}

std::uint16_t ReservePort() {
  const auto poller = toolkit::EventPollerPool::Instance().getPoller();
  auto server = std::make_shared<toolkit::TcpServer>(poller);
  server->start<mediakit::RtspSession>(0, "127.0.0.1");
  const auto port = server->getPort();
  poller->sync([&] { server.reset(); });
  return port;
}

struct Capture final {
  std::mutex mutex;
  std::condition_variable changed;
  int video_index = -1, audio_index = -1;
  AVRational video_time_base{0, 1}, audio_time_base{0, 1};
  std::size_t videos = 0, audios = 0, ready_calls = 0;
  std::size_t untimed_packets = 0;
  std::uint64_t source_generation = 0;
  std::int64_t video_us = std::numeric_limits<std::int64_t>::min();
  std::int64_t audio_us = std::numeric_limits<std::int64_t>::min();
  bool formats_valid = false;
  std::string error;

  void Fail(std::string_view message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      error = message.empty() ? "输入或输出失败" : std::string(message);
    }
    changed.notify_all();
  }
  bool Wait(std::int64_t video_time = 0, std::int64_t audio_time = 0) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 8s,
                            [&] {
                              return (videos >= 5 && audios >= 5 &&
                                      video_us >= video_time &&
                                      audio_us >= audio_time) ||
                                     !error.empty();
                            }) &&
           error.empty() && formats_valid && ready_calls == 1;
  }
  std::string Diagnostics() {
    std::lock_guard<std::mutex> lock(mutex);
    return fmt::format(
        "video={} audio={} ready={} formats={} source_generation={} "
        "video_pts_us={} audio_pts_us={} untimed_packets={} error={}",
        videos, audios, ready_calls, formats_valid, source_generation, video_us,
        audio_us, untimed_packets, error);
  }
};

// Keep supplying live media while FFmpeg probes, and join before shutting down
// the reader or publisher. Waiting for readiness must not interrupt production.
class Replay final {
 public:
  Replay(mw::streamer::SyncRemuxer& remuxer, const Media& media,
         Capture& capture)
      : remuxer_(remuxer),
        media_(media),
        capture_(capture),
        thread_([this] { Run(); }) {}
  ~Replay() { Stop(); }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    try {
      for (std::uint64_t generation = 0;; ++generation) {
        {
          std::lock_guard<std::mutex> lock(capture_.mutex);
          if (!capture_.error.empty()) return;
          capture_.source_generation = generation;
        }
        const auto start = std::chrono::steady_clock::now();
        for (const auto& packet : media_.packets) {
          const auto dts_us =
              av_rescale_q(packet->dts, packet->time_base, kMicroseconds);
          {
            std::unique_lock<std::mutex> lock(mutex_);
            if (changed_.wait_until(lock,
                                    start + std::chrono::microseconds(
                                                dts_us - media_.first_dts_us),
                                    [&] { return stopped_; }))
              return;
          }
          const auto pts = packet->pts, dts = packet->dts;
          if (!remuxer_.SubmitPacket(generation, packet)) {
            capture_.Fail("原包提交被拒绝");
            return;
          }
          if (packet->pts != pts || packet->dts != dts) {
            capture_.Fail("原包时间戳被修改");
            return;
          }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (changed_.wait_until(
                lock,
                start + std::chrono::microseconds(media_.end_us -
                                                  media_.first_dts_us),
                [&] { return stopped_; }))
          return;
      }
    } catch (const std::exception& error) {
      capture_.Fail(error.what());
    }
  }

  mw::streamer::SyncRemuxer& remuxer_;
  const Media& media_;
  Capture& capture_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_ = false;
  std::thread thread_;
};

}  // namespace

TEST_CASE("原包同步录制器连续两代RTSP发布可接收H264或H265及AAC",
          "[remuxer][sync][network][rtsp]") {
  for (const auto codec : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC}) {
    CAPTURE(codec);
    Runtime runtime;
    const auto media =
        ReadMedia(codec == AV_CODEC_ID_H264 ? "h264_aac.mp4" : "h265_aac.mp4");
    Capture capture;
    mw::streamer::SyncRemuxer remuxer;
    remuxer.SetOnError([&](std::string_view, int, std::string_view message) {
      capture.Fail(message);
    });
    remuxer.Start(media.streams);
    const auto port = ReservePort();
    remuxer.AddRtspPublish("sync", "original", "127.0.0.1", port);
    mw::streamer::FfmpegInputConfig config;
    config.mode = mw::streamer::InputMode::kRemux;
    config.auto_reconnect = false;
    config.open_timeout = 8s;
    config.read_timeout = 3s;
    mw::streamer::FfmpegInput client(config);
    client.SetOnReady([&](const std::vector<ffmpeg::StreamInfo>& streams) {
      std::lock_guard<std::mutex> lock(capture.mutex);
      ++capture.ready_calls;
      for (const auto& stream : streams) {
        const auto& parameters = *stream.codec_parameters.get();
        if (parameters.codec_id == codec) {
          capture.video_index = stream.stream_index;
          capture.video_time_base = stream.time_base;
        }
        if (parameters.codec_id == AV_CODEC_ID_AAC) {
          capture.audio_index = stream.stream_index;
          capture.audio_time_base = stream.time_base;
        }
      }
      capture.formats_valid =
          capture.video_index >= 0 && capture.audio_index >= 0;
    });
    client.SetOnPacket([&](std::uint64_t, const ffmpeg::Packet& packet) {
      {
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (!packet->data || packet->size <= 0) return;
        if (packet->stream_index == capture.video_index) ++capture.videos;
        if (packet->stream_index == capture.audio_index) ++capture.audios;
        if (packet->pts == AV_NOPTS_VALUE) {
          ++capture.untimed_packets;
        } else {
          // Input packet timestamps use OnReady's stream time base;
          // av_read_frame may leave AVPacket::time_base unset.
          if (packet->stream_index == capture.video_index) {
            const auto pts_us = av_rescale_q(
                packet->pts, capture.video_time_base, kMicroseconds);
            capture.video_us = std::max(capture.video_us, pts_us);
          }
          if (packet->stream_index == capture.audio_index) {
            const auto pts_us = av_rescale_q(
                packet->pts, capture.audio_time_base, kMicroseconds);
            capture.audio_us = std::max(capture.audio_us, pts_us);
          }
        }
      }
      capture.changed.notify_all();
    });
    client.SetOnStateChanged(
        [&](mw::streamer::InputState state, int, std::string_view message) {
          if (state == mw::streamer::InputState::kFailed) capture.Fail(message);
        });
    client.Start("rtsp://127.0.0.1:" + std::to_string(port) + "/sync/original");
    Replay replay(remuxer, media, capture);
    const bool ready = capture.Wait();
    INFO(capture.Diagnostics());
    REQUIRE(ready);
    std::int64_t video_us, audio_us;
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      video_us = capture.video_us;
      audio_us = capture.audio_us;
    }
    // Receiving more than one full source cycle on each track proves this
    // established connection survives generation changes, after initial probe.
    const auto cycle_us = media.end_us - media.first_dts_us;
    const bool continued = capture.Wait(video_us + cycle_us + 100000,
                                        audio_us + cycle_us + 100000);
    INFO(capture.Diagnostics());
    REQUIRE(continued);
    replay.Stop();
    client.Stop();
    remuxer.Stop();
    INFO(capture.Diagnostics());
    CHECK(capture.error.empty());
  }
}
