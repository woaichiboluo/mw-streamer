#include <memory>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"
#include "Util/RingBuffer.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using Storage = toolkit::_RingStorage<int>;
using Entry = std::pair<bool, int>;

std::vector<Entry> Entries(const Storage& storage) {
  std::vector<Entry> entries;
  storage.getCache().for_each([&](const auto& gop) {
    gop.for_each([&](const Entry& entry) { entries.push_back(entry); });
  });
  return entries;
}

void WriteStartupAudio(Storage& storage) {
  for (int timestamp : {0, 21, 42, 64, 85}) {
    storage.write(timestamp, false);
  }
}

std::vector<Entry> FirstGop() {
  return {{false, 0},  {false, 21}, {false, 42},
          {false, 64}, {false, 85}, {true, 21}};
}

}  // namespace

TEST_CASE("推流缓存保留首个视频关键帧前到达的五个音频包", "[startup_ring]") {
  Storage storage(32, 1, true);
  WriteStartupAudio(storage);
  storage.write(21, true);
  CHECK(Entries(storage) == FirstGop());
  CHECK(storage.getCache().size() == 1);

  // Subsequent keyframes retain the normal single-GOP eviction policy.
  storage.write(106, false);
  storage.write(10021, true);
  CHECK(Entries(storage) == std::vector<Entry>{{true, 10021}});
}

TEST_CASE("未启用启动保留的缓存保持原有淘汰行为", "[startup_ring]") {
  Storage storage(32, 1);
  WriteStartupAudio(storage);
  storage.write(21, true);
  CHECK(Entries(storage) == std::vector<Entry>{{true, 21}});
}

TEST_CASE("缓存克隆保留启动策略和首次关键帧状态", "[startup_ring]") {
  Storage storage(32, 1, true);
  WriteStartupAudio(storage);
  auto before_key = storage.clone();
  before_key->write(21, true);
  CHECK(Entries(*before_key) == FirstGop());
  REQUIRE(Entries(storage).size() == 5);

  auto after_key = before_key->clone();
  after_key->write(10021, true);
  CHECK(Entries(*after_key) == std::vector<Entry>{{true, 10021}});
  CHECK(Entries(*before_key) == FirstGop());
}

TEST_CASE("启动音频和首个关键帧共同受现有缓存容量限制", "[startup_ring]") {
  Storage storage(32, 1, true);
  for (int index = 0; index < 32; ++index) {
    storage.write(index, false);
  }
  REQUIRE(Entries(storage).size() == 32);
  storage.write(32, true);
  CHECK(Entries(storage).empty());

  // Overflow invalidates the GOP; discarded content must not reappear when
  // the next usable keyframe arrives.
  storage.write(33, false);
  CHECK(Entries(storage).empty());
  storage.write(34, true);
  CHECK(Entries(storage) == std::vector<Entry>{{true, 34}});
}

TEST_CASE("清空缓存不会恢复已淘汰的启动音频", "[startup_ring]") {
  Storage storage(32, 1, true);
  WriteStartupAudio(storage);
  storage.clearCache();
  storage.write(106, false);
  storage.write(21, true);
  CHECK(Entries(storage) == std::vector<Entry>{{false, 106}, {true, 21}});

  storage.clearCache();
  storage.write(128, false);
  CHECK(Entries(storage).empty());
  storage.write(10021, true);
  CHECK(Entries(storage) == std::vector<Entry>{{true, 10021}});
}

TEST_CASE("延迟挂接的推流读者无遗漏无重复接收启动缓存和实时包",
          "[startup_ring]") {
  toolkit::EventPollerPool::setPoolSize(1);
  toolkit::EventPollerPool::enableCpuAffinity(false);
  auto poller = toolkit::EventPollerPool::Instance().getPoller();
  auto ring = std::make_shared<toolkit::RingBuffer<int>>(32, nullptr, 1, true);
  for (int packet : {0, 1, 2, 3, 4}) {
    ring->write(packet, false);
  }
  ring->write(5, true);

  std::vector<int> received;
  toolkit::RingBuffer<int>::RingReader::Ptr reader;
  poller->sync([&] {
    reader = ring->attach(poller);
    reader->setReadCB([&](const int& packet) { received.push_back(packet); });
  });
  ring->write(6, false);
  ring->write(7, true);
  ring->write(8, false);

  std::vector<int> late_received;
  poller->sync([&] {
    auto late_reader = ring->attach(poller);
    late_reader->setReadCB(
        [&](const int& packet) { late_received.push_back(packet); });
    reader.reset();
  });
  CHECK(received == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8});
  CHECK(late_received == std::vector<int>{7, 8});
  ring.reset();
  poller->sync([] {});
}
