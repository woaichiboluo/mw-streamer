#include "mw/streamer/config/toml.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <type_traits>
#include <utility>
#include <vector>

#include "mw/log.h"
#include "mw/streamer/init/internal/runtime.h"
#include "mw/streamer/pipeline/internal/pipeline_builder.h"

namespace mw::streamer {
namespace {

using Table = toml::table;
using mw::log::LogConfig;
using mw::log::LogLevel;
using mw::log::OverflowPolicy;

std::string FieldPath(std::string_view table_path, std::string_view key) {
  if (table_path.empty()) {
    return std::string(key);
  }
  return fmt::format("{}.{}", table_path, key);
}

[[noreturn]] void ThrowTypeError(std::string_view path,
                                 std::string_view expected) {
  throw std::invalid_argument(
      fmt::format("TOML配置项{}必须是{}", path, expected));
}

void WarnUnknownKeys(const Table& table,
                     std::initializer_list<std::string_view> allowed,
                     std::string_view table_path) {
  for (const auto& [key, value] : table) {
    static_cast<void>(value);
    const std::string_view name = key.str();
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      MW_LOG_WARNING("streamer", "忽略未知TOML配置项: {}",
                     FieldPath(table_path, name));
    }
  }
}

const Table* OptionalTable(const Table& parent, std::string_view key,
                           std::string_view parent_path) {
  const auto* node = parent.get(key);
  if (!node) {
    return nullptr;
  }
  const auto* table = node->as_table();
  if (!table) {
    ThrowTypeError(FieldPath(parent_path, key), "表");
  }
  return table;
}

template <typename Integer>
void ReadInteger(const Table& table, std::string_view key,
                 std::string_view table_path, Integer* output) {
  static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
  const auto* node = table.get(key);
  if (!node) {
    return;
  }
  const auto value = node->value_exact<std::int64_t>();
  const auto path = FieldPath(table_path, key);
  if (!value) {
    ThrowTypeError(path, "整数");
  }

  if constexpr (std::is_signed_v<Integer>) {
    if (*value <
            static_cast<std::int64_t>(std::numeric_limits<Integer>::lowest()) ||
        *value >
            static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
      throw std::out_of_range(fmt::format("TOML配置项{}超出有效范围", path));
    }
  } else {
    if (*value < 0 ||
        static_cast<std::uint64_t>(*value) >
            static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
      throw std::out_of_range(fmt::format("TOML配置项{}超出有效范围", path));
    }
  }
  *output = static_cast<Integer>(*value);
}

void ReadMilliseconds(const Table& table, std::string_view key,
                      std::string_view table_path,
                      std::chrono::milliseconds* output) {
  std::chrono::milliseconds::rep value = output->count();
  ReadInteger(table, key, table_path, &value);
  *output = std::chrono::milliseconds(value);
}

void ReadBool(const Table& table, std::string_view key,
              std::string_view table_path, bool* output) {
  const auto* node = table.get(key);
  if (!node) {
    return;
  }
  const auto value = node->value_exact<bool>();
  if (!value) {
    ThrowTypeError(FieldPath(table_path, key), "布尔值");
  }
  *output = *value;
}

void ReadString(const Table& table, std::string_view key,
                std::string_view table_path, std::string* output) {
  const auto* node = table.get(key);
  if (!node) {
    return;
  }
  const auto value = node->value<std::string>();
  if (!value) {
    ThrowTypeError(FieldPath(table_path, key), "字符串");
  }
  *output = *value;
}

void ReadStringArray(const Table& table, std::string_view key,
                     std::string_view table_path,
                     std::vector<std::string>* output) {
  const auto* node = table.get(key);
  if (!node) {
    return;
  }
  const auto* array = node->as_array();
  const auto path = FieldPath(table_path, key);
  if (!array) {
    ThrowTypeError(path, "字符串数组");
  }

  std::vector<std::string> values;
  values.reserve(array->size());
  for (std::size_t index = 0; index < array->size(); ++index) {
    const auto value = (*array)[index].value<std::string>();
    if (!value) {
      ThrowTypeError(fmt::format("{}[{}]", path, index), "字符串");
    }
    values.push_back(*value);
  }
  *output = std::move(values);
}

void ReadStringMap(const Table& table, std::string_view key,
                   std::string_view table_path,
                   std::map<std::string, std::string>* output) {
  const auto* properties = OptionalTable(table, key, table_path);
  if (!properties) {
    return;
  }

  std::map<std::string, std::string> values;
  const auto path = FieldPath(table_path, key);
  for (const auto& [property, node] : *properties) {
    const auto value = node.value<std::string>();
    if (!value) {
      ThrowTypeError(FieldPath(path, property.str()), "字符串");
    }
    values.emplace(std::string(property.str()), *value);
  }
  *output = std::move(values);
}

template <typename Enum>
void ReadEnum(const Table& table, std::string_view key,
              std::string_view table_path,
              std::initializer_list<std::pair<std::string_view, Enum>> choices,
              Enum* output) {
  const auto* node = table.get(key);
  if (!node) {
    return;
  }
  const auto value = node->value<std::string_view>();
  const auto path = FieldPath(table_path, key);
  if (!value) {
    ThrowTypeError(path, "字符串枚举");
  }
  const auto choice = std::find_if(
      choices.begin(), choices.end(),
      [&](const auto& candidate) { return candidate.first == *value; });
  if (choice == choices.end()) {
    throw std::invalid_argument(
        fmt::format("TOML配置项{}包含未知枚举值: {}", path, *value));
  }
  *output = choice->second;
}

Table ParseFile(const std::filesystem::path& path) {
  try {
    return toml::parse_file(path.string());
  } catch (const toml::parse_error& error) {
    throw std::invalid_argument(
        fmt::format("解析TOML配置失败: path={}, line={}, column={}, error={}",
                    path.string(), error.source().begin.line,
                    error.source().begin.column, error.description()));
  }
}

void ReadLogLevel(const Table& table, std::string_view key,
                  std::string_view table_path, LogLevel* output) {
  ReadEnum(table, key, table_path,
           {{"off", LogLevel::kOff},
            {"trace", LogLevel::kTrace},
            {"debug", LogLevel::kDebug},
            {"info", LogLevel::kInfo},
            {"warning", LogLevel::kWarning},
            {"error", LogLevel::kError},
            {"critical", LogLevel::kCritical}},
           output);
}

void ReadLogConfig(const Table& table, LogConfig* config) {
  WarnUnknownKeys(
      table, {"level", "modules", "console", "rotating_file", "async"}, "log");
  ReadLogLevel(table, "level", "log", &config->level);
  if (const auto* modules = OptionalTable(table, "modules", "log")) {
    constexpr std::string_view kPath = "log.modules";
    for (const auto& [key, value] : *modules) {
      static_cast<void>(value);
      mw::log::ModuleLogConfig module;
      module.name = key.str();
      ReadLogLevel(*modules, module.name, kPath, &module.level);
      config->modules.emplace_back(std::move(module));
    }
  }
  if (const auto* console = OptionalTable(table, "console", "log")) {
    constexpr std::string_view kPath = "log.console";
    WarnUnknownKeys(*console, {"color", "level"}, kPath);
    ReadBool(*console, "color", kPath, &config->console.color);
    ReadLogLevel(*console, "level", kPath, &config->console.level);
  }
  if (const auto* rotating_file =
          OptionalTable(table, "rotating_file", "log")) {
    constexpr std::string_view kPath = "log.rotating_file";
    WarnUnknownKeys(*rotating_file,
                    {"path", "level", "max_file_size", "max_files"}, kPath);
    ReadString(*rotating_file, "path", kPath, &config->rotating_file.path);
    ReadLogLevel(*rotating_file, "level", kPath, &config->rotating_file.level);
    ReadInteger(*rotating_file, "max_file_size", kPath,
                &config->rotating_file.max_file_size);
    ReadInteger(*rotating_file, "max_files", kPath,
                &config->rotating_file.max_files);
  }
  if (const auto* async = OptionalTable(table, "async", "log")) {
    constexpr std::string_view kPath = "log.async";
    WarnUnknownKeys(*async, {"enabled", "queue_size", "overflow"}, kPath);
    ReadBool(*async, "enabled", kPath, &config->async.enabled);
    ReadInteger(*async, "queue_size", kPath, &config->async.queue_size);
    ReadEnum(*async, "overflow", kPath,
             {{"block", OverflowPolicy::kBlock},
              {"overrun_oldest", OverflowPolicy::kOverrunOldest}},
             &config->async.overflow);
  }
}

void ReadInitZlmConfig(const Table& table, ZlmConfig* config) {
  constexpr std::string_view kPath = "zlm";
  WarnUnknownKeys(
      table, {"event_poller_threads", "work_threads", "enable_cpu_affinity"},
      kPath);
  ReadInteger(table, "event_poller_threads", kPath,
              &config->event_poller_threads);
  ReadInteger(table, "work_threads", kPath, &config->work_threads);
  ReadBool(table, "enable_cpu_affinity", kPath, &config->enable_cpu_affinity);
}

void ReadPlayerConfig(const Table& table, PlayerConfig* config,
                      std::string_view path) {
  WarnUnknownKeys(
      table, {"connect_timeout_ms", "media_timeout_ms", "local_bind_ip"}, path);
  ReadMilliseconds(table, "connect_timeout_ms", path, &config->connect_timeout);
  ReadMilliseconds(table, "media_timeout_ms", path, &config->media_timeout);
  ReadString(table, "local_bind_ip", path, &config->local_bind_ip);
}

void ReadOutputConfig(const Table& table, OutputConfig* config,
                      std::string_view path) {
  WarnUnknownKeys(table, {"pusher", "muxer", "recording"}, path);
  if (const auto* pusher = OptionalTable(table, "pusher", path)) {
    const auto child_path = FieldPath(path, "pusher");
    WarnUnknownKeys(*pusher, {"connect_timeout_ms", "local_bind_ip"},
                    child_path);
    ReadMilliseconds(*pusher, "connect_timeout_ms", child_path,
                     &config->pusher.connect_timeout);
    ReadString(*pusher, "local_bind_ip", child_path,
               &config->pusher.local_bind_ip);
  }
  if (const auto* muxer = OptionalTable(table, "muxer", path)) {
    const auto child_path = FieldPath(path, "muxer");
    WarnUnknownKeys(*muxer, {"paced_sender_interval_ms"}, child_path);
    ReadMilliseconds(*muxer, "paced_sender_interval_ms", child_path,
                     &config->muxer.paced_sender_interval);
  }
  if (const auto* recording = OptionalTable(table, "recording", path)) {
    const auto child_path = FieldPath(path, "recording");
    WarnUnknownKeys(*recording, {"file_buffer_size", "hls_segment_duration_ms"},
                    child_path);
    ReadInteger(*recording, "file_buffer_size", child_path,
                &config->recording.file_buffer_size);
    ReadMilliseconds(*recording, "hls_segment_duration_ms", child_path,
                     &config->recording.hls_segment_duration);
  }
}

void ReadReconnectPolicyAt(const Table& table, ReconnectPolicy* config,
                           std::string_view path) {
  WarnUnknownKeys(
      table, {"max_retries", "min_delay_ms", "max_delay_ms", "delay_step_ms"},
      path);
  ReadInteger(table, "max_retries", path, &config->max_retries);
  ReadMilliseconds(table, "min_delay_ms", path, &config->min_delay);
  ReadMilliseconds(table, "max_delay_ms", path, &config->max_delay);
  ReadMilliseconds(table, "delay_step_ms", path, &config->delay_step);
}

void ReadAudioDecoderConfigAt(const Table& table, AudioDecoderConfig* config,
                              std::string_view path) {
  WarnUnknownKeys(table, {"decoder_name"}, path);
  ReadString(table, "decoder_name", path, &config->decoder_name);
}

void ReadVideoDecoderConfigAt(const Table& table, VideoDecoderConfig* config,
                              std::string_view path) {
  WarnUnknownKeys(table, {"decoder_name", "backend", "device_index"}, path);
  ReadString(table, "decoder_name", path, &config->decoder_name);
  ReadEnum(table, "backend", path,
           {{"software", VideoDecoderBackend::kSoftware},
            {"cuda", VideoDecoderBackend::kCuda}},
           &config->backend);
  ReadInteger(table, "device_index", path, &config->device_index);
}

std::string FormatToml(const Table& table) {
  std::ostringstream output;
  output << toml::toml_formatter{table};
  return output.str();
}

void ReadAudioEncoderConfigAt(const Table& table, AudioEncoderConfig* config,
                              std::string_view path) {
  WarnUnknownKeys(table, {"encoder_name", "properties"}, path);
  ReadString(table, "encoder_name", path, &config->encoder_name);
  ReadStringMap(table, "properties", path, &config->properties);
}

void ReadVideoEncoderConfigAt(const Table& table, VideoEncoderConfig* config,
                              std::string_view path) {
  WarnUnknownKeys(table, {"codec", "encoder_name", "frame_rate", "properties"},
                  path);
  ReadEnum(table, "codec", path,
           {{"h264", kMwStreamerCodecH264}, {"h265", kMwStreamerCodecH265}},
           &config->codec);
  ReadString(table, "encoder_name", path, &config->encoder_name);
  if (const auto* frame_rate = OptionalTable(table, "frame_rate", path)) {
    const auto frame_rate_path = FieldPath(path, "frame_rate");
    WarnUnknownKeys(*frame_rate, {"num", "den"}, frame_rate_path);
    ReadInteger(*frame_rate, "num", frame_rate_path, &config->frame_rate.num);
    ReadInteger(*frame_rate, "den", frame_rate_path, &config->frame_rate.den);
  }
  ReadStringMap(table, "properties", path, &config->properties);
}

template <typename Config, typename Reader>
void ReadOptionalConfigTable(const Table& root, std::string_view key,
                             Config* config, Reader reader) {
  if (const auto* table = OptionalTable(root, key, "")) {
    reader(*table, config);
  }
}

void RequireField(const Table& table, std::string_view key,
                  std::string_view path) {
  if (!table.contains(key)) {
    throw std::invalid_argument(
        fmt::format("缺少TOML配置项: {}", FieldPath(path, key)));
  }
}

Table ParseText(std::string_view text, std::string_view path = "") {
  try {
    return toml::parse(text);
  } catch (const toml::parse_error& error) {
    throw std::invalid_argument(
        fmt::format("解析TOML配置失败: path={}, line={}, column={}, error={}",
                    path, error.source().begin.line,
                    error.source().begin.column, error.description()));
  }
}

InputConfig ReadInput(const Table& root) {
  RequireField(root, "input", "");
  const auto& table = *OptionalTable(root, "input", "");
  constexpr std::string_view kPath = "input";
  RequireField(table, "type", kPath);
  RequireField(table, "downstream", kPath);
  InputConfig result;
  ReadEnum(table, "type", kPath,
           {{"zlm", InputType::kZlm}, {"file", InputType::kFile}},
           &result.type);
  ReadStringArray(table, "downstream", kPath, &result.downstream);
  if (result.type == InputType::kFile) {
    WarnUnknownKeys(table, {"type", "path", "downstream"}, kPath);
    RequireField(table, "path", kPath);
    ReadString(table, "path", kPath, &result.file.path);
    return result;
  }
  WarnUnknownKeys(table,
                  {"type", "url", "downstream", "player", "reconnect_policy"},
                  kPath);
  RequireField(table, "url", kPath);
  ReadString(table, "url", kPath, &result.options.url);
  if (const auto* player = OptionalTable(table, "player", kPath)) {
    ReadPlayerConfig(*player, &result.options.player, "input.player");
  }
  if (const auto* reconnect = OptionalTable(table, "reconnect_policy", kPath)) {
    ReadReconnectPolicyAt(*reconnect, &result.options.reconnect_policy,
                          "input.reconnect_policy");
  }
  return result;
}

std::unique_ptr<SinkConfig> ReadDecoderNode(const Table& table, std::string id,
                                            std::string_view path) {
  WarnUnknownKeys(
      table,
      {"id", "type", "downstream", "message_receiver", "cache_duration_ms",
       "audio_decode_queue_capacity", "video_decode_queue_capacity",
       "audio_decoder", "video_decoder"},
      path);
  auto node = std::make_unique<DecoderNodeConfig>(std::move(id));
  auto& config = node->options;
  ReadMilliseconds(table, "cache_duration_ms", path, &config.cache_duration);
  ReadInteger(table, "audio_decode_queue_capacity", path,
              &config.audio_decode_queue_capacity);
  ReadInteger(table, "video_decode_queue_capacity", path,
              &config.video_decode_queue_capacity);
  if (const auto* audio = OptionalTable(table, "audio_decoder", path)) {
    ReadAudioDecoderConfigAt(*audio, &config.audio_decoder,
                             FieldPath(path, "audio_decoder"));
  }
  if (const auto* video = OptionalTable(table, "video_decoder", path)) {
    ReadVideoDecoderConfigAt(*video, &config.video_decoder,
                             FieldPath(path, "video_decoder"));
  }
  return node;
}

std::unique_ptr<SinkConfig> ReadProcessorNode(const Table& table,
                                              std::string id, SinkType type,
                                              std::string_view path) {
  if (type == SinkType::kAnalysisProcessor) {
    WarnUnknownKeys(table, {"id", "type", "downstream", "message_receiver"},
                    path);
    return std::make_unique<AnalysisProcessorNodeConfig>(std::move(id));
  }
  WarnUnknownKeys(table, {"id", "type", "downstream", "message_receiver"},
                  path);
  return std::make_unique<TransformProcessorNodeConfig>(std::move(id));
}

std::unique_ptr<SinkConfig> ReadSynchronizerNode(const Table& table,
                                                 std::string id,
                                                 std::string_view path) {
  WarnUnknownKeys(
      table,
      {"id", "type", "downstream", "message_receiver", "frame_queue_capacity",
       "max_frame_lateness_ms", "standby_timeout_ms", "standby_image_path"},
      path);
  auto node = std::make_unique<SynchronizerNodeConfig>(std::move(id));
  auto& config = node->options;
  ReadInteger(table, "frame_queue_capacity", path,
              &config.frame_queue_capacity);
  ReadMilliseconds(table, "max_frame_lateness_ms", path,
                   &config.max_frame_lateness);
  ReadMilliseconds(table, "standby_timeout_ms", path, &config.standby_timeout);
  ReadString(table, "standby_image_path", path, &config.standby_image_path);
  return node;
}

std::unique_ptr<SinkConfig> ReadEncoderNode(const Table& table, std::string id,
                                            std::string_view path) {
  WarnUnknownKeys(
      table,
      {"id", "type", "downstream", "message_receiver", "frame_queue_capacity",
       "startup_packet_capacity", "audio_encoder", "video_encoder"},
      path);
  auto node = std::make_unique<EncoderNodeConfig>(std::move(id));
  auto& config = node->options;
  ReadInteger(table, "frame_queue_capacity", path,
              &config.frame_queue_capacity);
  ReadInteger(table, "startup_packet_capacity", path,
              &config.startup_packet_capacity);
  if (const auto* audio = OptionalTable(table, "audio_encoder", path)) {
    ReadAudioEncoderConfigAt(*audio, &config.audio_encoder,
                             FieldPath(path, "audio_encoder"));
  }
  if (const auto* video = OptionalTable(table, "video_encoder", path)) {
    ReadVideoEncoderConfigAt(*video, &config.video_encoder,
                             FieldPath(path, "video_encoder"));
  }
  return node;
}

std::unique_ptr<SinkConfig> ReadRemuxNode(const Table& table, std::string id,
                                          std::string_view path) {
  WarnUnknownKeys(table,
                  {"id", "type", "downstream", "message_receiver", "target",
                   "packet_queue_capacity", "zlm"},
                  path);
  RequireField(table, "target", path);
  auto node = std::make_unique<RemuxNodeConfig>(std::move(id));
  ReadString(table, "target", path, &node->options.target);
  ReadInteger(table, "packet_queue_capacity", path,
              &node->options.packet_queue_capacity);
  if (const auto* output = OptionalTable(table, "zlm", path)) {
    ReadOutputConfig(*output, &node->options.zlm, FieldPath(path, "zlm"));
  }
  return node;
}

std::unique_ptr<SinkConfig> ReadSinkNode(const Table& table,
                                         std::string_view path) {
  RequireField(table, "id", path);
  RequireField(table, "type", path);
  std::string id;
  ReadString(table, "id", path, &id);
  SinkType type;
  ReadEnum(table, "type", path,
           {{"decoder", SinkType::kDecoder},
            {"analysis_processor", SinkType::kAnalysisProcessor},
            {"transform_processor", SinkType::kTransformProcessor},
            {"synchronizer", SinkType::kSynchronizer},
            {"encoder", SinkType::kEncoder},
            {"remux", SinkType::kRemux}},
           &type);
  std::unique_ptr<SinkConfig> result;
  switch (type) {
    case SinkType::kDecoder:
      result = ReadDecoderNode(table, std::move(id), path);
      break;
    case SinkType::kAnalysisProcessor:
    case SinkType::kTransformProcessor:
      result = ReadProcessorNode(table, std::move(id), type, path);
      break;
    case SinkType::kSynchronizer:
      result = ReadSynchronizerNode(table, std::move(id), path);
      break;
    case SinkType::kEncoder:
      result = ReadEncoderNode(table, std::move(id), path);
      break;
    case SinkType::kRemux:
      result = ReadRemuxNode(table, std::move(id), path);
      break;
  }
  ReadStringArray(table, "downstream", path, &result->downstream);
  ReadString(table, "message_receiver", path, &result->message_receiver);
  return result;
}

PipelineConfig ReadPipeline(const Table& root) {
  WarnUnknownKeys(root, {"log", "zlm", "input", "sinks"}, "");
  PipelineConfig result;
  result.input = ReadInput(root);
  RequireField(root, "sinks", "");
  const auto* sinks = root.get("sinks")->as_array();
  if (!sinks) {
    ThrowTypeError("sinks", "表数组");
  }
  for (std::size_t index = 0; index < sinks->size(); ++index) {
    const auto path = fmt::format("sinks[{}]", index);
    const auto* table = (*sinks)[index].as_table();
    if (!table) {
      ThrowTypeError(path, "表");
    }
    result.sinks.push_back(ReadSinkNode(*table, path));
  }
  ValidatePipelineConfig(result);
  return result;
}

// TOML integers are signed 64-bit even where C++ capacities use size_t.
std::int64_t WriteCapacity(std::size_t value) {
  if (value >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::out_of_range("配置容量超出TOML整数范围");
  }
  return static_cast<std::int64_t>(value);
}

toml::array WriteStringArray(const std::vector<std::string>& values) {
  toml::array result;
  for (const auto& value : values) {
    result.push_back(value);
  }
  return result;
}

Table WriteProperties(const std::map<std::string, std::string>& properties) {
  Table result;
  for (const auto& [key, value] : properties) {
    result.insert(key, value);
  }
  return result;
}

template <typename Rational>
Table WriteRational(const Rational& value) {
  Table result{{"num", value.num}, {"den", value.den}};
  result.is_inline(true);
  return result;
}

Table WriteInput(const InputConfig& input) {
  if (input.type == InputType::kFile) {
    return Table{{"type", "file"},
                 {"path", input.file.path},
                 {"downstream", WriteStringArray(input.downstream)}};
  }
  const auto& config = input.options;
  const auto& reconnect = config.reconnect_policy;
  return Table{
      {"type", "zlm"},
      {"url", config.url},
      {"downstream", WriteStringArray(input.downstream)},
      {"player",
       Table{{"connect_timeout_ms", config.player.connect_timeout.count()},
             {"media_timeout_ms", config.player.media_timeout.count()},
             {"local_bind_ip", config.player.local_bind_ip}}},
      {"reconnect_policy",
       Table{{"max_retries", reconnect.max_retries},
             {"min_delay_ms", reconnect.min_delay.count()},
             {"max_delay_ms", reconnect.max_delay.count()},
             {"delay_step_ms", reconnect.delay_step.count()}}}};
}

void WriteDecoderNode(Table& table, const DecoderSinkConfig& config) {
  table.insert("cache_duration_ms", config.cache_duration.count());
  table.insert("audio_decode_queue_capacity",
               WriteCapacity(config.audio_decode_queue_capacity));
  table.insert("video_decode_queue_capacity",
               WriteCapacity(config.video_decode_queue_capacity));
  table.insert("audio_decoder",
               Table{{"decoder_name", config.audio_decoder.decoder_name}});
  table.insert("video_decoder",
               Table{{"decoder_name", config.video_decoder.decoder_name},
                     {"backend",
                      config.video_decoder.backend == VideoDecoderBackend::kCuda
                          ? "cuda"
                          : "software"},
                     {"device_index", config.video_decoder.device_index}});
}

void WriteSynchronizerNode(Table& table, const SynchronizerSinkConfig& config) {
  table.insert("frame_queue_capacity",
               WriteCapacity(config.frame_queue_capacity));
  table.insert("max_frame_lateness_ms", config.max_frame_lateness.count());
  table.insert("standby_timeout_ms", config.standby_timeout.count());
  table.insert("standby_image_path", config.standby_image_path);
}

void WriteEncoderNode(Table& table, const EncoderSinkConfig& config) {
  table.insert("frame_queue_capacity",
               WriteCapacity(config.frame_queue_capacity));
  table.insert("startup_packet_capacity",
               WriteCapacity(config.startup_packet_capacity));
  table.insert(
      "audio_encoder",
      Table{{"encoder_name", config.audio_encoder.encoder_name},
            {"properties", WriteProperties(config.audio_encoder.properties)}});
  table.insert(
      "video_encoder",
      Table{{"codec", config.video_encoder.codec == kMwStreamerCodecH264
                          ? "h264"
                          : "h265"},
            {"encoder_name", config.video_encoder.encoder_name},
            {"frame_rate", WriteRational(config.video_encoder.frame_rate)},
            {"properties", WriteProperties(config.video_encoder.properties)}});
}

void WriteRemuxNode(Table& table, const RemuxSinkConfig& config) {
  table.insert("target", config.target);
  table.insert("packet_queue_capacity",
               WriteCapacity(config.packet_queue_capacity));
  const auto& output = config.zlm;
  table.insert(
      "zlm",
      Table{{"pusher", Table{{"connect_timeout_ms",
                              output.pusher.connect_timeout.count()},
                             {"local_bind_ip", output.pusher.local_bind_ip}}},
            {"muxer", Table{{"paced_sender_interval_ms",
                             output.muxer.paced_sender_interval.count()}}},
            {"recording",
             Table{{"file_buffer_size",
                    WriteCapacity(output.recording.file_buffer_size)},
                   {"hls_segment_duration_ms",
                    output.recording.hls_segment_duration.count()}}}});
}

Table WriteSinkNode(const SinkConfig& node) {
  Table result{{"id", node.id},
               {"downstream", WriteStringArray(node.downstream)}};
  if (!node.message_receiver.empty()) {
    result.insert("message_receiver", node.message_receiver);
  }
  switch (node.type()) {
    case SinkType::kDecoder:
      result.insert("type", "decoder");
      WriteDecoderNode(result,
                       static_cast<const DecoderNodeConfig&>(node).options);
      break;
    case SinkType::kAnalysisProcessor:
      result.insert("type", "analysis_processor");
      break;
    case SinkType::kTransformProcessor:
      result.insert("type", "transform_processor");
      break;
    case SinkType::kSynchronizer:
      result.insert("type", "synchronizer");
      WriteSynchronizerNode(
          result, static_cast<const SynchronizerNodeConfig&>(node).options);
      break;
    case SinkType::kEncoder:
      result.insert("type", "encoder");
      WriteEncoderNode(result,
                       static_cast<const EncoderNodeConfig&>(node).options);
      break;
    case SinkType::kRemux:
      result.insert("type", "remux");
      WriteRemuxNode(result, static_cast<const RemuxNodeConfig&>(node).options);
      break;
  }
  return result;
}

void ResolveLocalPath(std::string* value,
                      const std::filesystem::path& directory) {
  if (value->empty() || value->find("://") != std::string::npos) {
    return;
  }
  *value = (directory / *value).lexically_normal().string();
}

}  // namespace

PipelineConfig ParsePipelineConfigFromToml(std::string_view text) {
  return ReadPipeline(ParseText(text));
}

std::string SerializePipelineConfigToToml(const PipelineConfig& config) {
  ValidatePipelineConfig(config);
  toml::array sinks;
  for (const auto& sink : config.sinks) {
    sinks.push_back(WriteSinkNode(*sink));
  }
  return FormatToml(
      Table{{"input", WriteInput(config.input)}, {"sinks", std::move(sinks)}});
}

PipelineConfig LoadPipelineConfigFromToml(const std::filesystem::path& path) {
  auto config = ReadPipeline(ParseFile(path));
  const auto directory = std::filesystem::absolute(path).parent_path();
  if (config.input.type == InputType::kFile) {
    ResolveLocalPath(&config.input.file.path, directory);
  } else {
    ResolveLocalPath(&config.input.options.url, directory);
  }
  for (const auto& sink : config.sinks) {
    if (auto* remux = dynamic_cast<RemuxNodeConfig*>(sink.get())) {
      ResolveLocalPath(&remux->options.target, directory);
    } else if (auto* sync = dynamic_cast<SynchronizerNodeConfig*>(sink.get())) {
      ResolveLocalPath(&sync->options.standby_image_path, directory);
    }
  }
  return config;
}

void SavePipelineConfigToToml(const PipelineConfig& config,
                              const std::filesystem::path& path) {
  const auto text = SerializePipelineConfigToToml(config);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error(
        fmt::format("无法写入TOML配置文件: {}", path.string()));
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  if (!output) {
    throw std::runtime_error(
        fmt::format("写入TOML配置文件失败: {}", path.string()));
  }
}

std::unique_ptr<Pipeline> BuildPipelineFromToml(
    const std::filesystem::path& path, const ProcessorBindings& bindings) {
  const auto root = ParseFile(path);
  internal::RuntimeConfig init_config;
  ReadOptionalConfigTable(root, "log", &init_config.log, ReadLogConfig);
  ReadOptionalConfigTable(root, "zlm", &init_config.zlm, ReadInitZlmConfig);
  auto config = ReadPipeline(root);
  const auto directory = std::filesystem::absolute(path).parent_path();
  if (config.input.type == InputType::kFile) {
    ResolveLocalPath(&config.input.file.path, directory);
  } else {
    ResolveLocalPath(&config.input.options.url, directory);
  }
  for (const auto& sink : config.sinks) {
    if (auto* remux = dynamic_cast<RemuxNodeConfig*>(sink.get())) {
      ResolveLocalPath(&remux->options.target, directory);
    } else if (auto* synchronizer =
                   dynamic_cast<SynchronizerNodeConfig*>(sink.get())) {
      ResolveLocalPath(&synchronizer->options.standby_image_path, directory);
    }
  }
  return internal::BuildPipelineWithRuntime(config, bindings, init_config);
}

}  // namespace mw::streamer
