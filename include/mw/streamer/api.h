#ifndef MW_STREAMER_API_H_
#define MW_STREAMER_API_H_

#include <stddef.h>
#include <stdint.h>

#include "mw/export.h"
#include "mw/streamer/processor/processor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MW_STREAMER_MAX_ERROR_LENGTH 512
#define MW_STREAMER_MAX_PERFORMANCE_NODES 64
#define MW_STREAMER_MAX_NODE_OPERATIONS 2
#define MW_STREAMER_MAX_PERFORMANCE_ID_LENGTH 128
#define MW_STREAMER_MAX_PERFORMANCE_NAME_LENGTH 64
#define MW_STREAMER_PERFORMANCE_NO_PARENT ((size_t)-1)

// Opaque handle. Pipeline operations except mw_pipeline_destroy require a
// non-null handle. A handle must not be used concurrently with or after
// destruction.
typedef struct MwPipeline MwPipeline;

typedef enum MwResult {
  kMwResultSuccess = 0,
  kMwResultConfigError,
  kMwResultInvalidArgument,
  kMwResultInvalidState,
  kMwResultCapacityExceeded,
  kMwResultOutOfMemory,
  kMwResultInternalError,
} MwResult;

typedef enum MwPipelineState {
  kMwPipelineIdle = 0,
  kMwPipelineRunning,
  kMwPipelineStopping,
  kMwPipelineStopped,
  kMwPipelineFailed,
} MwPipelineState;

typedef enum MwInputState {
  kMwInputIdle = 0,
  kMwInputConnecting,
  kMwInputReady,
  kMwInputWaitingRetry,
  kMwInputEnded,
  kMwInputFailed,
  kMwInputStopped,
} MwInputState;

typedef struct MwInputStatus {
  uint64_t generation;
  MwInputState state;
  uint8_t will_retry;
  char error[MW_STREAMER_MAX_ERROR_LENGTH];
} MwInputStatus;

typedef struct MwAnalysisProcessorBinding {
  // TOML Processor node ID. Borrowed only during Pipeline creation.
  const char* processor_id;
  MwStreamerAnalysisProcessorCallbacks callbacks;
} MwAnalysisProcessorBinding;

typedef struct MwTransformProcessorBinding {
  // TOML Processor node ID. Borrowed only during Pipeline creation.
  const char* processor_id;
  MwStreamerTransformProcessorCallbacks callbacks;
} MwTransformProcessorBinding;

typedef struct MwPipelineCreateInfo {
  // Path and binding arrays are borrowed only for the duration of creation.
  const char* toml_path;
  const MwAnalysisProcessorBinding* analysis_processors;
  size_t analysis_processor_count;
  const MwTransformProcessorBinding* transform_processors;
  size_t transform_processor_count;
} MwPipelineCreateInfo;

typedef enum MwPerformanceType {
  kMwPerformanceInput = 0,
  kMwPerformanceAudioDecoder,
  kMwPerformanceVideoDecoder,
  kMwPerformanceAudioProcessor,
  kMwPerformanceVideoProcessor,
  kMwPerformanceSynchronizer,
  kMwPerformanceAudioEncoder,
  kMwPerformanceVideoEncoder,
  kMwPerformanceRemux,
} MwPerformanceType;

typedef enum MwPerformanceUnit {
  kMwPerformanceUnitNone = 0,
  kMwPerformanceUnitPacket,
  kMwPerformanceUnitFrame,
  kMwPerformanceUnitSample,
} MwPerformanceUnit;

typedef struct MwLatencySnapshot {
  uint64_t sample_count;
  int64_t p50_us;
  int64_t p95_us;
  int64_t p99_us;
  int64_t max_us;
} MwLatencySnapshot;

typedef struct MwOperationSnapshot {
  MwPerformanceType type;
  MwPerformanceUnit input_unit;
  MwPerformanceUnit output_unit;
  uint64_t input_count;
  uint64_t output_count;
  uint64_t input_bytes;
  uint64_t output_bytes;
  uint64_t started_calls;
  uint64_t completed_calls;
  uint64_t failed_calls;
  uint64_t in_flight;
  int64_t total_time_ns;
  int64_t max_time_ns;
  // Percentiles are cumulative over the operation lifetime, including in a
  // snapshot whose interval rates have been calculated.
  MwLatencySnapshot lifetime_latency;
  uint8_t rates_available;
  double input_per_second;
  double output_per_second;
  double input_bytes_per_second;
  double output_bytes_per_second;
  double calls_per_second;
  int64_t interval_mean_time_ns;
} MwOperationSnapshot;

typedef struct MwPerformanceNode {
  // IDs are stable paths used for selection. Names are descriptive only.
  char id[MW_STREAMER_MAX_PERFORMANCE_ID_LENGTH];
  char name[MW_STREAMER_MAX_PERFORMANCE_NAME_LENGTH];
  // Root nodes use MW_STREAMER_PERFORMANCE_NO_PARENT.
  size_t parent_index;
  size_t operation_count;
  MwOperationSnapshot operations[MW_STREAMER_MAX_NODE_OPERATIONS];
} MwPerformanceNode;

typedef struct MwPerformanceSnapshot {
  uint64_t pipeline_id;
  // steady-clock values are comparable only within the current process.
  int64_t sampled_at_ns;
  // Zero for a raw snapshot; nonzero after rate calculation.
  int64_t interval_ns;
  size_t node_count;
  MwPerformanceNode nodes[MW_STREAMER_MAX_PERFORMANCE_NODES];
} MwPerformanceSnapshot;

// Returns the error from the latest failing C API call on this thread. The
// library owns the string until the next C API call on the same thread.
MW_STREAMER_API const char* mw_last_error(void);

// Loads TOML, copies callback tables into the corresponding Processor maps and
// constructs an idle Pipeline. On failure, *output is set to NULL. Each
// callback user_context remains caller-owned and must outlive Stop.
MW_STREAMER_API MwResult mw_pipeline_create_from_toml(
    const MwPipelineCreateInfo* create_info, MwPipeline** output);

// Before Start, replaces the initial Processor config. While running, invokes
// its update callback and may run concurrently with media callbacks. Strings
// are borrowed only for this call.
MW_STREAMER_API MwResult mw_pipeline_set_processor_config(
    MwPipeline* pipeline, const char* processor_id, const char* config);

// A Pipeline permits one start attempt. Synchronous failures are returned;
// later failures are reported by state and error queries.
MW_STREAMER_API MwResult mw_pipeline_start(MwPipeline* pipeline);

// Idempotently stops the Pipeline and waits for in-flight callbacks. Do not
// call from an input, sink or Processor callback.
MW_STREAMER_API void mw_pipeline_stop(MwPipeline* pipeline);

MW_STREAMER_API MwResult mw_pipeline_get_state(const MwPipeline* pipeline,
                                               MwPipelineState* output);

// The returned string is owned by the library until the next C API call on the
// same thread. An empty string means the Pipeline has no persistent failure.
MW_STREAMER_API MwResult mw_pipeline_get_error(const MwPipeline* pipeline,
                                               const char** output);

// Writes a thread-safe input-state snapshot into caller-owned storage. An
// overlong error returns kMwResultCapacityExceeded without truncation.
MW_STREAMER_API MwResult
mw_pipeline_get_input_status(const MwPipeline* pipeline, MwInputStatus* output);

// Writes a complete preorder performance tree into caller-owned inline
// storage. Capacity or string overflow returns kMwResultCapacityExceeded; no
// partial snapshot is returned. Safe during Start, Stop and media delivery.
MW_STREAMER_API MwResult mw_pipeline_get_performance(
    const MwPipeline* pipeline, MwPerformanceSnapshot* output);

// Calculates interval rates in current without allocation. Both snapshots
// must originate from the same Pipeline, have identical topology and operation
// types, and current must be newer with nondecreasing cumulative counters.
MW_STREAMER_API MwResult mw_performance_calculate_rates(
    MwPerformanceSnapshot* current, const MwPerformanceSnapshot* previous);

// Stops and destroys the Pipeline. Accepts NULL. Do not call from an input,
// sink or Processor callback.
MW_STREAMER_API void mw_pipeline_destroy(MwPipeline* pipeline);

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_API_H_
