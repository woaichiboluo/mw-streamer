#ifndef MW_STREAMER_API_H_
#define MW_STREAMER_API_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "mw/export.h"
#include "mw/log.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/sink/frame_custom_sink.h"
#include "mw/streamer/sink/packet_custom_sink.h"

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

typedef struct MwZlmConfig {
  // Zero lets ZLToolKit use the hardware concurrency.
  size_t event_poller_threads;
  size_t work_threads;
  int enable_cpu_affinity;
} MwZlmConfig;

// Fills config with the recommended defaults. A null pointer is ignored.
MW_STREAMER_API void mw_zlm_default_config(MwZlmConfig* config);

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

typedef struct MwFrameCustomSinkBinding {
  // TOML Frame Custom Sink node ID. Borrowed only during Pipeline creation.
  const char* sink_id;
  MwStreamerFrameCustomSinkCallbacks callbacks;
} MwFrameCustomSinkBinding;

typedef struct MwPacketCustomSinkBinding {
  // TOML Packet Custom Sink node ID. Borrowed only during Pipeline creation.
  const char* sink_id;
  MwStreamerPacketCustomSinkCallbacks callbacks;
} MwPacketCustomSinkBinding;

typedef struct MwPipelineCreateInfo {
  // Path and binding arrays are borrowed only for the duration of creation.
  const char* toml_path;
  const MwAnalysisProcessorBinding* analysis_processors;
  size_t analysis_processor_count;
  const MwTransformProcessorBinding* transform_processors;
  size_t transform_processor_count;
  const MwFrameCustomSinkBinding* frame_custom_sinks;
  size_t frame_custom_sink_count;
  const MwPacketCustomSinkBinding* packet_custom_sinks;
  size_t packet_custom_sink_count;
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

// Initializes the process-wide runtime. Both configurations are required and
// copied before this call returns. Returns false and records mw_last_error if
// initialization fails or the runtime was already initialized or shut down.
MW_STREAMER_API bool mw_streamer_initialize(const MwLogConfig* log_config,
                                            const MwZlmConfig* zlm_config);

// Returns whether initialization completed and shutdown has not begun.
MW_STREAMER_API bool mw_streamer_is_initialized(void);

// Permanently closes this library runtime to new media objects after every
// Pipeline and standalone media object has been destroyed. This releases SRT
// and ZLM worker threads and, on Windows, the WinSock runtime. Logging remains
// alive until process teardown, so this is not a dynamic-library unload
// barrier. Calling before initialization or after successful shutdown is
// harmless. A rejected shutdown leaves the runtime initialized and records
// mw_last_error.
MW_STREAMER_API void mw_streamer_shutdown(void);

// Loads TOML, copies callback tables into the corresponding node maps and
// constructs an idle Pipeline. On failure, *output is set to NULL. Each
// callback user_context remains caller-owned and must outlive Stop.
MW_STREAMER_API MwResult mw_pipeline_create_from_toml(
    const MwPipelineCreateInfo* create_info, MwPipeline** output);

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
