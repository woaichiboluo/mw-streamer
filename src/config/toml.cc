#include "mw/config/toml.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <type_traits>
#include <vector>

namespace mw::streamer::config {
namespace {

using Table = toml::table;

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

void RequireOnlyKeys(const Table& table,
                     std::initializer_list<std::string_view> allowed,
                     std::string_view table_path) {
  for (const auto& [key, value] : table) {
    static_cast<void>(value);
    const std::string_view name = key.str();
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      throw std::invalid_argument(
          fmt::format("未知TOML配置项: {}", FieldPath(table_path, name)));
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
  const auto value = node->value<std::int64_t>();
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
  const auto value = node->value<bool>();
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
                  std::string_view table_path, log::LogLevel* output) {
  ReadEnum(table, key, table_path,
           {{"off", log::LogLevel::kOff},
            {"trace", log::LogLevel::kTrace},
            {"debug", log::LogLevel::kDebug},
            {"info", log::LogLevel::kInfo},
            {"warning", log::LogLevel::kWarning},
            {"error", log::LogLevel::kError},
            {"critical", log::LogLevel::kCritical}},
           output);
}

void ReadLogConfig(const Table& table, log::LogConfig* config) {
  RequireOnlyKeys(table, {"modules", "console", "rotating_file", "async"},
                  "log");
  if (const auto* modules = OptionalTable(table, "modules", "log")) {
    constexpr std::string_view kPath = "log.modules";
    RequireOnlyKeys(*modules, {"zlm", "srt", "ffmpeg", "streamer", "processor"},
                    kPath);
    ReadLogLevel(*modules, "zlm", kPath, &config->modules.zlm);
    ReadLogLevel(*modules, "srt", kPath, &config->modules.srt);
    ReadLogLevel(*modules, "ffmpeg", kPath, &config->modules.ffmpeg);
    ReadLogLevel(*modules, "streamer", kPath, &config->modules.streamer);
    ReadLogLevel(*modules, "processor", kPath, &config->modules.processor);
  }
  if (const auto* console = OptionalTable(table, "console", "log")) {
    constexpr std::string_view kPath = "log.console";
    RequireOnlyKeys(*console, {"enabled", "color", "level"}, kPath);
    ReadBool(*console, "enabled", kPath, &config->console.enabled);
    ReadBool(*console, "color", kPath, &config->console.color);
    ReadLogLevel(*console, "level", kPath, &config->console.level);
  }
  if (const auto* rotating_file =
          OptionalTable(table, "rotating_file", "log")) {
    constexpr std::string_view kPath = "log.rotating_file";
    RequireOnlyKeys(*rotating_file,
                    {"enabled", "path", "level", "max_file_size", "max_files"},
                    kPath);
    ReadBool(*rotating_file, "enabled", kPath, &config->rotating_file.enabled);
    ReadString(*rotating_file, "path", kPath, &config->rotating_file.path);
    ReadLogLevel(*rotating_file, "level", kPath, &config->rotating_file.level);
    ReadInteger(*rotating_file, "max_file_size", kPath,
                &config->rotating_file.max_file_size);
    ReadInteger(*rotating_file, "max_files", kPath,
                &config->rotating_file.max_files);
  }
  if (const auto* async = OptionalTable(table, "async", "log")) {
    constexpr std::string_view kPath = "log.async";
    RequireOnlyKeys(*async, {"enabled", "queue_size", "overflow"}, kPath);
    ReadBool(*async, "enabled", kPath, &config->async.enabled);
    ReadInteger(*async, "queue_size", kPath, &config->async.queue_size);
    ReadEnum(*async, "overflow", kPath,
             {{"block", log::OverflowPolicy::kBlock},
              {"overrun_oldest", log::OverflowPolicy::kOverrunOldest}},
             &config->async.overflow);
  }
}

void ReadInitZlmConfig(const Table& table, zlm::Config* config) {
  constexpr std::string_view kPath = "zlm";
  RequireOnlyKeys(
      table, {"event_poller_threads", "work_threads", "enable_cpu_affinity"},
      kPath);
  ReadInteger(table, "event_poller_threads", kPath,
              &config->event_poller_threads);
  ReadInteger(table, "work_threads", kPath, &config->work_threads);
  ReadBool(table, "enable_cpu_affinity", kPath, &config->enable_cpu_affinity);
}

void ReadZlmPipelineConfig(const Table& table, zlm::PipelineConfig* config) {
  constexpr std::string_view kPath = "zlm";
  RequireOnlyKeys(table, {"player", "output"}, kPath);
  if (const auto* player = OptionalTable(table, "player", kPath)) {
    constexpr std::string_view kPlayerPath = "zlm.player";
    RequireOnlyKeys(*player,
                    {"connect_timeout_ms", "media_timeout_ms", "local_bind_ip"},
                    kPlayerPath);
    ReadMilliseconds(*player, "connect_timeout_ms", kPlayerPath,
                     &config->player.connect_timeout);
    ReadMilliseconds(*player, "media_timeout_ms", kPlayerPath,
                     &config->player.media_timeout);
    ReadString(*player, "local_bind_ip", kPlayerPath,
               &config->player.local_bind_ip);
  }
  if (const auto* output = OptionalTable(table, "output", kPath)) {
    constexpr std::string_view kOutputPath = "zlm.output";
    RequireOnlyKeys(*output, {"pusher", "muxer", "recording"}, kOutputPath);
    if (const auto* pusher = OptionalTable(*output, "pusher", kOutputPath)) {
      constexpr std::string_view kPusherPath = "zlm.output.pusher";
      RequireOnlyKeys(*pusher, {"connect_timeout_ms", "local_bind_ip"},
                      kPusherPath);
      ReadMilliseconds(*pusher, "connect_timeout_ms", kPusherPath,
                       &config->output.pusher.connect_timeout);
      ReadString(*pusher, "local_bind_ip", kPusherPath,
                 &config->output.pusher.local_bind_ip);
    }
    if (const auto* muxer = OptionalTable(*output, "muxer", kOutputPath)) {
      constexpr std::string_view kMuxerPath = "zlm.output.muxer";
      RequireOnlyKeys(*muxer, {"paced_sender_interval_ms"}, kMuxerPath);
      ReadMilliseconds(*muxer, "paced_sender_interval_ms", kMuxerPath,
                       &config->output.muxer.paced_sender_interval);
    }
    if (const auto* recording =
            OptionalTable(*output, "recording", kOutputPath)) {
      constexpr std::string_view kRecordingPath = "zlm.output.recording";
      RequireOnlyKeys(*recording,
                      {"file_buffer_size", "hls_segment_duration_ms"},
                      kRecordingPath);
      ReadInteger(*recording, "file_buffer_size", kRecordingPath,
                  &config->output.recording.file_buffer_size);
      ReadMilliseconds(*recording, "hls_segment_duration_ms", kRecordingPath,
                       &config->output.recording.hls_segment_duration);
    }
  }
}

void ReadReconnectPolicy(const Table& table, input::ReconnectPolicy* config) {
  constexpr std::string_view kPath = "reconnect_policy";
  RequireOnlyKeys(
      table, {"max_retries", "min_delay_ms", "max_delay_ms", "delay_step_ms"},
      kPath);
  ReadInteger(table, "max_retries", kPath, &config->max_retries);
  ReadMilliseconds(table, "min_delay_ms", kPath, &config->min_delay);
  ReadMilliseconds(table, "max_delay_ms", kPath, &config->max_delay);
  ReadMilliseconds(table, "delay_step_ms", kPath, &config->delay_step);
}

void ReadAudioDecoderConfig(const Table& table,
                            decoder::AudioDecoderConfig* config) {
  constexpr std::string_view kPath = "audio_decoder";
  RequireOnlyKeys(table, {"decoder_name"}, kPath);
  ReadString(table, "decoder_name", kPath, &config->decoder_name);
}

void ReadVideoDecoderConfig(const Table& table,
                            decoder::VideoDecoderConfig* config) {
  constexpr std::string_view kPath = "video_decoder";
  RequireOnlyKeys(table, {"decoder_name", "backend", "device_index"}, kPath);
  ReadString(table, "decoder_name", kPath, &config->decoder_name);
  ReadEnum(table, "backend", kPath,
           {{"software", decoder::VideoDecoderBackend::kSoftware},
            {"cuda", decoder::VideoDecoderBackend::kCuda}},
           &config->backend);
  ReadInteger(table, "device_index", kPath, &config->device_index);
}

std::string SerializeProcessorConfig(const Table& table) {
  std::ostringstream output;
  output << toml::toml_formatter{table};
  return output.str();
}

void ReadStreamingProcessorConfig(const Table& table,
                                  processor::StreamingProcessorConfig* config) {
  constexpr std::string_view kPath = "processor";
  RequireOnlyKeys(table, {"output_width", "output_height", "config"}, kPath);
  ReadInteger(table, "output_width", kPath, &config->output_width);
  ReadInteger(table, "output_height", kPath, &config->output_height);
  if (const auto* value = OptionalTable(table, "config", kPath)) {
    config->config = SerializeProcessorConfig(*value);
  }
}

void ReadFileProcessorConfig(const Table& table,
                             processor::FileProcessorConfig* config) {
  constexpr std::string_view kPath = "processor";
  RequireOnlyKeys(table, {"config"}, kPath);
  if (const auto* value = OptionalTable(table, "config", kPath)) {
    config->config = SerializeProcessorConfig(*value);
  }
}

void ReadAudioEncoderConfig(const Table& table,
                            encoder::AudioEncoderConfig* config) {
  constexpr std::string_view kPath = "audio_encoder";
  RequireOnlyKeys(table, {"encoder_name", "properties"}, kPath);
  ReadString(table, "encoder_name", kPath, &config->encoder_name);
  ReadStringMap(table, "properties", kPath, &config->properties);
}

void ReadVideoEncoderConfig(const Table& table,
                            encoder::VideoEncoderConfig* config) {
  constexpr std::string_view kPath = "video_encoder";
  RequireOnlyKeys(table, {"codec", "encoder_name", "frame_rate", "properties"},
                  kPath);
  ReadEnum(table, "codec", kPath,
           {{"h264", kMwStreamerCodecH264}, {"h265", kMwStreamerCodecH265}},
           &config->codec);
  ReadString(table, "encoder_name", kPath, &config->encoder_name);
  if (const auto* frame_rate = OptionalTable(table, "frame_rate", kPath)) {
    constexpr std::string_view kFrameRatePath = "video_encoder.frame_rate";
    RequireOnlyKeys(*frame_rate, {"num", "den"}, kFrameRatePath);
    ReadInteger(*frame_rate, "num", kFrameRatePath, &config->frame_rate.num);
    ReadInteger(*frame_rate, "den", kFrameRatePath, &config->frame_rate.den);
  }
  ReadStringMap(table, "properties", kPath, &config->properties);
}

void ReadStandbyConfig(const Table& table,
                       pipeline::StreamingStandbyConfig* config) {
  constexpr std::string_view kPath = "standby";
  RequireOnlyKeys(table, {"enabled", "image_path"}, kPath);
  ReadBool(table, "enabled", kPath, &config->enabled);
  ReadString(table, "image_path", kPath, &config->image_path);
}

template <typename Config, typename Reader>
void ReadOptionalConfigTable(const Table& root, std::string_view key,
                             Config* config, Reader reader) {
  if (const auto* table = OptionalTable(root, key, "")) {
    reader(*table, config);
  }
}

}  // namespace

InitConfig LoadInitConfigFromToml(const std::filesystem::path& path) {
  const auto root = ParseFile(path);
  RequireOnlyKeys(root, {"log", "zlm"}, "");

  InitConfig config;
  ReadOptionalConfigTable(root, "log", &config.log, ReadLogConfig);
  ReadOptionalConfigTable(root, "zlm", &config.zlm, ReadInitZlmConfig);
  return config;
}

pipeline::StreamingPipelineConfig LoadStreamingPipelineConfigFromToml(
    const std::filesystem::path& path) {
  const auto root = ParseFile(path);
  RequireOnlyKeys(
      root,
      {"input_url", "zlm", "reconnect_policy", "cache_duration_ms",
       "audio_queue_capacity", "video_queue_capacity", "max_track_wait_ms",
       "audio_decoder", "video_decoder", "processor", "audio_encoder",
       "video_encoder", "standby", "input_targets", "output_targets"},
      "");

  pipeline::StreamingPipelineConfig config;
  ReadString(root, "input_url", "", &config.input_url);
  ReadOptionalConfigTable(root, "zlm", &config.zlm, ReadZlmPipelineConfig);
  ReadOptionalConfigTable(root, "reconnect_policy", &config.reconnect_policy,
                          ReadReconnectPolicy);
  ReadMilliseconds(root, "cache_duration_ms", "", &config.cache_duration);
  ReadInteger(root, "audio_queue_capacity", "", &config.audio_queue_capacity);
  ReadInteger(root, "video_queue_capacity", "", &config.video_queue_capacity);
  ReadMilliseconds(root, "max_track_wait_ms", "", &config.max_track_wait);
  ReadOptionalConfigTable(root, "audio_decoder", &config.audio_decoder,
                          ReadAudioDecoderConfig);
  ReadOptionalConfigTable(root, "video_decoder", &config.video_decoder,
                          ReadVideoDecoderConfig);
  ReadOptionalConfigTable(root, "processor", &config.processor,
                          ReadStreamingProcessorConfig);
  ReadOptionalConfigTable(root, "audio_encoder", &config.audio_encoder,
                          ReadAudioEncoderConfig);
  ReadOptionalConfigTable(root, "video_encoder", &config.video_encoder,
                          ReadVideoEncoderConfig);
  ReadOptionalConfigTable(root, "standby", &config.standby, ReadStandbyConfig);
  ReadStringArray(root, "input_targets", "", &config.input_targets);
  ReadStringArray(root, "output_targets", "", &config.output_targets);
  return config;
}

pipeline::RemuxPipelineConfig LoadRemuxPipelineConfigFromToml(
    const std::filesystem::path& path) {
  const auto root = ParseFile(path);
  RequireOnlyKeys(
      root, {"input_url", "zlm", "reconnect_policy", "output_targets"}, "");

  pipeline::RemuxPipelineConfig config;
  ReadString(root, "input_url", "", &config.input_url);
  ReadOptionalConfigTable(root, "zlm", &config.zlm, ReadZlmPipelineConfig);
  ReadOptionalConfigTable(root, "reconnect_policy", &config.reconnect_policy,
                          ReadReconnectPolicy);
  ReadStringArray(root, "output_targets", "", &config.output_targets);
  return config;
}

pipeline::LocalFilePipelineConfig LoadFilePipelineConfigFromToml(
    const std::filesystem::path& path) {
  const auto root = ParseFile(path);
  RequireOnlyKeys(
      root, {"input_path", "audio_decoder", "video_decoder", "processor"}, "");

  pipeline::LocalFilePipelineConfig config;
  ReadString(root, "input_path", "", &config.input_path);
  ReadOptionalConfigTable(root, "audio_decoder", &config.audio_decoder,
                          ReadAudioDecoderConfig);
  ReadOptionalConfigTable(root, "video_decoder", &config.video_decoder,
                          ReadVideoDecoderConfig);
  ReadOptionalConfigTable(root, "processor", &config.processor,
                          ReadFileProcessorConfig);
  return config;
}

}  // namespace mw::streamer::config
