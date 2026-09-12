#include "mw/c_api.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "mw/config/toml.h"
#include "mw/input/input_state.h"
#include "mw/performance/pipeline_snapshot.h"
#include "mw/pipeline/pipeline.h"
#include "mw/pipeline/pipeline_builder.h"

struct MwPipeline {
  std::unique_ptr<mw::streamer::Pipeline> pipeline;
};

namespace {

thread_local std::string last_error;

void ClearError() { last_error.clear(); }

MwResult Fail(MwResult result, const char* message) noexcept {
  try {
    last_error = message == nullptr ? "" : message;
  } catch (...) {
    last_error.clear();
  }
  return result;
}

template <typename Function>
MwResult Guard(Function&& function) noexcept {
  try {
    ClearError();
    function();
    return kMwResultSuccess;
  } catch (const std::bad_alloc& error) {
    return Fail(kMwResultOutOfMemory, error.what());
  } catch (const std::invalid_argument& error) {
    return Fail(kMwResultInvalidArgument, error.what());
  } catch (const std::logic_error& error) {
    return Fail(kMwResultInvalidState, error.what());
  } catch (const std::exception& error) {
    return Fail(kMwResultInternalError, error.what());
  } catch (...) {
    return Fail(kMwResultInternalError, "未知的内部错误");
  }
}

template <typename Binding, typename Callbacks>
void AddBindings(const Binding* bindings, std::size_t count,
                 std::map<std::string, Callbacks>* output) {
  if (count != 0 && bindings == nullptr) {
    throw std::invalid_argument("Processor binding数组不能为空");
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (bindings[i].processor_id == nullptr ||
        bindings[i].processor_id[0] == '\0') {
      throw std::invalid_argument("Processor ID不能为空");
    }
    if (!output->emplace(bindings[i].processor_id, bindings[i].callbacks)
             .second) {
      throw std::invalid_argument("Processor ID不能重复");
    }
  }
}

MwPipelineState ConvertState(mw::streamer::PipelineState state) {
  using PipelineState = mw::streamer::PipelineState;
  switch (state) {
    case PipelineState::kIdle:
      return kMwPipelineIdle;
    case PipelineState::kRunning:
      return kMwPipelineRunning;
    case PipelineState::kStopping:
      return kMwPipelineStopping;
    case PipelineState::kStopped:
      return kMwPipelineStopped;
    case PipelineState::kFailed:
      return kMwPipelineFailed;
  }
  throw std::runtime_error("未知的Pipeline状态");
}

MwInputState ConvertInputState(mw::streamer::InputState state) {
  using InputState = mw::streamer::InputState;
  switch (state) {
    case InputState::kIdle:
      return kMwInputIdle;
    case InputState::kConnecting:
      return kMwInputConnecting;
    case InputState::kReady:
      return kMwInputReady;
    case InputState::kWaitingRetry:
      return kMwInputWaitingRetry;
    case InputState::kEnded:
      return kMwInputEnded;
    case InputState::kFailed:
      return kMwInputFailed;
    case InputState::kStopped:
      return kMwInputStopped;
  }
  throw std::runtime_error("未知的Input状态");
}

template <std::size_t Size>
void CopyString(const std::string& source, char (&destination)[Size]) {
  if (source.size() >= Size) {
    throw std::length_error("性能节点字符串超过C API容量");
  }
  std::memcpy(destination, source.c_str(), source.size() + 1);
}

MwOperationSnapshot ConvertOperation(
    const mw::streamer::OperationSnapshot& source) {
  MwOperationSnapshot result{};
  result.type = static_cast<MwPerformanceType>(source.type);
  result.input_unit = static_cast<MwPerformanceUnit>(source.input_unit);
  result.output_unit = static_cast<MwPerformanceUnit>(source.output_unit);
  result.input_count = source.input_count;
  result.output_count = source.output_count;
  result.input_bytes = source.input_bytes;
  result.output_bytes = source.output_bytes;
  result.started_calls = source.started_calls;
  result.completed_calls = source.completed_calls;
  result.failed_calls = source.failed_calls;
  result.in_flight = source.in_flight;
  result.total_time_ns = source.total_time.count();
  result.max_time_ns = source.max_time.count();
  result.lifetime_latency.sample_count = source.lifetime_latency.sample_count;
  result.lifetime_latency.p50_us = source.lifetime_latency.p50.count();
  result.lifetime_latency.p95_us = source.lifetime_latency.p95.count();
  result.lifetime_latency.p99_us = source.lifetime_latency.p99.count();
  result.lifetime_latency.max_us = source.lifetime_latency.max.count();
  result.rates_available = source.rates_available ? 1 : 0;
  result.input_per_second = source.input_per_second;
  result.output_per_second = source.output_per_second;
  result.input_bytes_per_second = source.input_bytes_per_second;
  result.output_bytes_per_second = source.output_bytes_per_second;
  result.calls_per_second = source.calls_per_second;
  result.interval_mean_time_ns = source.interval_mean_time.count();
  return result;
}

void FlattenNode(const mw::streamer::NodeSnapshot& source,
                 std::size_t parent_index, MwPerformanceSnapshot* output) {
  if (output->node_count >= MW_STREAMER_MAX_PERFORMANCE_NODES) {
    throw std::length_error("性能节点数量超过C API容量");
  }
  if (source.operations.size() > MW_STREAMER_MAX_NODE_OPERATIONS) {
    throw std::length_error("单个性能节点的处理项超过C API容量");
  }

  const std::size_t node_index = output->node_count++;
  MwPerformanceNode& node = output->nodes[node_index];
  CopyString(source.id, node.id);
  CopyString(source.name, node.name);
  node.parent_index = parent_index;
  node.operation_count = source.operations.size();
  for (std::size_t i = 0; i < source.operations.size(); ++i) {
    node.operations[i] = ConvertOperation(source.operations[i]);
  }
  for (const auto& downstream : source.downstream) {
    FlattenNode(downstream, node_index, output);
  }
}

MwPerformanceSnapshot ConvertPerformance(
    const mw::streamer::PipelineSnapshot& source) {
  MwPerformanceSnapshot result{};
  result.pipeline_id = source.pipeline_id;
  result.sampled_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             source.sampled_at.time_since_epoch())
                             .count();
  result.interval_ns = source.interval.count();
  FlattenNode(source.input, MW_STREAMER_PERFORMANCE_NO_PARENT, &result);
  for (const auto& sink : source.sinks) {
    FlattenNode(sink, MW_STREAMER_PERFORMANCE_NO_PARENT, &result);
  }
  return result;
}

std::uint64_t Delta(std::uint64_t current, std::uint64_t previous) {
  if (current < previous) {
    throw std::invalid_argument("性能快照累计计数不能回退");
  }
  return current - previous;
}

void CalculateOperationRates(MwOperationSnapshot* current,
                             const MwOperationSnapshot& previous,
                             double seconds) {
  if (current->type != previous.type ||
      current->input_unit != previous.input_unit ||
      current->output_unit != previous.output_unit ||
      current->total_time_ns < previous.total_time_ns) {
    throw std::invalid_argument("性能快照处理项不兼容");
  }
  current->input_per_second =
      Delta(current->input_count, previous.input_count) / seconds;
  current->output_per_second =
      Delta(current->output_count, previous.output_count) / seconds;
  current->input_bytes_per_second =
      Delta(current->input_bytes, previous.input_bytes) / seconds;
  current->output_bytes_per_second =
      Delta(current->output_bytes, previous.output_bytes) / seconds;
  const std::uint64_t calls =
      Delta(current->completed_calls, previous.completed_calls);
  static_cast<void>(Delta(current->started_calls, previous.started_calls));
  static_cast<void>(Delta(current->failed_calls, previous.failed_calls));
  current->calls_per_second = calls / seconds;
  current->interval_mean_time_ns =
      calls == 0 ? 0
                 : (current->total_time_ns - previous.total_time_ns) /
                       static_cast<std::int64_t>(calls);
  current->rates_available = 1;
}

void CalculateRates(MwPerformanceSnapshot* current,
                    const MwPerformanceSnapshot& previous) {
  if (current->pipeline_id == 0 ||
      current->pipeline_id != previous.pipeline_id ||
      current->sampled_at_ns <= previous.sampled_at_ns ||
      current->node_count != previous.node_count ||
      current->node_count > MW_STREAMER_MAX_PERFORMANCE_NODES) {
    throw std::invalid_argument("性能速率需要同一Pipeline的前后两份快照");
  }
  const std::int64_t interval_ns =
      current->sampled_at_ns - previous.sampled_at_ns;
  const double seconds = static_cast<double>(interval_ns) / 1000000000.0;
  for (std::size_t i = 0; i < current->node_count; ++i) {
    MwPerformanceNode& node = current->nodes[i];
    const MwPerformanceNode& previous_node = previous.nodes[i];
    if (std::strcmp(node.id, previous_node.id) != 0 ||
        std::strcmp(node.name, previous_node.name) != 0 ||
        node.parent_index != previous_node.parent_index ||
        node.operation_count != previous_node.operation_count ||
        node.operation_count > MW_STREAMER_MAX_NODE_OPERATIONS ||
        previous_node.operation_count > MW_STREAMER_MAX_NODE_OPERATIONS) {
      throw std::invalid_argument("性能快照链路结构不兼容");
    }
    for (std::size_t j = 0; j < node.operation_count; ++j) {
      CalculateOperationRates(&node.operations[j], previous_node.operations[j],
                              seconds);
    }
  }
  current->interval_ns = interval_ns;
}

}  // namespace

extern "C" {

const char* mw_last_error(void) { return last_error.c_str(); }

MwResult mw_pipeline_create_from_toml(const MwPipelineCreateInfo* create_info,
                                      MwPipeline** output) {
  if (output != nullptr) *output = nullptr;
  bool building = false;
  try {
    ClearError();
    if (create_info == nullptr || output == nullptr ||
        create_info->toml_path == nullptr ||
        create_info->toml_path[0] == '\0') {
      throw std::invalid_argument("Pipeline创建参数不能为空");
    }
    mw::streamer::ProcessorBindings bindings;
    AddBindings(create_info->analysis_processors,
                create_info->analysis_processor_count, &bindings.analysis);
    AddBindings(create_info->transform_processors,
                create_info->transform_processor_count, &bindings.transform);
    auto handle = std::make_unique<MwPipeline>();
    building = true;
    handle->pipeline = mw::streamer::BuildPipelineFromToml(
        create_info->toml_path, bindings);
    *output = handle.release();
    return kMwResultSuccess;
  } catch (const std::bad_alloc& error) {
    return Fail(kMwResultOutOfMemory, error.what());
  } catch (const std::exception& error) {
    return Fail(building ? kMwResultConfigError : kMwResultInvalidArgument,
                error.what());
  } catch (...) {
    return Fail(kMwResultInternalError, "未知的内部错误");
  }
}

MwResult mw_pipeline_set_processor_config(MwPipeline* pipeline,
                                          const char* processor_id,
                                          const char* config) {
  return Guard([&] {
    if (pipeline == nullptr || processor_id == nullptr || config == nullptr) {
      throw std::invalid_argument("Pipeline和Processor配置参数不能为空");
    }
    pipeline->pipeline->SetProcessorConfig(processor_id, config);
  });
}

MwResult mw_pipeline_start(MwPipeline* pipeline) {
  return Guard([&] {
    if (pipeline == nullptr) throw std::invalid_argument("Pipeline不能为空");
    pipeline->pipeline->Start();
  });
}

void mw_pipeline_stop(MwPipeline* pipeline) {
  try {
    ClearError();
    if (pipeline == nullptr) {
      Fail(kMwResultInvalidArgument, "Pipeline不能为空");
      return;
    }
    pipeline->pipeline->Stop();
  } catch (...) {
    Fail(kMwResultInternalError, "停止Pipeline时发生未知错误");
  }
}

MwResult mw_pipeline_get_state(const MwPipeline* pipeline,
                               MwPipelineState* output) {
  return Guard([&] {
    if (pipeline == nullptr || output == nullptr) {
      throw std::invalid_argument("Pipeline状态参数不能为空");
    }
    *output = ConvertState(pipeline->pipeline->state());
  });
}

MwResult mw_pipeline_get_error(const MwPipeline* pipeline,
                               const char** output) {
  return Guard([&] {
    if (pipeline == nullptr || output == nullptr) {
      throw std::invalid_argument("Pipeline错误参数不能为空");
    }
    last_error = pipeline->pipeline->error();
    *output = last_error.c_str();
  });
}

MwResult mw_pipeline_get_input_status(const MwPipeline* pipeline,
                                      MwInputStatus* output) {
  try {
    ClearError();
    if (pipeline == nullptr || output == nullptr) {
      throw std::invalid_argument("Input状态参数不能为空");
    }
    const auto status = pipeline->pipeline->input_status();
    if (status.error.size() >= MW_STREAMER_MAX_ERROR_LENGTH) {
      throw std::length_error("Input错误信息超过C API容量");
    }
    MwInputStatus result{};
    result.generation = status.generation;
    result.state = ConvertInputState(status.state);
    result.will_retry = status.will_retry ? 1 : 0;
    std::memcpy(result.error, status.error.c_str(), status.error.size() + 1);
    *output = result;
    return kMwResultSuccess;
  } catch (const std::length_error& error) {
    return Fail(kMwResultCapacityExceeded, error.what());
  } catch (const std::bad_alloc& error) {
    return Fail(kMwResultOutOfMemory, error.what());
  } catch (const std::invalid_argument& error) {
    return Fail(kMwResultInvalidArgument, error.what());
  } catch (const std::exception& error) {
    return Fail(kMwResultInternalError, error.what());
  } catch (...) {
    return Fail(kMwResultInternalError, "未知的内部错误");
  }
}

MwResult mw_pipeline_get_performance(const MwPipeline* pipeline,
                                     MwPerformanceSnapshot* output) {
  try {
    ClearError();
    if (pipeline == nullptr || output == nullptr) {
      throw std::invalid_argument("性能快照参数不能为空");
    }
    const MwPerformanceSnapshot result =
        ConvertPerformance(pipeline->pipeline->GetPerformance());
    *output = result;
    return kMwResultSuccess;
  } catch (const std::length_error& error) {
    return Fail(kMwResultCapacityExceeded, error.what());
  } catch (const std::bad_alloc& error) {
    return Fail(kMwResultOutOfMemory, error.what());
  } catch (const std::invalid_argument& error) {
    return Fail(kMwResultInvalidArgument, error.what());
  } catch (const std::exception& error) {
    return Fail(kMwResultInternalError, error.what());
  } catch (...) {
    return Fail(kMwResultInternalError, "未知的内部错误");
  }
}

MwResult mw_performance_calculate_rates(MwPerformanceSnapshot* current,
                                        const MwPerformanceSnapshot* previous) {
  return Guard([&] {
    if (current == nullptr || previous == nullptr) {
      throw std::invalid_argument("性能快照不能为空");
    }
    MwPerformanceSnapshot result = *current;
    CalculateRates(&result, *previous);
    *current = result;
  });
}

void mw_pipeline_destroy(MwPipeline* pipeline) {
  try {
    ClearError();
    delete pipeline;
  } catch (...) {
  }
}

}  // extern "C"
