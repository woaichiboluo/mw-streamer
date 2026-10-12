#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "tls_test_support.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "../encoder/encoder_test_support.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/pipeline/remux_pipeline.h"
#include "mw/streamer/remuxer/async_remuxer.h"
#include "mw/streamer/remuxer/sync_remuxer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using namespace std::chrono_literals;

mw::streamer::InitConfig Config(const std::string& certificate = {},
                                const std::string& password = {}) {
  mw::streamer::InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console_enabled = 0;
  config.tls.certificate_file = certificate;
  config.tls.certificate_password = password;
  return config;
}

class Runtime final {
 public:
  explicit Runtime(const mw::streamer::InitConfig& config)
      : context_(mw::streamer::Init(config)) {}
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_;
};

std::string Name() {
  static std::atomic<unsigned> next{0};
  return "secure_publish_" + std::to_string(++next);
}

struct Encoded final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
};

Encoded Encode() {
  encoder_test::Capture capture;
  mw::streamer::Encoder encoder;
  capture.Bind(encoder);
  auto config = encoder_test::Config();
  config.gop_size = 50;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(
      config, {encoder_test::VideoStream(), encoder_test::AudioStream()}, cpu);
  for (int index = 0; index < 400; ++index) {
    REQUIRE(encoder.SubmitFrame(encoder_test::Frame(
        encoder_test::VideoFrame(index),
        encoder_test::AudioFrame(960,
                                 encoder_test::kAudioStart + index * 960))));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.callback_error.empty());
  std::vector<std::size_t> order;
  for (std::size_t index = 0; index < capture.packets.size(); ++index)
    order.push_back(index);
  std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
    return av_compare_ts(capture.packets[left]->dts,
                         capture.packets[left]->time_base,
                         capture.packets[right]->dts,
                         capture.packets[right]->time_base) < 0;
  });
  Encoded result;
  result.streams = std::move(capture.streams);
  for (auto index : order) {
    result.packets.push_back(std::move(capture.packets[index]));
  }
  return result;
}

std::vector<std::string> Segments(const std::string& playlist) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (start < playlist.size()) {
    const auto end = playlist.find('\n', start);
    auto line = playlist.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line.front() != '#') segments.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return segments;
}

tls_test::Response Playlist(const std::string& url,
                            const tls_test::CertFixture& certificate) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  do {
    auto response = tls_test::VerifiedHttpGet(url, certificate.trusted_file());
    if (response.status == 200 && !Segments(response.body).empty())
      return response;
    std::this_thread::sleep_for(20ms);
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("HTTPS HLS playlist did not become ready");
}

void RequireDecodedTs(const std::string& bytes,
                      const tls_test::CertFixture& certificate,
                      std::size_t minimum_video_frames = 50) {
  REQUIRE(bytes.size() >= 188);
  REQUIRE(static_cast<unsigned char>(bytes.front()) == 0x47);
  const auto path =
      std::filesystem::u8path(certificate.trusted_file()).parent_path() /
      "download.ts";
  {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
  }
  struct FormatCloser {
    void operator()(AVFormatContext* format) const {
      avformat_close_input(&format);
    }
  };
  AVFormatContext* raw = nullptr;
  const auto filename = path.generic_u8string();
  REQUIRE(avformat_open_input(&raw, filename.c_str(), nullptr, nullptr) == 0);
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  REQUIRE(avformat_find_stream_info(raw, nullptr) >= 0);
  std::vector<std::unique_ptr<ffmpeg::Decoder>> decoders(raw->nb_streams);
  std::size_t video_frames = 0, audio_frames = 0;
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
      if (raw->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        ++video_frames;
      else
        ++audio_frames;
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const auto result = av_read_frame(raw, packet.get());
    if (result == AVERROR_EOF) break;
    REQUIRE(result >= 0);
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
  REQUIRE(video_frames >= minimum_video_frames);
  REQUIRE(audio_frames >= 40);
}

template <typename Remuxer>
void ExerciseHttps(const tls_test::CertFixture& certificate) {
  Runtime runtime(Config(certificate.combined_file()));
  const auto encoded = Encode();
  Remuxer secure, plain, conflict;
  secure.Start(encoded.streams);
  plain.Start(encoded.streams);
  conflict.Start(encoded.streams);
  const auto secure_port = tls_test::ReservePort();
  const auto plain_port = tls_test::ReservePort();
  const auto secure_url =
      secure.AddHlsPublish("live", Name(), "127.0.0.1", secure_port, true);
  const auto plain_url =
      plain.AddHlsPublish("live", Name(), "127.0.0.1", plain_port);
  REQUIRE(secure_url.find("https://") == 0);
  REQUIRE(plain_url.find("http://") == 0);
  REQUIRE_THROWS(
      conflict.AddHlsPublish("live", Name(), "127.0.0.1", secure_port, false));
  REQUIRE_THROWS(
      conflict.AddHlsPublish("live", Name(), "127.0.0.1", plain_port, true));
  for (std::size_t index = 0; index < encoded.packets.size(); ++index) {
    if constexpr (std::is_same_v<Remuxer, mw::streamer::AsyncRemuxer>) {
      REQUIRE(secure.SubmitPacket(encoded.packets[index]));
      REQUIRE(plain.SubmitPacket(encoded.packets[index]));
    } else {
      REQUIRE(secure.SubmitPacket(0, encoded.packets[index]));
      REQUIRE(plain.SubmitPacket(0, encoded.packets[index]));
    }
  }
  const auto playlist = Playlist(secure_url, certificate);
  REQUIRE(playlist.body.find("#EXTM3U") == 0);
  const auto segment_url = secure_url.substr(0, secure_url.rfind('/') + 1) +
                           Segments(playlist.body).front();
  const auto segment =
      tls_test::VerifiedHttpGet(segment_url, certificate.trusted_file());
  REQUIRE(segment.status == 200);
  RequireDecodedTs(segment.body, certificate);
  REQUIRE(tls_test::VerifiedHttpGet(secure_url, certificate.trusted_file(),
                                    "localhost")
              .status == 200);
  REQUIRE_THROWS(tls_test::VerifiedHttpGet(
      secure_url, certificate.trusted_file(), "wrong.example"));
  tls_test::CertFixture untrusted;
  REQUIRE_THROWS(
      tls_test::VerifiedHttpGet(secure_url, untrusted.trusted_file()));
  REQUIRE(Playlist(plain_url, certificate).body.find("#EXTM3U") == 0);
  secure.Stop();
  plain.Stop();
  conflict.Stop();
}

}  // namespace

TEST_CASE("HTTPS HLS实际校验证书并解码异步输出TS",
          "[remuxer][async][https][tls]") {
  tls_test::CertFixture certificate;
  ExerciseHttps<mw::streamer::AsyncRemuxer>(certificate);
}

TEST_CASE("HTTPS HLS实际校验证书并解码原包输出TS",
          "[remuxer][sync][https][tls]") {
  tls_test::CertFixture certificate;
  ExerciseHttps<mw::streamer::SyncRemuxer>(certificate);
}

TEST_CASE("TLS初始化拒绝缺少私钥并允许随后重新初始化", "[init][tls]") {
  tls_test::CertFixture certificate;
  REQUIRE_THROWS(mw::streamer::Init(Config(certificate.trusted_file())));
  REQUIRE_THROWS(mw::streamer::Init(
      Config(certificate.encrypted_file(), "wrong-password")));
  REQUIRE_THROWS(
      mw::streamer::Init(Config(certificate.pkcs12_file(), "wrong-password")));
  for (const auto& file :
       {certificate.combined_file(), certificate.encrypted_file(),
        certificate.pkcs12_file()}) {
    CAPTURE(file);
    Runtime runtime(Config(file, file == certificate.combined_file()
                                     ? ""
                                     : tls_test::CertFixture::kPassword));
  }
}

TEST_CASE("HTTPS发布要求本次初始化配置证书且Shutdown清除旧证书",
          "[init][remuxer][https][tls]") {
  tls_test::CertFixture certificate;
  {
    Runtime runtime(Config(certificate.combined_file()));
    const auto encoded = Encode();
    mw::streamer::SyncRemuxer remuxer;
    remuxer.Start(encoded.streams);
    REQUIRE(remuxer
                .AddHlsPublish("live", Name(), "127.0.0.1",
                               tls_test::ReservePort(), true)
                .find("https://") == 0);
    remuxer.Stop();
  }
  Runtime runtime(Config());
  const auto encoded = Encode();
  mw::streamer::SyncRemuxer remuxer;
  remuxer.Start(encoded.streams);
  REQUIRE_THROWS(remuxer.AddHlsPublish("live", Name(), "127.0.0.1",
                                       tls_test::ReservePort(), true));
  REQUIRE_THROWS(remuxer.AddRtspPublish("live", Name(), "127.0.0.1",
                                        tls_test::ReservePort(), true));
  REQUIRE(
      remuxer
          .AddHlsPublish("live", Name(), "127.0.0.1", tls_test::ReservePort())
          .find("http://") == 0);
  remuxer.Stop();
}

TEST_CASE("RemuxPipeline启动前添加HTTPS并停止本地循环发布",
          "[pipeline][https][tls]") {
  tls_test::CertFixture certificate;
  Runtime runtime(Config(certificate.combined_file()));
  mw::streamer::RemuxPipelineConfig config;
  config.input.loop = true;
  mw::streamer::RemuxPipeline pipeline(config);
  const auto planned_url = pipeline.AddHlsPublish(
      "tls", Name(), "127.0.0.1", tls_test::ReservePort(), true);
  pipeline.Start(std::string(MW_STREAMER_TLS_TEST_DATA_DIR) +
                 "/seek_h264_aac.mp4");
  const auto url = pipeline.ready_info().hls_url;
  REQUIRE(url == planned_url);
  REQUIRE(url.find("https://127.0.0.1:") == 0);
  tls_test::Response segment;
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  do {
    const auto playlist = Playlist(url, certificate);
    REQUIRE(playlist.body.find("#EXTM3U") == 0);
    const auto segment_url =
        url.substr(0, url.rfind('/') + 1) + Segments(playlist.body).back();
    segment =
        tls_test::VerifiedHttpGet(segment_url, certificate.trusted_file());
    // kRemux does not pace file reads. A rolling segment can be removed while
    // the client opens its request; fetch the current playlist again in that
    // case.
    if (segment.status == 200) break;
  } while (std::chrono::steady_clock::now() < deadline);
  REQUIRE(segment.status == 200);
  RequireDecodedTs(segment.body, certificate, 10);
  pipeline.Stop();
  REQUIRE(pipeline.state() == mw::streamer::PipelineState::kStopped);
  REQUIRE_THROWS(tls_test::VerifiedHttpGet(url, certificate.trusted_file()));
}

TEST_CASE("RemuxPipeline启动前添加RTSPS并实际交付音视频",
          "[pipeline][rtsps][tls]") {
  tls_test::CertFixture certificate;
  Runtime runtime(Config(certificate.combined_file()));
  std::mutex mutex;
  std::condition_variable changed;
  int video_index = -1, audio_index = -1;
  unsigned videos = 0, audios = 0;
  std::string error;
  mw::streamer::RemuxPipelineConfig config;
  config.input.loop = true;
  mw::streamer::RemuxPipeline pipeline(config);
  pipeline.AddRtspPublish("tls", Name(), "127.0.0.1", tls_test::ReservePort(),
                          true);
  pipeline.Start(std::string(MW_STREAMER_TLS_TEST_DATA_DIR) +
                 "/seek_h264_aac.mp4");
  const auto url = pipeline.ready_info().rtsp_url;
  REQUIRE(url.find("rtsps://127.0.0.1:") == 0);
  mw::streamer::FfmpegInputConfig client_config;
  client_config.mode = mw::streamer::InputMode::kRemux;
  client_config.auto_reconnect = false;
  client_config.open_timeout = 5s;
  client_config.read_timeout = 3s;
  mw::streamer::FfmpegInput client(client_config);
  client.SetOnReady([&](const std::vector<ffmpeg::StreamInfo>& streams) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& stream : streams) {
      if (stream.codec_parameters.get()->codec_id == AV_CODEC_ID_H264)
        video_index = stream.stream_index;
      if (stream.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC)
        audio_index = stream.stream_index;
    }
  });
  client.SetOnPacket([&](std::uint64_t, const ffmpeg::Packet& packet) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (packet->size > 0 && packet->data) {
        if (packet->stream_index == video_index) ++videos;
        if (packet->stream_index == audio_index) ++audios;
      }
    }
    changed.notify_all();
  });
  client.SetOnStateChanged(
      [&](mw::streamer::InputState state, int, std::string_view message) {
        if (state != mw::streamer::InputState::kFailed) return;
        {
          std::lock_guard<std::mutex> lock(mutex);
          error = message.empty() ? "RTSPS input failed" : std::string(message);
        }
        changed.notify_all();
      });
  client.Start(url);
  bool received = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    received = changed.wait_for(lock, 5s, [&] {
      return (videos >= 5 && audios >= 5) || !error.empty();
    });
  }
  client.Stop();
  pipeline.Stop();
  INFO(error);
  CAPTURE(videos, audios);
  REQUIRE(received);
  REQUIRE(error.empty());
  REQUIRE(videos >= 5);
  REQUIRE(audios >= 5);
  REQUIRE(pipeline.state() == mw::streamer::PipelineState::kStopped);
}
