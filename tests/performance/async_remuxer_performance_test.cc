#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../encoder/encoder_test_support.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/async_remuxer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
constexpr AVRational kNanoseconds{1, 1000000000};
constexpr AVRational kMilliseconds{1, 1000};

class LoggedRuntime final {
 public:
  explicit LoggedRuntime(const std::string& level)
      : root_(fs::temp_directory_path()),
        directory_(
            root_ /
            ("mw-async-remux-performance-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        path_((directory_ / "performance.log").string()),
        modules_("perf.remux:" + level +
                 ";perf.encoder.video:off;perf.encoder.audio:off;streamer:off"),
        runtime_(nullptr, &mw::streamer::Shutdown) {
    fs::create_directories(directory_);
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    config.log.async_enabled = 0;
    config.log.rotating_file_enabled = 1;
    config.log.rotating_file_path = path_.c_str();
    config.log.rotating_file_path_size = path_.size();
    config.log.modules = modules_.c_str();
    config.log.modules_size = modules_.size();
    runtime_.reset(mw::streamer::Init(config));
  }
  ~LoggedRuntime() {
    runtime_.reset();
    if (directory_.parent_path() == root_) {
      std::error_code error;
      fs::remove_all(directory_, error);
    }
  }
  std::string ReadAfterShutdown() {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  }

 private:
  fs::path root_;
  fs::path directory_;
  std::string path_;
  std::string modules_;
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime_;
};

void EncodeFixture(encoder_test::Capture& capture) {
  mw::streamer::Encoder encoder;
  capture.Bind(encoder);
  auto config = encoder_test::Config();
  config.max_b_frames = 2;
  config.video_options["preset"] = "medium";
  config.video_options.erase("tune");
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=5:sync-lookahead=0";
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(
      config, {encoder_test::VideoStream(), encoder_test::AudioStream()}, cpu);
  REQUIRE(encoder.SubmitAudio(encoder_test::AudioFrame(3072)));
  for (int frame = 0; frame < 10; ++frame) {
    REQUIRE(encoder.SubmitVideo(encoder_test::VideoFrame(frame)));
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.callback_error.empty());
  REQUIRE(capture.streams.size() == 2);
  REQUIRE(capture.packets.size() == capture.dts_ns.size());
  REQUIRE(capture.packets.size() == 14);
}

struct Totals final {
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint64_t video = 0;
  std::uint64_t audio = 0;
  void Add(const ffmpeg::Packet& packet) {
    ++packets;
    bytes += static_cast<std::uint64_t>(packet->size);
    if (packet->stream_index == encoder_test::VideoStream().stream_index)
      ++video;
    else
      ++audio;
  }
};

std::vector<std::size_t> Order(const encoder_test::Capture& capture) {
  std::vector<std::size_t> order(capture.packets.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
    return capture.dts_ns[left] < capture.dts_ns[right];
  });
  return order;
}

struct PacketSnapshot final {
  std::int64_t pts;
  std::int64_t dts;
  std::int64_t duration;
  int stream_index;
  int flags;
  AVRational time_base;
  std::vector<std::uint8_t> payload;
};

std::vector<PacketSnapshot> Snapshot(const encoder_test::Capture& capture) {
  std::vector<PacketSnapshot> result;
  for (const auto& packet : capture.packets) {
    REQUIRE(packet->size > 0);
    result.push_back({packet->pts,
                      packet->dts,
                      packet->duration,
                      packet->stream_index,
                      packet->flags,
                      packet->time_base,
                      {packet->data, packet->data + packet->size}});
  }
  return result;
}

void CheckUnchanged(const encoder_test::Capture& capture,
                    const std::vector<PacketSnapshot>& snapshots) {
  REQUIRE(capture.packets.size() == snapshots.size());
  for (std::size_t index = 0; index < snapshots.size(); ++index) {
    const auto& packet = capture.packets[index];
    const auto& expected = snapshots[index];
    CHECK(packet->pts == expected.pts);
    CHECK(packet->dts == expected.dts);
    CHECK(packet->duration == expected.duration);
    CHECK(packet->stream_index == expected.stream_index);
    CHECK(packet->flags == expected.flags);
    CHECK(packet->time_base.num == expected.time_base.num);
    CHECK(packet->time_base.den == expected.time_base.den);
    CHECK(std::vector<std::uint8_t>(
              packet->data, packet->data + packet->size) == expected.payload);
  }
}

std::vector<std::string> Lines(const std::string& text,
                               const std::string& level = "",
                               const std::string& event = "") {
  std::istringstream input(text);
  std::vector<std::string> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.find("[perf.remux]") == std::string::npos) continue;
    if (!level.empty() && line.find("[" + level + "]") == std::string::npos)
      continue;
    if (!event.empty() && line.find(event) == std::string::npos) continue;
    result.push_back(line);
  }
  return result;
}

std::int64_t Integer(const std::string& line, const std::string& field) {
  INFO(line);
  INFO(field);
  std::smatch match;
  REQUIRE(std::regex_search(line, match,
                            std::regex("\\b" + field + "=(-?[0-9]+)")));
  return std::stoll(match[1].str());
}

double Number(const std::string& line, const std::string& field) {
  INFO(line);
  INFO(field);
  std::smatch match;
  REQUIRE(std::regex_search(
      line, match,
      std::regex("\\b" + field +
                 "=([+-]?[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)")));
  const auto value = std::stod(match[1].str());
  REQUIRE(std::isfinite(value));
  return value;
}

void CheckInfo(const std::string& line) {
  for (const auto* field :
       {"accepted_pps", "video_pps", "audio_pps", "payload_Mb_per_second",
        "pending_MiB", "queue_wait_mean_ms", "work_mean_ms",
        "interleave_mean_ms"}) {
    CHECK(Number(line, field) >= 0);
  }
  CHECK(line.find("state=") != std::string::npos);
  CHECK(line.find("accepted_packets=") == std::string::npos);
  CHECK(line.find("handed_off_packets=") == std::string::npos);
  CHECK(line.find("payload_bytes=") == std::string::npos);
}

void CheckSummary(const std::string& line, const Totals& accepted,
                  const Totals& handed_off, const Totals& discarded = {},
                  int errors = 0, int rejected = 0) {
  CHECK(line.find("counts=since_start") != std::string::npos);
  CHECK(Integer(line, "accepted_packets") ==
        static_cast<std::int64_t>(accepted.packets));
  CHECK(Integer(line, "accepted_bytes") ==
        static_cast<std::int64_t>(accepted.bytes));
  CHECK(Integer(line, "handed_off_packets") ==
        static_cast<std::int64_t>(handed_off.packets));
  CHECK(Integer(line, "payload_bytes") ==
        static_cast<std::int64_t>(handed_off.bytes));
  CHECK(Integer(line, "video_packets") ==
        static_cast<std::int64_t>(handed_off.video));
  CHECK(Integer(line, "audio_packets") ==
        static_cast<std::int64_t>(handed_off.audio));
  CHECK(Integer(line, "startup_discarded_packets") ==
        static_cast<std::int64_t>(discarded.packets));
  CHECK(Integer(line, "startup_discarded_bytes") ==
        static_cast<std::int64_t>(discarded.bytes));
  CHECK(Integer(line, "aborted_packets") == 0);
  CHECK(Integer(line, "aborted_bytes") == 0);
  CHECK(Integer(line, "rejected_packets") == rejected);
  CHECK(Integer(line, "errors") == errors);
  CHECK(Integer(line, "pending_packets") == 0);
  CHECK(Integer(line, "pending_bytes") == 0);
  CHECK(Integer(line, "queue_packets") == 0);
  CHECK(Integer(line, "queue_bytes") == 0);
  CHECK(Integer(line, "in_flight_packets") == 0);
  CHECK(Integer(line, "in_flight_bytes") == 0);
  CHECK(line.find("interleave_scope=push_pop_not_mutex_wait") !=
        std::string::npos);
  CHECK(Integer(line, "interleave_calls") >=
        static_cast<std::int64_t>(accepted.packets));
  for (const auto* field :
       {"interleave_total_ms", "interleave_mean_ms", "interleave_max_ms"}) {
    CHECK(Number(line, field) >= 0);
  }
}

void CheckHandoffs(const std::string& text,
                   const encoder_test::Capture& capture) {
  const auto events = Lines(text, "trace", "event=handoff");
  REQUIRE(events.size() == capture.packets.size());
  std::int64_t minimum = 0;
  for (const auto& packet : capture.packets) {
    minimum = std::min(
        {minimum, av_rescale_q(packet->pts, packet->time_base, kNanoseconds),
         av_rescale_q(packet->dts, packet->time_base, kNanoseconds)});
  }
  std::vector<bool> seen(capture.packets.size(), false);
  auto previous = std::numeric_limits<std::int64_t>::min();
  for (const auto& line : events) {
    const auto stream = Integer(line, "stream_index");
    const auto pts = Integer(line, "raw_pts");
    const auto dts = Integer(line, "raw_dts");
    const auto packet = std::find_if(
        capture.packets.begin(), capture.packets.end(), [&](const auto& value) {
          return value->stream_index == stream && value->pts == pts &&
                 value->dts == dts;
        });
    REQUIRE(packet != capture.packets.end());
    const auto index =
        static_cast<std::size_t>(packet - capture.packets.begin());
    REQUIRE_FALSE(seen[index]);
    seen[index] = true;
    const auto time_base = packet->get()->time_base;
    CHECK(line.find("time_base=" + std::to_string(time_base.num) + "/" +
                    std::to_string(time_base.den)) != std::string::npos);
    const auto media_pts = av_rescale_q(pts, time_base, kNanoseconds);
    const auto media_dts = av_rescale_q(dts, time_base, kNanoseconds);
    CHECK(Integer(line, "media_pts_ns") == media_pts);
    CHECK(Integer(line, "media_dts_ns") == media_dts);
    CHECK(Integer(line, "zlm_pts_ms") ==
          av_rescale_q(media_pts - minimum, kNanoseconds, kMilliseconds));
    CHECK(Integer(line, "zlm_dts_ms") ==
          av_rescale_q(media_dts - minimum, kNanoseconds, kMilliseconds));
    CHECK(media_dts >= previous);
    previous = media_dts;
    for (const auto* field :
         {"queue_wait_ms", "convert_ms", "mux_ms", "work_ms"}) {
      CHECK(Number(line, field) >= 0);
    }
  }
}

}  // namespace

TEST_CASE("公开AsyncRemuxer性能info trace off实际排空统计且日志不改变媒体包",
          "[performance][remuxer][async]") {
  const std::string level = GENERATE("info", "trace", "off");
  LoggedRuntime logging(level);
  encoder_test::Capture capture;
  EncodeFixture(capture);
  const auto snapshots = Snapshot(capture);
  const auto order = Order(capture);
  Totals totals;
  std::atomic<int> ended{0};
  std::atomic<int> errors{0};
  {
    mw::streamer::AsyncRemuxer remuxer;
    remuxer.SetOnEnded([&]() noexcept { ++ended; });
    remuxer.SetOnError(
        [&](std::string_view, int, std::string_view) noexcept { ++errors; });
    remuxer.Start(capture.streams);
    // There are deliberately no targets: a false SDK inputFrame return is
    // demand information, and must not be counted as a lost encoded packet.
    for (std::size_t position = 0; position < order.size(); ++position) {
      const auto index = order[position];
      REQUIRE(
          remuxer.SubmitPacket(capture.packets[index], capture.dts_ns[index]));
      totals.Add(capture.packets[index]);
      if (level == "info" && position == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2050));
      }
    }
    remuxer.Drain();
    remuxer.Stop();
    remuxer.Drain();
    remuxer.Stop();
  }
  CHECK(ended.load() == 1);
  CHECK(errors.load() == 0);
  CheckUnchanged(capture, snapshots);
  const auto text = logging.ReadAfterShutdown();
  if (level == "off") {
    CHECK(Lines(text).empty());
    return;
  }
  const auto summaries = Lines(text, "info", "report=summary");
  REQUIRE(summaries.size() == 1);
  CheckInfo(summaries.front());
  CHECK(Integer(summaries.front(), "errors") == 0);
  CHECK(Integer(summaries.front(), "pending_packets") == 0);
  CHECK(Integer(summaries.front(), "startup_discarded_total") == 0);
  if (level == "info") {
    REQUIRE_FALSE(Lines(text, "info", "report=interval").empty());
    CHECK(Lines(text, "trace").empty());
  } else {
    const auto details = Lines(text, "trace", "report=summary");
    REQUIRE(details.size() == 1);
    CheckSummary(details.front(), totals, totals);
    CHECK(Integer(details.front(), "peak_pending_packets") >= 1);
    CheckHandoffs(text, capture);
  }
}

TEST_CASE("AsyncRemuxer性能复用会话累计清零且重复Stop不产生多份摘要",
          "[performance][remuxer][async][lifecycle]") {
  LoggedRuntime logging("trace");
  encoder_test::Capture capture;
  EncodeFixture(capture);
  std::vector<Totals> totals(2);
  std::atomic<int> ended{0};
  std::atomic<int> errors{0};
  {
    mw::streamer::AsyncRemuxer remuxer;
    remuxer.SetOnEnded([&]() noexcept { ++ended; });
    remuxer.SetOnError(
        [&](std::string_view, int, std::string_view) noexcept { ++errors; });
    for (std::size_t session = 0; session < totals.size(); ++session) {
      remuxer.Start(capture.streams);
      for (const auto index : Order(capture)) {
        if (session == 1 && (capture.packets[index]->stream_index !=
                                 encoder_test::VideoStream().stream_index ||
                             totals[session].packets == 2))
          continue;
        REQUIRE(remuxer.SubmitPacket(capture.packets[index],
                                     capture.dts_ns[index]));
        totals[session].Add(capture.packets[index]);
      }
      remuxer.Drain();
      remuxer.Stop();
      remuxer.Stop();
      CHECK(ended.load() == static_cast<int>(session + 1));
    }
  }
  CHECK(errors.load() == 0);
  const auto text = logging.ReadAfterShutdown();
  CHECK(Lines(text, "info", "report=summary").size() == 2);
  const auto summaries = Lines(text, "trace", "report=summary");
  REQUIRE(summaries.size() == 2);
  CheckSummary(summaries[0], totals[0], totals[0]);
  CheckSummary(summaries[1], totals[1], totals[1]);
  CHECK(totals[1].packets == 2);
}

TEST_CASE("AsyncRemuxer性能区分EOF缺轨启动淘汰和非法包拒绝",
          "[performance][remuxer][async][accounting]") {
  LoggedRuntime logging("trace");
  encoder_test::Capture capture;
  EncodeFixture(capture);
  Totals accepted;
  Totals handed_off;
  Totals discarded;
  int expected_errors = 0;
  int expected_rejected = 0;
  std::atomic<int> ended{0};
  {
    mw::streamer::AsyncRemuxer remuxer;
    remuxer.SetOnEnded([&]() noexcept { ++ended; });
    remuxer.SetOnError([](std::string_view, int, std::string_view) noexcept {});
    remuxer.Start(capture.streams);
    SECTION("EOF仅收到AAC时全部有效包交给SDK") {
      for (const auto index : Order(capture)) {
        if (capture.packets[index]->stream_index !=
            encoder_test::AudioStream().stream_index)
          continue;
        REQUIRE(remuxer.SubmitPacket(capture.packets[index],
                                     capture.dts_ns[index]));
        accepted.Add(capture.packets[index]);
        handed_off.Add(capture.packets[index]);
      }
    }
    SECTION("EOF没有首个视频关键帧时计入启动淘汰") {
      for (const auto index : Order(capture)) {
        if (capture.packets[index]->stream_index !=
            encoder_test::VideoStream().stream_index)
          continue;
        auto packet = capture.packets[index].Clone();
        packet->flags &= ~AV_PKT_FLAG_KEY;
        REQUIRE(remuxer.SubmitPacket(packet, capture.dts_ns[index]));
        accepted.Add(packet);
        discarded.Add(packet);
      }
    }
    SECTION("非法包不被接受且不污染后续有效包统计") {
      auto invalid = capture.packets.front().Clone();
      invalid->stream_index = 99;
      REQUIRE_THROWS_AS(remuxer.SubmitPacket(invalid, capture.dts_ns.front()),
                        std::invalid_argument);
      expected_errors = 1;
      for (const auto index : Order(capture)) {
        REQUIRE(remuxer.SubmitPacket(capture.packets[index],
                                     capture.dts_ns[index]));
        accepted.Add(capture.packets[index]);
        handed_off.Add(capture.packets[index]);
      }
    }
    remuxer.Drain();
    remuxer.Stop();
    remuxer.Stop();
  }
  CHECK(ended.load() == 1);
  REQUIRE(accepted.packets > 0);
  const auto text = logging.ReadAfterShutdown();
  const auto info = Lines(text, "info", "report=summary");
  REQUIRE(info.size() == 1);
  CHECK(Integer(info.front(), "errors") == expected_errors);
  CHECK(Integer(info.front(), "startup_discarded_total") ==
        static_cast<std::int64_t>(discarded.packets));
  CHECK(Integer(info.front(), "pending_packets") == 0);
  const auto details = Lines(text, "trace", "report=summary");
  REQUIRE(details.size() == 1);
  CheckSummary(details.front(), accepted, handed_off, discarded,
               expected_errors, expected_rejected);
}

TEST_CASE("AsyncRemuxer转换失败将已取出的在途编码包计入中止而不冒充交付",
          "[performance][remuxer][async][failure]") {
  LoggedRuntime logging("trace");
  encoder_test::Capture capture;
  EncodeFixture(capture);
  const auto snapshots = Snapshot(capture);
  const auto stream = std::find_if(
      capture.streams.begin(), capture.streams.end(), [](const auto& value) {
        return value.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO;
      });
  REQUIRE(stream != capture.streams.end());
  const auto key = std::find_if(
      capture.packets.begin(), capture.packets.end(), [&](const auto& packet) {
        return packet->stream_index == stream->stream_index &&
               (packet->flags & AV_PKT_FLAG_KEY);
      });
  REQUIRE(key != capture.packets.end());
  const auto index = static_cast<std::size_t>(key - capture.packets.begin());
  auto damaged = key->Clone();
  ffmpeg::FfmpegException::throwIfError(av_packet_make_writable(damaged.get()),
                                        "独立损坏测试包的负载");
  REQUIRE(damaged->size > 0);
  std::fill_n(damaged->data, damaged->size, std::uint8_t{0});
  const auto bytes = static_cast<std::int64_t>(damaged->size);
  std::atomic<int> ended{0};
  std::atomic<int> errors{0};
  {
    mw::streamer::AsyncRemuxer remuxer;
    remuxer.SetOnEnded([&]() noexcept { ++ended; });
    remuxer.SetOnError(
        [&](std::string_view, int, std::string_view) noexcept { ++errors; });
    remuxer.Start({*stream});
    // Metadata passes admission. With one declared video track PopReady
    // removes this key packet before the adapter rejects its zeroed NAL data.
    // Counting only the remaining interleaver queue would lose this abort.
    REQUIRE(remuxer.SubmitPacket(damaged, capture.dts_ns[index]));
    remuxer.Drain();
    remuxer.Stop();
    remuxer.Stop();
  }
  CHECK(ended.load() == 1);
  CHECK(errors.load() >= 1);
  CheckUnchanged(capture, snapshots);
  const auto text = logging.ReadAfterShutdown();
  CHECK(Lines(text, "trace", "event=handoff").empty());
  const auto info = Lines(text, "info", "report=summary");
  REQUIRE(info.size() == 1);
  CHECK(Integer(info.front(), "errors") >= 1);
  CHECK(Integer(info.front(), "startup_discarded_total") == 0);
  CHECK(Integer(info.front(), "pending_packets") == 0);
  const auto details = Lines(text, "trace", "report=summary");
  REQUIRE(details.size() == 1);
  const auto& summary = details.front();
  CHECK(Integer(summary, "accepted_packets") == 1);
  CHECK(Integer(summary, "accepted_bytes") == bytes);
  CHECK(Integer(summary, "handed_off_packets") == 0);
  CHECK(Integer(summary, "payload_bytes") == 0);
  CHECK(Integer(summary, "startup_discarded_packets") == 0);
  CHECK(Integer(summary, "startup_discarded_bytes") == 0);
  CHECK(Integer(summary, "aborted_packets") == 1);
  CHECK(Integer(summary, "aborted_bytes") == bytes);
  CHECK(Integer(summary, "rejected_packets") == 0);
  CHECK(Integer(summary, "errors") >= 1);
  CHECK(Integer(summary, "pending_packets") == 0);
  CHECK(Integer(summary, "pending_bytes") == 0);
  CHECK(Integer(summary, "queue_packets") == 0);
  CHECK(Integer(summary, "queue_bytes") == 0);
  CHECK(Integer(summary, "in_flight_packets") == 0);
  CHECK(Integer(summary, "in_flight_bytes") == 0);
}
