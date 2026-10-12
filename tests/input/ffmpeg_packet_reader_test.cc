#include "mw/streamer/input/ffmpeg_packet_reader.h"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/error.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::FfmpegPacketReader;
constexpr int kThrow = std::numeric_limits<int>::max();

// Positive entries describe packet payload sizes; negative entries are native
// FFmpeg results. All observations use the same lock as the fake I/O operation.
class PacketSource {
 public:
  explicit PacketSource(std::vector<int> results, bool block_read = false)
      : results_(std::move(results)), block_read_(block_read) {}

  int Read(ffmpeg::Packet& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto index = calls_++;
    changed_.notify_all();
    if (block_read_) {
      changed_.wait(lock, [this] { return cancelled_; });
      return AVERROR_EXIT;
    }
    if (index >= results_.size()) return AVERROR_EOF;
    const int result = results_[index];
    if (result == kThrow) throw std::runtime_error("fake demux failure");
    if (result <= 0) return result;
    packet.Unref();
    ffmpeg::FfmpegException::throwIfError(av_new_packet(packet.get(), result),
                                          "分配 Packet 读取测试数据");
    packet->pts = static_cast<std::int64_t>(index + 1);
    packet->dts = packet->pts - 1;
    packet->duration = 5;
    packet->stream_index = static_cast<int>(index % 2);
    packet->flags = index % 3 == 0 ? AV_PKT_FLAG_KEY : 0;
    std::memset(packet->data, static_cast<int>((index + 1) % 256), result);
    return 0;
  }

  void Cancel() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    ++cancellations_;
    changed_.notify_all();
  }

  bool WaitCalls(std::size_t expected) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 2s,
                             [this, expected] { return calls_ >= expected; });
  }

  bool NoCallsBeyond(std::size_t expected) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !changed_.wait_for(lock, 50ms,
                              [this, expected] { return calls_ > expected; });
  }

  int cancellations() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancellations_;
  }

 private:
  const std::vector<int> results_;
  const bool block_read_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t calls_ = 0;
  int cancellations_ = 0;
  bool cancelled_ = false;
};

void CheckPacket(const ffmpeg::Packet& packet, std::size_t source_index,
                 int size) {
  CHECK(packet->pts == static_cast<std::int64_t>(source_index + 1));
  CHECK(packet->dts == static_cast<std::int64_t>(source_index));
  CHECK(packet->duration == 5);
  CHECK(packet->stream_index == static_cast<int>(source_index % 2));
  CHECK(packet->flags == (source_index % 3 == 0 ? AV_PKT_FLAG_KEY : 0));
  REQUIRE(packet->size == size);
  for (int index = 0; index < size; ++index) {
    CHECK(packet->data[index] ==
          static_cast<std::uint8_t>((source_index + 1) % 256));
  }
}

TEST_CASE("Packet 读取器跨 EAGAIN 保留原包顺序并排空 EOF",
          "[input][packet-reader]") {
  PacketSource source({AVERROR(EAGAIN), 4, AVERROR(EAGAIN), 3, 2, AVERROR_EOF});
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 32);
  ffmpeg::Packet packet;
  for (const auto [index, size] :
       std::vector<std::pair<std::size_t, int>>{{1, 4}, {3, 3}, {4, 2}}) {
    REQUIRE(reader.Pop(packet));
    CheckPacket(packet, index, size);
    reader.Release(static_cast<std::size_t>(packet->size));
  }
  CHECK_FALSE(reader.Pop(packet));
  CHECK(reader.outstanding_bytes() == 0);
  CHECK(reader.high_water_bytes() <= 32);
  reader.Stop();
  reader.Stop();
  CHECK(source.cancellations() == 1);
}

TEST_CASE("Packet 读取器暂时 ENOMEM 重试且成功读包重置计数",
          "[input][packet-reader][error][retry]") {
  std::vector<int> results{3};
  results.insert(results.end(), 19, AVERROR(ENOMEM));
  results.push_back(AVERROR(EAGAIN));
  const auto second = results.size();
  results.push_back(4);
  results.insert(results.end(), 19, AVERROR(ENOMEM));
  const auto third = results.size();
  results.push_back(2);
  results.push_back(AVERROR_EOF);
  PacketSource source(std::move(results));
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 32);
  ffmpeg::Packet packet;
  for (const auto [index, size] : std::vector<std::pair<std::size_t, int>>{
           {0, 3}, {second, 4}, {third, 2}}) {
    REQUIRE(reader.Pop(packet));
    CheckPacket(packet, index, size);
    reader.Release(static_cast<std::size_t>(packet->size));
  }
  CHECK_FALSE(reader.Pop(packet));
  CHECK(reader.finished());
  CHECK(reader.outstanding_bytes() == 0);
}

TEST_CASE("Packet 读取器二十次 ENOMEM 后先排空缓存再正常 EOF",
          "[input][packet-reader][error][retry]") {
  bool eagain = false;
  SECTION("连续内存不足") {}
  SECTION("EAGAIN 不重置内存不足计数") { eagain = true; }
  std::vector<int> results{3, 4};
  results.insert(results.end(), 19, AVERROR(ENOMEM));
  if (eagain) results.push_back(AVERROR(EAGAIN));
  results.push_back(AVERROR(ENOMEM));
  const auto expected_calls = results.size();
  results.push_back(5);
  PacketSource source(std::move(results));
  const auto started = std::chrono::steady_clock::now();
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 32);
  REQUIRE(source.WaitCalls(expected_calls));
  // Nineteen retry waits must not become a busy loop.
  CHECK(std::chrono::steady_clock::now() - started >= 80ms);
  CHECK(source.NoCallsBeyond(expected_calls));
  ffmpeg::Packet packet;
  for (std::size_t index = 0; index < 2; ++index) {
    REQUIRE(reader.Pop(packet));
    CheckPacket(packet, index, static_cast<int>(index + 3));
    reader.Release(static_cast<std::size_t>(packet->size));
  }
  CHECK_FALSE(reader.Pop(packet));
  CHECK(reader.finished());
  CHECK(reader.outstanding_bytes() == 0);
}

TEST_CASE("Packet 读取器 AVERROR_EXIT 排空缓存后正常结束",
          "[input][packet-reader][error]") {
  PacketSource source({3, 4, AVERROR_EXIT, 5});
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 32);
  REQUIRE(source.WaitCalls(3));
  CHECK(source.NoCallsBeyond(3));
  ffmpeg::Packet packet;
  for (std::size_t index = 0; index < 2; ++index) {
    REQUIRE(reader.Pop(packet));
    CheckPacket(packet, index, static_cast<int>(index + 3));
    reader.Release(static_cast<std::size_t>(packet->size));
  }
  CHECK_FALSE(reader.Pop(packet));
  CHECK(reader.finished());
  CHECK(reader.outstanding_bytes() == 0);
}

TEST_CASE("Packet 读取器 Stop 在 ENOMEM 返回后取消重试等待",
          "[input][packet-reader][stop][retry]") {
  std::mutex mutex;
  std::condition_variable changed;
  bool release = false;
  int calls = 0;
  int cancellations = 0;
  FfmpegPacketReader reader(
      [&](auto&) {
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return AVERROR(ENOMEM);
      },
      [&] {
        std::lock_guard<std::mutex> lock(mutex);
        ++cancellations;
        release = true;
        changed.notify_all();
      },
      32);
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(changed.wait_for(lock, 2s, [&] { return calls != 0; }));
  }
  // Cancel an in-flight read that returns ENOMEM. The retry wait must observe
  // Stop even when its notification arrived before entering that wait.
  const auto started = std::chrono::steady_clock::now();
  reader.Stop();
  CHECK(std::chrono::steady_clock::now() - started < 500ms);
  CHECK(calls == 1);
  CHECK(cancellations == 1);
  ffmpeg::Packet packet;
  CHECK_FALSE(reader.Pop(packet));
}

TEST_CASE("Packet 读取器先交付缓存包再传播 I/O 错误或异常",
          "[input][packet-reader][error]") {
  bool native = false;
  SECTION("原生 FFmpeg 失败保留错误码") { native = true; }
  SECTION("读取回调异常保留原异常") {}
  PacketSource source({3, 4, native ? AVERROR(EIO) : kThrow});
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 32);
  REQUIRE(source.WaitCalls(3));
  ffmpeg::Packet packet;
  for (std::size_t index = 0; index < 2; ++index) {
    REQUIRE(reader.Pop(packet));
    CheckPacket(packet, index, static_cast<int>(index + 3));
    reader.Release(static_cast<std::size_t>(packet->size));
  }
  if (native) {
    try {
      reader.Pop(packet);
      FAIL("排空缓存后必须传播读取失败");
    } catch (const ffmpeg::FfmpegException& error) {
      CHECK(error.error_code() == AVERROR(EIO));
    }
  } else {
    try {
      reader.Pop(packet);
      FAIL("排空缓存后必须传播读取异常");
    } catch (const std::runtime_error& error) {
      CHECK(std::string(error.what()) == "fake demux failure");
    }
  }
  CHECK(reader.outstanding_bytes() == 0);
}

TEST_CASE("Packet 读取器保留 256 个排队包加一个待推包且消费后继续读取",
          "[input][packet-reader][backpressure]") {
  static_assert(FfmpegPacketReader::kPacketCapacity == 256);
  PacketSource source(std::vector<int>(257, 1));
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 1024);
  REQUIRE(source.WaitCalls(257));
  CHECK(source.NoCallsBeyond(257));
  CHECK(reader.outstanding_bytes() == 256);
  CHECK(reader.high_water_bytes() == 256);
  ffmpeg::Packet packet;
  REQUIRE(reader.Pop(packet));
  CheckPacket(packet, 0, 1);
  reader.Release(1);
  REQUIRE(source.WaitCalls(258));
  for (std::size_t index = 1; index < 257; ++index) {
    REQUIRE(reader.Pop(packet));
    CheckPacket(packet, index, 1);
    reader.Release(1);
  }
  CHECK_FALSE(reader.Pop(packet));
  CHECK(reader.outstanding_bytes() == 0);
  CHECK(reader.high_water_bytes() <= 257);
}

TEST_CASE("Packet 读取器 Pop 后仍占用共享字节预算直到轨道 Release",
          "[input][packet-reader][backpressure]") {
  // Release followed immediately by Pop must not treat a stale worker wait
  // flag as a budget failure after enough byte capacity has become available.
  for (int iteration = 0; iteration < 32; ++iteration) {
    PacketSource source({4, 4, AVERROR_EOF});
    FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                              [&] { source.Cancel(); }, 4);
    ffmpeg::Packet first;
    REQUIRE(reader.Pop(first));
    REQUIRE(source.WaitCalls(2));
    CHECK(reader.outstanding_bytes() == 4);
    ffmpeg::Packet pending;
    // This observation proves the worker has entered its byte-capacity wait.
    CHECK_THROWS_AS(reader.Pop(pending), std::length_error);
    reader.Release(4);
    ffmpeg::Packet second;
    REQUIRE(reader.Pop(second));
    CheckPacket(first, 0, 4);
    CheckPacket(second, 1, 4);
    CHECK(reader.outstanding_bytes() == 4);
    CHECK(reader.high_water_bytes() == 4);
    reader.Release(4);
    CHECK_FALSE(reader.Pop(second));
    CHECK(reader.outstanding_bytes() == 0);
  }
}

TEST_CASE("Packet 读取器字节预算阻止跨轨道等待时显式失败",
          "[input][packet-reader][error][backpressure]") {
  PacketSource source({4, 4, AVERROR_EOF});
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 6);
  ffmpeg::Packet held_by_track;
  REQUIRE(reader.Pop(held_by_track));
  REQUIRE(source.WaitCalls(2));
  CHECK(reader.outstanding_bytes() == 4);
  ffmpeg::Packet next_track;
  CHECK_THROWS_AS(reader.Pop(next_track), std::length_error);
  CHECK(reader.outstanding_bytes() == 4);
  reader.Release(4);
  REQUIRE(reader.Pop(next_track));
  CheckPacket(next_track, 1, 4);
  reader.Release(4);
  CHECK_FALSE(reader.Pop(next_track));
}

TEST_CASE("异步供包将暂时占用的字节预算视为背压而非错误",
          "[input][packet-reader][backpressure]") {
  PacketSource source({4, 4, AVERROR_EOF});
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 4);
  ffmpeg::Packet first;
  REQUIRE(reader.Pop(first));
  REQUIRE(source.WaitCalls(2));
  REQUIRE(source.NoCallsBeyond(2));
  CHECK(reader.budget_blocked());
  CHECK_FALSE(reader.finished());
  ffmpeg::Packet pending;
  CHECK_FALSE(reader.TryPop(pending));
  reader.Release(4);
  REQUIRE(reader.Pop(pending));
  CheckPacket(pending, 1, 4);
  reader.Release(4);
  CHECK_FALSE(reader.Pop(pending));
  CHECK(reader.finished());
}

TEST_CASE("Packet 读取器拒绝单个超过总字节预算的包",
          "[input][packet-reader][error]") {
  PacketSource source({7, AVERROR_EOF});
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 6);
  ffmpeg::Packet packet;
  CHECK_THROWS_AS(reader.Pop(packet), std::length_error);
  CHECK(reader.outstanding_bytes() == 0);
}

TEST_CASE("Packet 读取器 Stop 解除满包队列和满字节预算等待",
          "[input][packet-reader][stop]") {
  bool bytes = false;
  SECTION("包队列达到固定容量") {}
  SECTION("字节预算被队列占满") { bytes = true; }
  PacketSource source(std::vector<int>(257, 1));
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, bytes ? 1 : 1024);
  REQUIRE(source.WaitCalls(bytes ? 2 : 257));
  reader.RequestStop();
  reader.Stop();
  reader.Stop();
  CHECK(source.cancellations() == 1);
  ffmpeg::Packet packet;
  CHECK_FALSE(reader.Pop(packet));
}

TEST_CASE("Packet 读取器 Stop 取消阻塞 I/O 且停止后不交付包",
          "[input][packet-reader][stop]") {
  PacketSource source({}, true);
  FfmpegPacketReader reader([&](auto& packet) { return source.Read(packet); },
                            [&] { source.Cancel(); }, 32);
  REQUIRE(source.WaitCalls(1));
  reader.Stop();
  reader.RequestStop();
  CHECK(source.cancellations() == 1);
  ffmpeg::Packet packet;
  CHECK_FALSE(reader.Pop(packet));
}

}  // namespace
