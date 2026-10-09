#ifndef MW_STREAMER_TESTS_REMUXER_HLS_TEST_SUPPORT_H_
#define MW_STREAMER_TESTS_REMUXER_HLS_TEST_SUPPORT_H_

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "../encoder/encoder_test_support.h"
#include "Common/MediaSource.h"
#include "Common/config.h"
#include "Http/HttpRequester.h"
#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "Record/Recorder.h"
#include "Rtsp/RtspMediaSource.h"
#include "Util/mini.h"
#include "Util/util.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/async_remuxer.h"
#include "mw/streamer/remuxer/sync_remuxer.h"

namespace hls_test {

namespace ffmpeg = mw::streamer::ffmpeg;
using namespace std::chrono_literals;

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_;
};

inline std::string Name() {
  static std::atomic<unsigned> next{0};
  return "hls_publish_" + std::to_string(++next) + "_" +
         toolkit::makeRandStr(12);
}

inline std::uint16_t ReservePort() {
  const auto descriptor = toolkit::SockUtil::listen(0, "127.0.0.1");
  REQUIRE(descriptor >= 0);
  const auto port = toolkit::SockUtil::get_local_port(descriptor);
#if defined(_WIN32)
  toolkit::close(descriptor);
#else
  ::close(descriptor);
#endif
  REQUIRE(port != 0);
  return port;
}

struct Encoded final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
  std::vector<std::int64_t> clocks;
};

inline Encoded Encode() {
  encoder_test::Capture capture;
  mw::streamer::Encoder encoder;
  capture.Bind(encoder);
  auto config = encoder_test::Config();
  config.gop_size = 50;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(
      config, {encoder_test::VideoStream(), encoder_test::AudioStream()}, cpu);
  REQUIRE(encoder.SubmitAudio(encoder_test::AudioFrame(8 * 48000)));
  for (int index = 0; index < 400; ++index)
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
  Encoded result;
  result.streams = std::move(capture.streams);
  for (const auto index : order) {
    result.packets.push_back(std::move(capture.packets[index]));
    result.clocks.push_back(capture.dts_ns[index]);
  }
  return result;
}

template <typename Remuxer>
void Feed(Remuxer& remuxer, const Encoded& encoded) {
  for (std::size_t index = 0; index < encoded.packets.size(); ++index) {
    if constexpr (std::is_same_v<Remuxer, mw::streamer::AsyncRemuxer>) {
      REQUIRE(
          remuxer.SubmitPacket(encoded.packets[index], encoded.clocks[index]));
    } else {
      REQUIRE(remuxer.SubmitPacket(0, encoded.packets[index]));
    }
  }
}

struct Response final {
  std::string status;
  std::string body;
  std::string error;
};

inline Response Get(const std::string& url) {
  struct Pending final {
    std::mutex mutex;
    std::condition_variable changed;
    Response response;
    bool done = false;
  };
  const auto pending = std::make_shared<Pending>();
  auto requester = std::make_shared<mediakit::HttpRequester>();
  requester->getPoller()->sync([&] {
    requester->setMethod("GET");
    requester->startRequester(
        url,
        [pending](const toolkit::SockException& error,
                  const mediakit::Parser& parser) {
          {
            std::lock_guard lock(pending->mutex);
            pending->response.status = parser.status();
            pending->response.body = parser.content();
            if (error) pending->response.error = error.what();
            pending->done = true;
          }
          pending->changed.notify_all();
        },
        4);
  });
  std::unique_lock lock(pending->mutex);
  REQUIRE(pending->changed.wait_for(lock, 6s, [&] { return pending->done; }));
  INFO(pending->response.error);
  INFO(url);
  INFO(pending->response.status);
  INFO(pending->response.body);
  REQUIRE(pending->response.error.empty());
  return pending->response;
}

inline std::vector<std::string> Segments(const std::string& playlist) {
  std::vector<std::string> result;
  std::size_t offset = 0;
  while (offset < playlist.size()) {
    const auto end = playlist.find('\n', offset);
    auto line = playlist.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line.front() != '#') result.push_back(std::move(line));
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return result;
}

inline Response WaitPlaylist(const std::string& url) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  Response response;
  do {
    response = Get(url);
    if (response.status == "200" && Segments(response.body).size() == 3)
      return response;
    std::this_thread::sleep_for(20ms);
  } while (std::chrono::steady_clock::now() < deadline);
  INFO(response.status);
  INFO(response.body);
  FAIL("未收到三段 TS 的直播播放列表");
  return response;
}

inline std::filesystem::path PlaylistPath(const std::string& name) {
  const auto root =
      toolkit::mINI::Instance()[mediakit::Http::kRootPath].as<std::string>();
  return mediakit::Recorder::getRecordPath(mediakit::Recorder::type_hls,
                                           {DEFAULT_VHOST, "live", name}, root);
}

inline std::vector<std::filesystem::path> CaptureOutputs(
    const std::filesystem::path& playlist, const std::string& content) {
  REQUIRE(std::filesystem::is_regular_file(playlist));
  const auto segments = Segments(content);
  for (const auto& segment : segments) {
    REQUIRE(std::filesystem::is_regular_file(playlist.parent_path() / segment));
  }
  std::vector<std::filesystem::path> files;
  std::size_t open_segments = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(playlist.parent_path())) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() == ".m3u8") {
      files.push_back(entry.path());
    } else if (entry.path().extension() == ".ts") {
      files.push_back(entry.path());
      const auto relative = entry.path()
                                .lexically_relative(playlist.parent_path())
                                .generic_string();
      if (std::find(segments.begin(), segments.end(), relative) ==
          segments.end())
        ++open_segments;
    }
  }
  // Feeding eight seconds leaves a fourth, unlisted segment open alongside
  // the three sealed segments. Stop must remove both kinds immediately.
  REQUIRE(open_segments >= 1);
  return files;
}

inline void RequireNoHlsFiles(const std::filesystem::path& directory) {
  if (!std::filesystem::exists(directory)) return;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    CAPTURE(entry.path().string());
    REQUIRE(entry.path().extension() != ".m3u8");
    REQUIRE(entry.path().extension() != ".ts");
  }
}

inline void RequireRemoved(const std::vector<std::filesystem::path>& files,
                           const std::filesystem::path& playlist) {
  for (const auto& file : files) {
    CAPTURE(file.string());
    REQUIRE_FALSE(std::filesystem::exists(file));
  }
  RequireNoHlsFiles(playlist.parent_path());
}

struct DecodedTimes final {
  std::int64_t first_video = AV_NOPTS_VALUE, last_video = AV_NOPTS_VALUE;
  std::int64_t first_audio = AV_NOPTS_VALUE, last_audio = AV_NOPTS_VALUE;
};

inline DecodedTimes DecodeSegment(const std::string& bytes) {
  REQUIRE(bytes.size() >= 188);
  REQUIRE(static_cast<unsigned char>(bytes.front()) == 0x47);
  struct Temporary final {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                      (Name() + "_" + toolkit::makeRandStr(12));
    ~Temporary() { std::filesystem::remove_all(directory); }
  } temporary;
  REQUIRE(std::filesystem::create_directory(temporary.directory));
  const auto path = temporary.directory / "segment.ts";
  {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
  }
  struct FormatCloser final {
    void operator()(AVFormatContext* format) const {
      avformat_close_input(&format);
    }
  };
  AVFormatContext* raw = nullptr;
  const auto path_utf8 = path.u8string();
  REQUIRE(avformat_open_input(&raw, path_utf8.c_str(), nullptr, nullptr) == 0);
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  REQUIRE(avformat_find_stream_info(raw, nullptr) >= 0);
  std::vector<std::unique_ptr<ffmpeg::Decoder>> decoders(raw->nb_streams);
  std::size_t videos = 0, audios = 0;
  DecodedTimes times;
  for (unsigned index = 0; index < raw->nb_streams; ++index) {
    const auto* stream = raw->streams[index];
    ffmpeg::StreamInfo info{static_cast<int>(index),
                            ffmpeg::CodecParameters(*stream->codecpar),
                            stream->time_base};
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      REQUIRE(stream->codecpar->codec_id == AV_CODEC_ID_H264);
      decoders[index] = std::make_unique<ffmpeg::VideoDecoder>(info);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      REQUIRE(stream->codecpar->codec_id == AV_CODEC_ID_AAC);
      decoders[index] = std::make_unique<ffmpeg::AudioDecoder>(info);
    }
  }
  const auto receive = [&](std::size_t index) {
    ffmpeg::Frame frame;
    while (decoders[index]->ReceiveFrame(frame) ==
           ffmpeg::DecodeResult::kFrame) {
      REQUIRE(frame->pts != AV_NOPTS_VALUE);
      const auto pts = av_rescale_q(frame->pts, frame->time_base, {1, 1000000});
      const bool video =
          raw->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
      auto& first = video ? times.first_video : times.first_audio;
      auto& last = video ? times.last_video : times.last_audio;
      if (first == AV_NOPTS_VALUE) first = pts;
      if (last != AV_NOPTS_VALUE) REQUIRE(pts > last);
      last = pts;
      if (video)
        ++videos;
      else
        ++audios;
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const auto status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    REQUIRE(status >= 0);
    const auto index = static_cast<std::size_t>(packet->stream_index);
    if (!decoders[index]) continue;
    while (!decoders[index]->SendPacket(packet)) receive(index);
    receive(index);
  }
  for (std::size_t index = 0; index < decoders.size(); ++index) {
    if (!decoders[index]) continue;
    while (!decoders[index]->Drain()) receive(index);
    receive(index);
  }
  REQUIRE(videos >= 50);
  REQUIRE(audios >= 40);
  return times;
}

template <typename Remuxer>
std::filesystem::path ExerciseMedia() {
  Runtime runtime;
  const auto encoded = Encode();
  const auto name = Name();
  const auto port = ReservePort();
  Remuxer remuxer;
  remuxer.Start(encoded.streams);
  const auto url = remuxer.AddHlsPublish("live", name, "127.0.0.1", port);
  REQUIRE(url == "http://127.0.0.1:" + std::to_string(port) + "/live/" + name +
                     "/hls.m3u8");
  remuxer.AddRtspPublish("live", name, "127.0.0.1", ReservePort());
  Feed(remuxer, encoded);
  const auto playlist = WaitPlaylist(url);
  REQUIRE(playlist.body.find("#EXTM3U") == 0);
  REQUIRE(playlist.body.find("#EXT-X-ENDLIST") == std::string::npos);
  REQUIRE(playlist.body.find("#EXT-X-MAP") == std::string::npos);
  const auto segments = Segments(playlist.body);
  REQUIRE(segments.size() == 3);
  REQUIRE(segments.front().find(".ts") != std::string::npos);
  DecodedTimes previous;
  for (const auto& segment_name : segments) {
    const auto segment_url = url.substr(0, url.rfind('/') + 1) + segment_name;
    const auto segment = Get(segment_url);
    REQUIRE(segment.status == "200");
    const auto current = DecodeSegment(segment.body);
    if (previous.last_video != AV_NOPTS_VALUE) {
      REQUIRE(current.first_video > previous.last_video);
      REQUIRE(current.first_audio > previous.last_audio);
    }
    previous = current;
  }
  const auto rtsp = std::dynamic_pointer_cast<mediakit::RtspMediaSource>(
      mediakit::MediaSource::find(RTSP_SCHEMA, DEFAULT_VHOST, "live", name));
  REQUIRE(rtsp);
  REQUIRE(rtsp->getSdp().find("m=video") != std::string::npos);
  REQUIRE(rtsp->getSdp().find("m=audio") != std::string::npos);
  const auto playlist_path = PlaylistPath(name);
  const auto files = CaptureOutputs(playlist_path, playlist.body);
  remuxer.Stop();
  RequireRemoved(files, playlist_path);
  return playlist_path.parent_path();
}

template <typename Remuxer>
void ExerciseLifecycle() {
  Runtime runtime;
  const auto encoded = Encode();
  const auto first_name = Name(), second_name = Name();
  const auto port = ReservePort();
  Remuxer first, second, duplicate;
  first.Start(encoded.streams);
  second.Start(encoded.streams);
  duplicate.Start(encoded.streams);
  const auto first_url =
      first.AddHlsPublish("live", first_name, "127.0.0.1", port);
  const auto second_url =
      second.AddHlsPublish("live", second_name, "127.0.0.1", port);
  REQUIRE_THROWS(
      duplicate.AddHlsPublish("live", first_name, "127.0.0.1", ReservePort()));
  REQUIRE_THROWS(duplicate.AddHlsPublish("..", "bad", "127.0.0.1", port));
  REQUIRE_THROWS(duplicate.AddHlsPublish("", "bad", "127.0.0.1", port));
  REQUIRE_THROWS(duplicate.AddHlsPublish("live", "bad", "127.0.0.1", 0));
  Feed(first, encoded);
  Feed(second, encoded);
  const auto first_playlist = WaitPlaylist(first_url);
  const auto second_playlist = WaitPlaylist(second_url);
  const auto first_path = PlaylistPath(first_name);
  const auto second_path = PlaylistPath(second_name);
  const auto first_files = CaptureOutputs(first_path, first_playlist.body);
  const auto second_files = CaptureOutputs(second_path, second_playlist.body);
  first.Stop();
  RequireRemoved(first_files, first_path);
  REQUIRE_FALSE(mediakit::MediaSource::find(HLS_SCHEMA, DEFAULT_VHOST, "live",
                                            first_name));
  REQUIRE(mediakit::MediaSource::find(HLS_SCHEMA, DEFAULT_VHOST, "live",
                                      second_name));
  REQUIRE(Get(second_url).status == "200");
  for (const auto& file : second_files)
    REQUIRE(std::filesystem::is_regular_file(file));
  const auto second_segment = second_url.substr(0, second_url.rfind('/') + 1) +
                              Segments(second_playlist.body).front();
  REQUIRE(Get(second_segment).status == "200");
  first.Start(encoded.streams);
  REQUIRE(first.AddHlsPublish("live", first_name, "127.0.0.1", port) ==
          first_url);
  Feed(first, encoded);
  const auto restarted_playlist = WaitPlaylist(first_url);
  const auto restarted_files =
      CaptureOutputs(first_path, restarted_playlist.body);
  const auto restarted_segment = first_url.substr(0, first_url.rfind('/') + 1) +
                                 Segments(restarted_playlist.body).front();
  const auto response = Get(restarted_segment);
  REQUIRE(response.status == "200");
  DecodeSegment(response.body);
  first.Stop();
  RequireRemoved(restarted_files, first_path);
  second.Stop();
  RequireRemoved(second_files, second_path);
  REQUIRE_FALSE(mediakit::MediaSource::find(HLS_SCHEMA, DEFAULT_VHOST, "live",
                                            second_name));
  duplicate.Stop();
}

}  // namespace hls_test

#endif  // MW_STREAMER_TESTS_REMUXER_HLS_TEST_SUPPORT_H_
