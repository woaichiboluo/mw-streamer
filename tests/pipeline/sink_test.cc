#include "mw/streamer/sink/sink.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "mw/streamer/performance/pipeline_snapshot.h"

namespace {

using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using mw::streamer::SinkMessage;
using mw::streamer::NodeSnapshot;

class TopologySink final : public Sink {
 public:
  TopologySink(std::string name, SinkMediaType input, SinkMediaType output,
               int* destructions = nullptr)
      : Sink(name, input, output),
        name_(std::move(name)),
        destructions_(destructions) {}
  ~TopologySink() override {
    Stop();
    if (destructions_) {
      ++*destructions_;
    }
  }

  void Emit(const SinkMessage& message) { SendMessage(message); }

  void Activate() {
    CloseRegistration();
    StartMessages();
  }

 protected:
  NodeSnapshot GetOwnPerformance() const override {
    return {{}, name_, {}, {}};
  }

 private:
  std::string name_;
  int* destructions_;
};

}  // namespace

TEST_CASE("Sink构造必须提供非空且稳定的ID") {
  CHECK_THROWS_AS(TopologySink("", SinkMediaType::kFrame, SinkMediaType::kNone),
                  std::invalid_argument);
  TopologySink sink("processor", SinkMediaType::kFrame, SinkMediaType::kNone);
  CHECK(sink.id() == "processor");
}

TEST_CASE("Sink只通过注入的void函数发送消息") {
  TopologySink sink("sender", SinkMediaType::kFrame, SinkMediaType::kNone);
  int calls = 0;
  std::string type;
  sink.SetMessageSender([&](const SinkMessage& message) {
    ++calls;
    type = message.type;
  });
  sink.Emit({"sender", "before"});
  CHECK(calls == 1);
  CHECK(type == "before");
  sink.Activate();
  sink.Emit({"sender", "running"});
  CHECK(calls == 2);
  CHECK(type == "running");
  CHECK_THROWS(sink.SetMessageSender({}));
  sink.Stop();
  sink.Emit({"sender", "after"});
  CHECK(calls == 2);
}

TEST_CASE("Sink统一AddSink校验媒体连接并拒绝空节点") {
  TopologySink decoder("decoder", SinkMediaType::kPacket,
                       SinkMediaType::kFrame);
  CHECK_THROWS_AS(decoder.AddSink(nullptr), std::invalid_argument);
  CHECK_THROWS_AS(decoder.AddSink(std::make_unique<TopologySink>(
                      "packet", SinkMediaType::kPacket, SinkMediaType::kNone)),
                  std::invalid_argument);
  CHECK_NOTHROW(decoder.AddSink(std::make_unique<TopologySink>(
      "encoder", SinkMediaType::kFrame, SinkMediaType::kPacket)));
}

TEST_CASE("无输出Sink不允许添加下游") {
  TopologySink terminal("terminal", SinkMediaType::kPacket,
                        SinkMediaType::kNone);
  CHECK_THROWS(terminal.AddSink(std::make_unique<TopologySink>(
      "child", SinkMediaType::kPacket, SinkMediaType::kNone)));
}

TEST_CASE("Sink启动或停止后不允许修改拓扑") {
  TopologySink sink("parent", SinkMediaType::kFrame, SinkMediaType::kFrame);
  SECTION("启动后") { sink.Activate(); }
  SECTION("停止后") { sink.Stop(); }
  CHECK_THROWS(sink.AddSink(std::make_unique<TopologySink>(
      "child", SinkMediaType::kFrame, SinkMediaType::kNone)));
}

TEST_CASE("Sink基类递归收集多个分支统计并独占销毁整棵树") {
  int destructions = 0;
  NodeSnapshot snapshot;
  {
    TopologySink decoder("decoder", SinkMediaType::kPacket,
                         SinkMediaType::kFrame, &destructions);
    auto encoder =
        std::make_unique<TopologySink>("encoder", SinkMediaType::kFrame,
                                       SinkMediaType::kPacket, &destructions);
    encoder->AddSink(std::make_unique<TopologySink>(
        "record", SinkMediaType::kPacket, SinkMediaType::kNone, &destructions));
    encoder->AddSink(
        std::make_unique<TopologySink>("publish", SinkMediaType::kPacket,
                                       SinkMediaType::kNone, &destructions));
    decoder.AddSink(std::move(encoder));
    decoder.AddSink(
        std::make_unique<TopologySink>("analysis", SinkMediaType::kFrame,
                                       SinkMediaType::kNone, &destructions));
    snapshot = decoder.GetPerformance();
    CHECK(destructions == 0);
  }
  CHECK(destructions == 5);
  CHECK(snapshot.id == "decoder");
  CHECK(snapshot.name == "decoder");
  REQUIRE(snapshot.downstream.size() == 2);
  CHECK(snapshot.downstream[0].id == "encoder");
  CHECK(snapshot.downstream[0].name == "encoder");
  CHECK(snapshot.downstream[1].name == "analysis");
  REQUIRE(snapshot.downstream[0].downstream.size() == 2);
  CHECK(snapshot.downstream[0].downstream[0].id == "record");
  CHECK(snapshot.downstream[0].downstream[0].name == "record");
  CHECK(snapshot.downstream[0].downstream[1].name == "publish");
}
