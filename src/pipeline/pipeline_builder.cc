#include "mw/pipeline/pipeline_builder.h"

#include <fmt/format.h>

#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mw/decoder/decoder_sink.h"
#include "mw/encoder/encoder_sink.h"
#include "mw/input/file_input.h"
#include "mw/input/zlm_input.h"
#include "mw/output/internal/remux_output.h"
#include "mw/output/remux_sink.h"
#include "mw/processor/analysis_processor_sink.h"
#include "mw/processor/transform_processor_sink.h"
#include "mw/synchronizer/synchronizer_sink.h"
#include "mw/zlm/internal/config_validator.h"

namespace mw::streamer::pipeline {
namespace {

using NodeIndex = std::unordered_map<std::string, const SinkConfig*>;

template <typename Node>
const auto& Options(const SinkConfig& config) {
  const auto* node = dynamic_cast<const Node*>(&config);
  if (!node) {
    throw std::invalid_argument(
        fmt::format("Sink配置类型不匹配: {}", config.id));
  }
  return node->options;
}

void Require(bool valid, const SinkConfig& config, const char* reason) {
  if (!valid) {
    throw std::invalid_argument(fmt::format("Sink {}: {}", config.id, reason));
  }
}

struct MediaContract {
  sink::SinkMediaType input;
  sink::SinkMediaType output;
};

MediaContract ValidateNode(const SinkConfig& config) {
  using namespace std::chrono_literals;
  switch (config.type()) {
    case SinkType::kDecoder: {
      const auto& options = Options<DecoderNodeConfig>(config);
      Require(options.audio_decode_queue_capacity > 0 &&
                  options.video_decode_queue_capacity > 0,
              config, "解码队列容量必须大于0");
      Require(options.cache_duration == 0ms || (options.cache_duration >= 1s &&
                                                options.cache_duration <= 30s),
              config, "cache_duration_ms必须为0或1000到30000");
      Require(options.video_decoder.backend ==
                      decoder::VideoDecoderBackend::kSoftware ||
                  options.video_decoder.backend ==
                      decoder::VideoDecoderBackend::kCuda,
              config, "未知视频解码后端");
      Require(options.video_decoder.backend !=
                      decoder::VideoDecoderBackend::kCuda ||
                  options.video_decoder.device_index >= 0,
              config, "CUDA设备索引不能为负数");
      return {sink::SinkMediaType::kPacket, sink::SinkMediaType::kFrame};
    }
    case SinkType::kAnalysisProcessor:
      static_cast<void>(Options<AnalysisProcessorNodeConfig>(config));
      return {sink::SinkMediaType::kFrame, sink::SinkMediaType::kNone};
    case SinkType::kTransformProcessor:
      static_cast<void>(Options<TransformProcessorNodeConfig>(config));
      return {sink::SinkMediaType::kFrame, sink::SinkMediaType::kFrame};
    case SinkType::kSynchronizer: {
      const auto& options = Options<SynchronizerNodeConfig>(config);
      Require(options.frame_queue_capacity > 0 &&
                  options.max_frame_lateness >= 0ms &&
                  options.standby_timeout >= 0ms,
              config, "同步队列容量或等待时间无效");
      return {sink::SinkMediaType::kFrame, sink::SinkMediaType::kFrame};
    }
    case SinkType::kEncoder: {
      const auto& options = Options<EncoderNodeConfig>(config);
      Require(options.frame_queue_capacity > 0 &&
                  options.startup_packet_capacity > 0,
              config, "编码队列容量必须大于0");
      Require(options.video_encoder.codec == kMwStreamerCodecH264 ||
                  options.video_encoder.codec == kMwStreamerCodecH265,
              config, "视频编码仅支持h264或h265");
      Require(options.video_encoder.frame_rate.num >= 0 &&
                  options.video_encoder.frame_rate.den > 0,
              config, "视频编码帧率必须为非负有理数");
      return {sink::SinkMediaType::kFrame, sink::SinkMediaType::kPacket};
    }
    case SinkType::kRemux: {
      const auto& options = Options<RemuxNodeConfig>(config);
      Require(options.packet_queue_capacity > 0, config,
              "Remux队列容量必须大于0");
      output::internal::ValidateRemuxOutputConfig(
          {options.target, options.zlm});
      return {sink::SinkMediaType::kPacket, sink::SinkMediaType::kNone};
    }
  }
  throw std::invalid_argument(fmt::format("未知Sink配置类型: {}", config.id));
}

NodeIndex IndexNodes(const PipelineConfig& config) {
  NodeIndex index;
  for (const auto& node : config.sinks) {
    if (!node || node->id.empty()) {
      throw std::invalid_argument("Sink配置和ID不能为空");
    }
    if (!index.emplace(node->id, node.get()).second) {
      throw std::invalid_argument(fmt::format("Sink ID重复: {}", node->id));
    }
  }
  return index;
}

template <typename Callbacks>
void ValidateBindings(const std::map<std::string, Callbacks>& bindings,
                      SinkType expected, const NodeIndex& index) {
  for (const auto& entry : bindings) {
    const auto found = index.find(entry.first);
    if (found == index.end() || found->second->type() != expected) {
      throw std::invalid_argument(fmt::format(
          "Processor回调绑定的ID不存在或类型不匹配: {}", entry.first));
    }
  }
}

template <typename Callbacks>
Callbacks FindCallbacks(const std::map<std::string, Callbacks>& bindings,
                        const std::string& id) {
  const auto found = bindings.find(id);
  return found == bindings.end() ? Callbacks{} : found->second;
}

std::unique_ptr<sink::Sink> CreateSink(const SinkConfig& config,
                                       const ProcessorBindings& bindings) {
  switch (config.type()) {
    case SinkType::kDecoder:
      return std::make_unique<decoder::DecoderSink>(
          config.id, Options<DecoderNodeConfig>(config));
    case SinkType::kAnalysisProcessor:
      return std::make_unique<processor::AnalysisProcessorSink>(
          config.id, Options<AnalysisProcessorNodeConfig>(config),
          FindCallbacks(bindings.analysis, config.id));
    case SinkType::kTransformProcessor:
      return std::make_unique<processor::TransformProcessorSink>(
          config.id, Options<TransformProcessorNodeConfig>(config),
          FindCallbacks(bindings.transform, config.id));
    case SinkType::kSynchronizer:
      return std::make_unique<synchronizer::SynchronizerSink>(
          config.id, Options<SynchronizerNodeConfig>(config));
    case SinkType::kEncoder:
      return std::make_unique<encoder::EncoderSink>(
          config.id, Options<EncoderNodeConfig>(config));
    case SinkType::kRemux:
      return std::make_unique<output::RemuxSink>(
          config.id, Options<RemuxNodeConfig>(config));
  }
  throw std::invalid_argument("未知Sink配置类型");
}

std::unique_ptr<sink::Sink> BuildSink(const SinkConfig& config,
                                      const NodeIndex& index,
                                      const ProcessorBindings& bindings) {
  auto sink = CreateSink(config, bindings);
  for (const auto& id : config.downstream) {
    sink->AddSink(BuildSink(*index.at(id), index, bindings));
  }
  return sink;
}

void ValidateInput(const InputConfig& input) {
  if (input.downstream.empty()) {
    throw std::invalid_argument("Input至少需要一个下游Sink");
  }
  if (input.type == InputType::kFile) {
    if (input.file.path.empty()) {
      throw std::invalid_argument("FileInput路径不能为空");
    }
    return;
  }
  if (input.type != InputType::kZlm || input.options.url.empty()) {
    throw std::invalid_argument("Input类型无效或url为空");
  }
  zlm::internal::ValidatePlayerConfig(input.options.player);
  const auto& reconnect = input.options.reconnect_policy;
  if (reconnect.max_retries < -1 || reconnect.min_delay.count() <= 0 ||
      reconnect.max_delay < reconnect.min_delay ||
      reconnect.delay_step.count() <= 0) {
    throw std::invalid_argument("Input重连策略参数无效");
  }
}

void ValidateEdges(const std::vector<std::string>& downstream,
                   sink::SinkMediaType output, const std::string& parent,
                   const std::unordered_map<std::string, MediaContract>& media,
                   std::unordered_set<std::string>& parented) {
  if (output == sink::SinkMediaType::kNone && !downstream.empty()) {
    throw std::invalid_argument(
        fmt::format("终端Sink不能配置媒体下游: {}", parent));
  }
  if (output != sink::SinkMediaType::kNone && downstream.empty()) {
    throw std::invalid_argument(
        fmt::format("节点至少需要一个媒体下游: {}", parent));
  }
  for (const auto& id : downstream) {
    const auto child = media.find(id);
    if (child == media.end()) {
      throw std::invalid_argument(
          fmt::format("节点 {}引用不存在的下游: {}", parent, id));
    }
    if (child->second.input != output) {
      throw std::invalid_argument(
          fmt::format("媒体类型不匹配: {} -> {}", parent, id));
    }
    if (!parented.insert(id).second) {
      throw std::invalid_argument(
          fmt::format("Sink只能有一个媒体上游: {}", id));
    }
  }
}

void ValidateReachability(const PipelineConfig& config,
                          const NodeIndex& index) {
  std::vector<std::string> pending = config.input.downstream;
  std::unordered_set<std::string> visited;
  while (!pending.empty()) {
    auto id = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(id).second) {
      throw std::invalid_argument(fmt::format("媒体连接存在环路: {}", id));
    }
    const auto& children = index.at(id)->downstream;
    pending.insert(pending.end(), children.begin(), children.end());
  }
  for (const auto& node : config.sinks) {
    if (!visited.count(node->id)) {
      throw std::invalid_argument(
          fmt::format("Sink未连接到Input或存在媒体环路: {}", node->id));
    }
  }
}

}  // namespace

void ValidatePipelineConfig(const PipelineConfig& config) {
  ValidateInput(config.input);
  const auto index = IndexNodes(config);
  std::unordered_map<std::string, MediaContract> media;
  for (const auto& node : config.sinks) {
    media.emplace(node->id, ValidateNode(*node));
    if (config.input.type == InputType::kFile) {
      const auto* decoder = dynamic_cast<const DecoderNodeConfig*>(node.get());
      if (decoder && decoder->options.cache_duration.count() != 0) {
        throw std::invalid_argument("离线文件解码不能配置延迟缓存");
      }
    }
    if (!node->message_receiver.empty() &&
        !index.count(node->message_receiver)) {
      throw std::invalid_argument(
          fmt::format("Sink {}引用不存在的消息接收者: {}", node->id,
                      node->message_receiver));
    }
  }
  std::unordered_set<std::string> parented;
  ValidateEdges(config.input.downstream, sink::SinkMediaType::kPacket, "input",
                media, parented);
  for (const auto& node : config.sinks) {
    ValidateEdges(node->downstream, media.at(node->id).output, node->id, media,
                  parented);
  }
  ValidateReachability(config, index);
}

std::unique_ptr<Pipeline> BuildPipeline(const PipelineConfig& config,
                                        const ProcessorBindings& bindings) {
  ValidatePipelineConfig(config);
  const auto index = IndexNodes(config);
  ValidateBindings(bindings.analysis, SinkType::kAnalysisProcessor, index);
  ValidateBindings(bindings.transform, SinkType::kTransformProcessor, index);
  std::unique_ptr<input::Input> input;
  if (config.input.type == InputType::kFile) {
    input = std::make_unique<input::FileInput>(config.input.file);
  } else {
    input = std::make_unique<input::ZlmInput>(config.input.options);
  }
  auto pipeline = std::make_unique<Pipeline>(std::move(input));
  for (const auto& id : config.input.downstream) {
    pipeline->AddSink(BuildSink(*index.at(id), index, bindings));
  }
  for (const auto& node : config.sinks) {
    if (!node->message_receiver.empty()) {
      pipeline->SetMessageReceiver(node->id, node->message_receiver);
    }
  }
  return pipeline;
}

}  // namespace mw::streamer::pipeline
