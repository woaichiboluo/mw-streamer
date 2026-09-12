#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "mw/streamer/config/toml.h"
#include "mw/streamer/pipeline/pipeline_builder.h"

namespace {

using namespace std::chrono_literals;
using namespace mw::streamer;
using mw::streamer::LoadPipelineConfigFromToml;
using mw::streamer::ParsePipelineConfigFromToml;
using mw::streamer::SavePipelineConfigToToml;
using mw::streamer::SerializePipelineConfigToToml;

constexpr std::string_view kCompleteToml = R"toml(
[input]
type = "zlm"
url = "rtsp://127.0.0.1/live/camera"
downstream = ["raw", "decode"]
[input.player]
connect_timeout_ms = 3100
media_timeout_ms = 4200
local_bind_ip = "127.0.0.2"
[input.reconnect_policy]
max_retries = 7
min_delay_ms = 110
max_delay_ms = 1200
delay_step_ms = 70

[[sinks]]
id = "encode"
type = "encoder"
downstream = ["publish", "record"]
message_receiver = "transform"
frame_queue_capacity = 21
startup_packet_capacity = 22
[sinks.audio_encoder]
encoder_name = "aac"
[sinks.audio_encoder.properties]
b = "192k"
[sinks.video_encoder]
codec = "h265"
encoder_name = "hevc_nvenc"
frame_rate = { num = 30000, den = 1001 }
[sinks.video_encoder.properties]
preset = "p5"
"key.with.dot" = "value with \"quotes\""

[[sinks]]
id = "decode"
type = "decoder"
downstream = ["analysis", "transform"]
cache_duration_ms = 1500
audio_decode_queue_capacity = 31
video_decode_queue_capacity = 32
[sinks.audio_decoder]
decoder_name = "aac"
[sinks.video_decoder]
decoder_name = "h264"
backend = "software"
device_index = 2

[[sinks]]
id = "analysis"
type = "analysis_processor"
message_receiver = "transform"

[[sinks]]
id = "transform"
type = "transform_processor"
downstream = ["sync"]

[[sinks]]
id = "sync"
type = "synchronizer"
downstream = ["encode"]
frame_queue_capacity = 41
max_frame_lateness_ms = 42
standby_timeout_ms = 430
standby_image_path = "./images/standby.png"

[[sinks]]
id = "publish"
type = "remux"
target = "rtmp://127.0.0.1/live/processed"
packet_queue_capacity = 51
[sinks.zlm.pusher]
connect_timeout_ms = 5200
local_bind_ip = "127.0.0.3"
[sinks.zlm.muxer]
paced_sender_interval_ms = 6
[sinks.zlm.recording]
file_buffer_size = 131072
hls_segment_duration_ms = 1400

[[sinks]]
id = "raw"
type = "remux"
target = "./recordings/original.mp4"

[[sinks]]
id = "record"
type = "remux"
target = "./recordings/processed.mp4"
)toml";

template <typename Node>
Node& FindNode(PipelineConfig& config, const std::string& id) {
  for (const auto& node : config.sinks) {
    if (node && node->id == id) {
      auto* result = dynamic_cast<Node*>(node.get());
      REQUIRE(result != nullptr);
      return *result;
    }
  }
  throw std::logic_error("测试节点不存在");
}

PipelineConfig MakeRecordingPipelineConfig() {
  PipelineConfig config;
  config.input.options.url = "rtsp://127.0.0.1/live/camera";
  config.input.downstream = {"record"};
  auto record = std::make_unique<RemuxNodeConfig>("record");
  record->options.target = "./record.mp4";
  config.sinks.push_back(std::move(record));
  return config;
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("mw-unified-config-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(path_));
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, std::string_view content) {
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output);
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  REQUIRE(output);
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input), {}};
}

}  // namespace

TEST_CASE("统一配置完整参数双向转换并保持节点与连接顺序") {
  auto original = ParsePipelineConfigFromToml(kCompleteToml);
  const auto serialized = SerializePipelineConfigToToml(original);
  auto config = ParsePipelineConfigFromToml(serialized);
  CHECK(SerializePipelineConfigToToml(config) == serialized);
  CHECK(config.input.type == InputType::kZlm);
  CHECK(config.input.options.url == "rtsp://127.0.0.1/live/camera");
  CHECK(config.input.downstream == std::vector<std::string>{"raw", "decode"});
  CHECK(config.input.options.player.connect_timeout == 3100ms);
  CHECK(config.input.options.player.media_timeout == 4200ms);
  CHECK(config.input.options.player.local_bind_ip == "127.0.0.2");
  CHECK(config.input.options.reconnect_policy.max_retries == 7);
  CHECK(config.input.options.reconnect_policy.min_delay == 110ms);
  CHECK(config.input.options.reconnect_policy.max_delay == 1200ms);
  CHECK(config.input.options.reconnect_policy.delay_step == 70ms);
  REQUIRE(config.sinks.size() == 8);
  std::vector<std::string> ids;
  for (const auto& node : config.sinks) ids.push_back(node->id);
  CHECK(ids == std::vector<std::string>{"encode", "decode", "analysis",
                                        "transform", "sync", "publish", "raw",
                                        "record"});

  const auto& decoder = FindNode<DecoderNodeConfig>(config, "decode");
  CHECK(decoder.type() == SinkType::kDecoder);
  CHECK(decoder.downstream ==
        std::vector<std::string>{"analysis", "transform"});
  CHECK(decoder.message_receiver.empty());
  CHECK(decoder.options.cache_duration == 1500ms);
  CHECK(decoder.options.audio_decode_queue_capacity == 31);
  CHECK(decoder.options.video_decode_queue_capacity == 32);
  CHECK(decoder.options.audio_decoder.decoder_name == "aac");
  CHECK(decoder.options.video_decoder.decoder_name == "h264");
  CHECK(decoder.options.video_decoder.backend ==
        mw::streamer::VideoDecoderBackend::kSoftware);
  CHECK(decoder.options.video_decoder.device_index == 2);

  const auto& analysis =
      FindNode<AnalysisProcessorNodeConfig>(config, "analysis");
  CHECK(analysis.type() == SinkType::kAnalysisProcessor);
  CHECK(analysis.message_receiver == "transform");
  const auto& transform =
      FindNode<TransformProcessorNodeConfig>(config, "transform");
  CHECK(transform.type() == SinkType::kTransformProcessor);

  const auto& sync = FindNode<SynchronizerNodeConfig>(config, "sync").options;
  CHECK(sync.frame_queue_capacity == 41);
  CHECK(sync.max_frame_lateness == 42ms);
  CHECK(sync.standby_timeout == 430ms);
  CHECK(sync.standby_image_path == "./images/standby.png");
  const auto& encoder = FindNode<EncoderNodeConfig>(config, "encode");
  CHECK(encoder.message_receiver == "transform");
  CHECK(encoder.downstream == std::vector<std::string>{"publish", "record"});
  CHECK(encoder.options.frame_queue_capacity == 21);
  CHECK(encoder.options.startup_packet_capacity == 22);
  CHECK(encoder.options.audio_encoder.encoder_name == "aac");
  CHECK(encoder.options.audio_encoder.properties.at("b") == "192k");
  CHECK(encoder.options.video_encoder.codec == kMwStreamerCodecH265);
  CHECK(encoder.options.video_encoder.encoder_name == "hevc_nvenc");
  CHECK(encoder.options.video_encoder.frame_rate.num == 30000);
  CHECK(encoder.options.video_encoder.frame_rate.den == 1001);
  CHECK(encoder.options.video_encoder.properties.at("preset") == "p5");
  CHECK(encoder.options.video_encoder.properties.at("key.with.dot") ==
        "value with \"quotes\"");
  const auto& remux = FindNode<RemuxNodeConfig>(config, "publish").options;
  CHECK(remux.target == "rtmp://127.0.0.1/live/processed");
  CHECK(remux.packet_queue_capacity == 51);
  CHECK(remux.zlm.pusher.connect_timeout == 5200ms);
  CHECK(remux.zlm.pusher.local_bind_ip == "127.0.0.3");
  CHECK(remux.zlm.muxer.paced_sender_interval == 6ms);
  CHECK(remux.zlm.recording.file_buffer_size == 131072);
  CHECK(remux.zlm.recording.hls_segment_duration == 1400ms);
}

TEST_CASE("统一配置省略参数保持默认值且没有隐式消息连接") {
  auto config = ParsePipelineConfigFromToml(R"(
[input]
type = "zlm"
url = "./input.mp4"
downstream = ["decode"]
[[sinks]]
id = "decode"
type = "decoder"
downstream = ["analysis"]
[[sinks]]
id = "analysis"
type = "analysis_processor"
)");
  const auto& decoder = FindNode<DecoderNodeConfig>(config, "decode");
  CHECK(decoder.options.cache_duration == 0ms);
  CHECK(decoder.options.audio_decode_queue_capacity == 256);
  CHECK(decoder.options.video_decode_queue_capacity == 128);
  CHECK(decoder.options.video_decoder.backend ==
        mw::streamer::VideoDecoderBackend::kCuda);
  CHECK(config.input.options.player.connect_timeout == 10000ms);
  CHECK(config.input.options.reconnect_policy.max_retries == -1);
  const auto serialized = SerializePipelineConfigToToml(config);
  auto again = ParsePipelineConfigFromToml(serialized);
  for (const auto& node : again.sinks) CHECK(node->message_receiver.empty());
}

TEST_CASE("统一配置严格拒绝错误TOML结构") {
  const std::vector<std::string> invalid = {
      "",
      "[input",
      "input = 42",
      "sinks = 42",
      "[input]\ntype='other'\nurl='x'\ndownstream=[]",
      "[input]\ntype='zlm'\nurl=42\ndownstream=[]",
      "[input]\ntype='zlm'\nurl='x'\ndownstream=[1]",
      "[input]\ntype='zlm'\nurl='x'\ndownstream=['r']\n"
      "[[sinks]]\nid='r'\ntype='other'\ntarget='a.mp4'",
      "[input]\ntype='zlm'\nurl='x'\ndownstream=['r']\n"
      "[[sinks]]\nid='r'\ntype='remux'\ntarget='a.mp4'\nmessage_receiver=['r']",
  };
  for (const auto& text : invalid) {
    CAPTURE(text);
    CHECK_THROWS(ParsePipelineConfigFromToml(text));
  }
  auto valid = std::string(kCompleteToml);
  SECTION("Encoder属性必须为字符串") {
    const auto pos = valid.find("b = \"192k\"");
    REQUIRE(pos != std::string::npos);
    valid.replace(pos, std::string("b = \"192k\"").size(), "b = 192");
  }
  SECTION("无符号参数拒绝负数") {
    const auto pos = valid.find("frame_queue_capacity = 21");
    REQUIRE(pos != std::string::npos);
    valid.replace(pos, std::string("frame_queue_capacity = 21").size(),
                  "frame_queue_capacity = -1");
  }
  CHECK_THROWS(ParsePipelineConfigFromToml(valid));
}

TEST_CASE("统一配置忽略未知TOML字段") {
  auto config = ParsePipelineConfigFromToml(R"(
unknown = true

[input]
type = "zlm"
url = "rtsp://127.0.0.1/live/camera"
downstream = ["record"]
unused = "value"

[[sinks]]
id = "record"
type = "remux"
target = "./record.mp4"
unused = 1

[sinks.zlm.pusher]
unused = true
)");

  REQUIRE(config.sinks.size() == 1);
  CHECK(config.input.options.url == "rtsp://127.0.0.1/live/camera");
  CHECK(FindNode<RemuxNodeConfig>(config, "record").options.target ==
        "./record.mp4");
}

TEST_CASE("统一配置校验媒体树与消息引用") {
  auto config = ParsePipelineConfigFromToml(kCompleteToml);
  SECTION("重复ID") { config.sinks.back()->id = "raw"; }
  SECTION("空ID") { config.sinks.back()->id.clear(); }
  SECTION("空节点") { config.sinks.push_back(nullptr); }
  SECTION("缺失输入") { config.input.options.url.clear(); }
  SECTION("输入无下游") { config.input.downstream.clear(); }
  SECTION("缺失媒体节点") { config.input.downstream[0] = "missing"; }
  SECTION("缺失消息节点") { config.sinks[0]->message_receiver = "missing"; }
  SECTION("媒体类型不匹配") { config.input.downstream[0] = "analysis"; }
  SECTION("重复媒体连接") { config.input.downstream.push_back("raw"); }
  SECTION("多个媒体上游") {
    FindNode<EncoderNodeConfig>(config, "encode").downstream.push_back("raw");
  }
  SECTION("不可达节点") {
    auto extra = std::make_unique<RemuxNodeConfig>("extra");
    extra->options.target = "./extra.mp4";
    config.sinks.push_back(std::move(extra));
  }
  SECTION("终端不能有媒体下游") {
    FindNode<RemuxNodeConfig>(config, "raw").downstream = {"decode"};
  }
  SECTION("Decoder必须有下游") {
    FindNode<DecoderNodeConfig>(config, "decode").downstream.clear();
  }
  SECTION("Encoder必须有下游") {
    FindNode<EncoderNodeConfig>(config, "encode").downstream.clear();
  }
  SECTION("不可达的类型合法媒体环") {
    auto first = std::make_unique<TransformProcessorNodeConfig>("cycle_a");
    first->downstream = {"cycle_b"};
    auto second = std::make_unique<TransformProcessorNodeConfig>("cycle_b");
    second->downstream = {"cycle_a"};
    config.sinks.push_back(std::move(first));
    config.sinks.push_back(std::move(second));
  }
  CHECK_THROWS_AS(ValidatePipelineConfig(config), std::invalid_argument);
  CHECK_THROWS(SerializePipelineConfigToToml(config));
  CHECK_THROWS(BuildPipeline(config));
}

TEST_CASE("统一配置校验节点参数无需启动媒体资源") {
  auto config = ParsePipelineConfigFromToml(kCompleteToml);
  SECTION("Decoder缓存时长") {
    FindNode<DecoderNodeConfig>(config, "decode").options.cache_duration =
        500ms;
  }
  SECTION("Decoder队列") {
    FindNode<DecoderNodeConfig>(config, "decode")
        .options.audio_decode_queue_capacity = 0;
  }
  SECTION("视频设备") {
    auto& options =
        FindNode<DecoderNodeConfig>(config, "decode").options.video_decoder;
    options.backend = mw::streamer::VideoDecoderBackend::kCuda;
    options.device_index = -1;
  }
  SECTION("Encoder队列") {
    FindNode<EncoderNodeConfig>(config, "encode")
        .options.startup_packet_capacity = 0;
  }
  SECTION("Encoder帧率分母") {
    FindNode<EncoderNodeConfig>(config, "encode")
        .options.video_encoder.frame_rate.den = 0;
  }
  SECTION("Synchronizer延迟") {
    FindNode<SynchronizerNodeConfig>(config, "sync")
        .options.max_frame_lateness = -1ms;
  }
  SECTION("Remux目标") {
    FindNode<RemuxNodeConfig>(config, "raw").options.target.clear();
  }
  SECTION("Remux队列") {
    FindNode<RemuxNodeConfig>(config, "raw").options.packet_queue_capacity = 0;
  }
  SECTION("Input超时") { config.input.options.player.connect_timeout = 0ms; }
  SECTION("Reconnect重试次数") {
    config.input.options.reconnect_policy.max_retries = -2;
  }
  SECTION("Reconnect延迟顺序") {
    config.input.options.reconnect_policy.min_delay = 10s;
    config.input.options.reconnect_policy.max_delay = 1s;
  }
  CHECK_THROWS_AS(ValidatePipelineConfig(config), std::invalid_argument);
}

TEST_CASE("程序配置无需TOML即可构建且生命周期不借用配置") {
  std::unique_ptr<Pipeline> pipeline;
  {
    auto config = MakeRecordingPipelineConfig();
    config.sinks[0]->message_receiver = "record";
    pipeline = BuildPipeline(config);
  }
  REQUIRE(pipeline);
  const auto snapshot = pipeline->GetPerformance();
  REQUIRE(snapshot.sinks.size() == 1);
  CHECK(snapshot.sinks[0].id == "record");
  pipeline->Stop();
}

TEST_CASE("构建器按ID绑定Processor并保持媒体投递顺序") {
  const auto config = ParsePipelineConfigFromToml(kCompleteToml);
  ProcessorBindings bindings;
  bindings.analysis["analysis"] = {};
  bindings.transform["transform"] = {};
  SECTION("正确类型及无回调均可构建") {
    auto pipeline = BuildPipeline(config, bindings);
    const auto snapshot = pipeline->GetPerformance();
    REQUIRE(snapshot.sinks.size() == 2);
    CHECK(snapshot.sinks[0].id == "raw");
    CHECK(snapshot.sinks[1].id == "decode");
    REQUIRE(snapshot.sinks[1].downstream.size() == 2);
    CHECK(snapshot.sinks[1].downstream[0].id == "analysis");
    CHECK(snapshot.sinks[1].downstream[1].id == "transform");
    CHECK_NOTHROW(BuildPipeline(config));
  }
  SECTION("不存在的回调节点") {
    bindings.analysis["missing"] = {};
    CHECK_THROWS_AS(BuildPipeline(config, bindings), std::invalid_argument);
  }
  SECTION("错误的Processor回调类型") {
    bindings.transform["analysis"] = {};
    CHECK_THROWS_AS(BuildPipeline(config, bindings), std::invalid_argument);
  }
  SECTION("普通Sink不能绑定Processor回调") {
    bindings.analysis["raw"] = {};
    CHECK_THROWS_AS(BuildPipeline(config, bindings), std::invalid_argument);
  }
}

TEST_CASE("文件加载解析本地路径而字符串解析保留路径") {
  TemporaryDirectory directory;
  auto config = ParsePipelineConfigFromToml(kCompleteToml);
  config.input.options.url = "./input.mp4";
  const auto text = SerializePipelineConfigToToml(config);
  auto parsed = ParsePipelineConfigFromToml(text);
  CHECK(parsed.input.options.url == "./input.mp4");
  CHECK(FindNode<RemuxNodeConfig>(parsed, "raw").options.target ==
        "./recordings/original.mp4");
  const auto file = directory.path() / "pipeline.toml";
  Write(file, text);
  auto loaded = LoadPipelineConfigFromToml(file);
  CHECK(loaded.input.options.url == (directory.path() / "input.mp4").string());
  CHECK(FindNode<RemuxNodeConfig>(loaded, "raw").options.target ==
        (directory.path() / "recordings/original.mp4").lexically_normal().string());
  CHECK(FindNode<SynchronizerNodeConfig>(loaded, "sync")
            .options.standby_image_path ==
        (directory.path() / "images/standby.png").lexically_normal().string());
  CHECK(FindNode<RemuxNodeConfig>(loaded, "publish").options.target ==
        "rtmp://127.0.0.1/live/processed");
  loaded.input.options.url = "rtsp://127.0.0.1/live/camera";
  FindNode<SynchronizerNodeConfig>(loaded, "sync")
      .options.standby_image_path.clear();
  SavePipelineConfigToToml(loaded, file);
  auto reloaded = LoadPipelineConfigFromToml(file);
  CHECK(SerializePipelineConfigToToml(loaded) ==
        SerializePipelineConfigToToml(reloaded));
}

TEST_CASE("保存加载报告IO错误且无效配置不会覆盖原文件") {
  TemporaryDirectory directory;
  const auto file = directory.path() / "pipeline.toml";
  auto config = MakeRecordingPipelineConfig();
  config.input.options.url = (directory.path() / "input.mp4").string();
  FindNode<RemuxNodeConfig>(config, "record").options.target =
      (directory.path() / "record.mp4").string();
  SavePipelineConfigToToml(config, file);
  CHECK(SerializePipelineConfigToToml(LoadPipelineConfigFromToml(file)) ==
        SerializePipelineConfigToToml(config));
  const auto previous = Read(file);
  config.sinks[0]->id.clear();
  CHECK_THROWS(SavePipelineConfigToToml(config, file));
  CHECK(Read(file) == previous);
  config.sinks[0]->id = "record";
  CHECK_THROWS(LoadPipelineConfigFromToml(directory.path() / "missing.toml"));
  CHECK_THROWS(SavePipelineConfigToToml(config, directory.path()));
  CHECK_THROWS(SavePipelineConfigToToml(
      config, directory.path() / "missing" / "file.toml"));
}

TEST_CASE("统一Pipeline模板支持加载和双向转换") {
  const auto config = LoadPipelineConfigFromToml(
      std::filesystem::path(MW_UNIFIED_CONFIG_TEMPLATE_DIR) / "pipeline.toml");
  REQUIRE(config.sinks.size() == 7);
  CHECK(config.input.downstream ==
        std::vector<std::string>{"decoder", "original_recording"});
  const auto text = SerializePipelineConfigToToml(config);
  CHECK(SerializePipelineConfigToToml(ParsePipelineConfigFromToml(text)) ==
        text);
}

TEST_CASE("全速文件输入结构体构建及TOML双向转换") {
  PipelineConfig original;
  original.input.type = InputType::kFile;
  original.input.file.path = "./source.mp4";
  original.input.downstream = {"decode"};
  auto decoder = std::make_unique<DecoderNodeConfig>("decode");
  decoder->downstream = {"analyze"};
  original.sinks.push_back(std::move(decoder));
  original.sinks.push_back(
      std::make_unique<AnalysisProcessorNodeConfig>("analyze"));
  const auto text = SerializePipelineConfigToToml(original);
  auto parsed = ParsePipelineConfigFromToml(text);
  CHECK(parsed.input.type == InputType::kFile);
  CHECK(parsed.input.file.path == "./source.mp4");
  CHECK(parsed.input.options.url.empty());
  CHECK(parsed.input.downstream == std::vector<std::string>{"decode"});
  CHECK(SerializePipelineConfigToToml(parsed) == text);
  CHECK(text.find("reconnect_policy") == std::string::npos);
  CHECK(text.find("url =") == std::string::npos);
  auto pipeline = BuildPipeline(parsed);
  const auto snapshot = pipeline->GetPerformance();
  CHECK(snapshot.input.name == "FileInput");
  REQUIRE(snapshot.sinks.size() == 1);
  CHECK(snapshot.sinks[0].id == "decode");
  pipeline->Stop();

  TemporaryDirectory directory;
  const auto path = directory.path() / "file.toml";
  SavePipelineConfigToToml(original, path);
  const auto loaded = LoadPipelineConfigFromToml(path);
  CHECK(loaded.input.file.path == (directory.path() / "source.mp4").string());
  CHECK(loaded.input.options.url.empty());
  SavePipelineConfigToToml(loaded, path);
  CHECK(SerializePipelineConfigToToml(LoadPipelineConfigFromToml(path)) ==
        SerializePipelineConfigToToml(loaded));
}

TEST_CASE("全速文件输入拒绝无效路径并忽略实时配置") {
  for (const auto fields : {"", "path = ''", "path = 1"}) {
    const std::string text =
        "[input]\ntype = 'file'\ndownstream = ['record']\n" +
        std::string(fields) +
        "\n[[sinks]]\nid = 'record'\ntype = 'remux'\ntarget = 'out.mp4'\n";
    CAPTURE(text);
    CHECK_THROWS(ParsePipelineConfigFromToml(text));
  }

  for (const auto fields :
       {"path = 'a.mp4'\nurl = 'a.mp4'", "path = 'a.mp4'\n[input.player]",
        "path = 'a.mp4'\n[input.reconnect_policy]"}) {
    const std::string text =
        "[input]\ntype = 'file'\ndownstream = ['record']\n" +
        std::string(fields) +
        "\n[[sinks]]\nid = 'record'\ntype = 'remux'\ntarget = 'out.mp4'\n";
    CAPTURE(text);
    const auto parsed = ParsePipelineConfigFromToml(text);
    CHECK(parsed.input.file.path == "a.mp4");
    CHECK(parsed.input.options.url.empty());
  }

  auto config = ParsePipelineConfigFromToml(kCompleteToml);
  config.input.type = InputType::kFile;
  config.input.file.path = "a.mp4";
  CHECK_THROWS_AS(ValidatePipelineConfig(config), std::invalid_argument);
  FindNode<DecoderNodeConfig>(config, "decode").options.cache_duration = 0ms;
  CHECK_NOTHROW(ValidatePipelineConfig(config));
}
