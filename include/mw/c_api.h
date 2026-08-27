#ifndef MW_STREAMER_INCLUDE_MW_C_API_H_
#define MW_STREAMER_INCLUDE_MW_C_API_H_

#include <stddef.h>
#include <stdint.h>

#include "mw/processor/processor.h"

#if defined(_WIN32)
#if defined(MW_STREAMER_STATIC_LIBRARY)
#define MW_STREAMER_API
#elif defined(MW_STREAMER_BUILDING_LIBRARY)
#define MW_STREAMER_API __declspec(dllexport)
#else
#define MW_STREAMER_API __declspec(dllimport)
#endif
#elif defined(MW_STREAMER_BUILDING_LIBRARY)
#define MW_STREAMER_API __attribute__((visibility("default")))
#else
#define MW_STREAMER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MwResult {
  kMwResultSuccess = 0,
  kMwResultConfigError,
  kMwResultInvalidArgument,
  kMwResultInvalidState,
  kMwResultOutOfMemory,
  kMwResultInternalError,
  kMwResultQueueFull,
} MwResult;

typedef enum MwPipelineStatus {
  kMwPipelineStatusIdle = 0,
  kMwPipelineStatusStarting,
  kMwPipelineStatusRunning,
  kMwPipelineStatusFailed,
  kMwPipelineStatusStopped,
} MwPipelineStatus;

typedef struct MwStreaming MwStreaming;
typedef struct MwRemux MwRemux;
typedef struct MwFile MwFile;

// One independently scheduled raw output flow. The callback table is copied
// during registration; user_context remains caller-owned and must stay valid
// until stop returns. All callbacks are serialized on this Sink's dedicated
// thread. Frame views and their storage are borrowed for the callback duration.
typedef struct MwStreamerOutputSinkCallbacks {
  void* user_context;
  void (*start)(void* user_context);
  void (*write_video)(const MwStreamerVideoFrameView* frame,
                      void* user_context);
  void (*write_audio)(const MwStreamerAudioFrameView* frame,
                      void* user_context);
  void (*stop)(void* user_context);
} MwStreamerOutputSinkCallbacks;

// Status callbacks run synchronously on the thread that changes the state.
// They must return promptly and must not call control or destroy functions.
typedef void (*MwStatusCallback)(MwPipelineStatus status, void* user_context);

typedef struct MwLatencyStats {
  uint64_t sample_count;
  int64_t p50_us;
  int64_t p95_us;
  int64_t p99_us;
  int64_t max_us;
} MwLatencyStats;

typedef struct MwVideoStageStats {
  uint64_t frames;
  double frames_per_second;
  MwLatencyStats latency;
} MwVideoStageStats;

typedef struct MwAudioStageStats {
  uint64_t samples;
  double samples_per_second;
  MwLatencyStats latency;
} MwAudioStageStats;

typedef struct MwNetworkInputStats {
  uint8_t is_network;
  uint8_t connected;
  uint64_t generation;
  uint64_t reconnect_count;
  uint64_t received_bytes;
} MwNetworkInputStats;

typedef struct MwNetworkOutputStats {
  // Owned by the enclosing stats object and valid until its destroy function
  // is called.
  const char* target;
  uint8_t connected;
  uint64_t reconnect_count;
  uint64_t sent_bytes;
} MwNetworkOutputStats;

typedef struct MwStreamingVideoStats {
  MwVideoStageStats decode;
  MwVideoStageStats process;
  MwVideoStageStats encode;
  uint64_t dropped_packets;
  size_t queue_depth;
} MwStreamingVideoStats;

typedef struct MwStreamingAudioStats {
  MwAudioStageStats decode;
  MwAudioStageStats process;
  MwAudioStageStats encode;
  uint64_t dropped_packets;
  size_t queue_depth;
} MwStreamingAudioStats;

typedef struct MwStreamingStats {
  int64_t interval_ns;
  MwNetworkInputStats input;
  size_t output_count;
  MwNetworkOutputStats* outputs;
  uint8_t has_video;
  MwStreamingVideoStats video;
  uint8_t has_audio;
  MwStreamingAudioStats audio;
  size_t output_queue_depth;
} MwStreamingStats;

typedef struct MwRemuxStats {
  int64_t interval_ns;
  MwNetworkInputStats input;
  size_t output_count;
  MwNetworkOutputStats* outputs;
  uint64_t packets;
  uint64_t bytes;
  double bits_per_second;
  size_t output_queue_depth;
} MwRemuxStats;

typedef struct MwFileVideoStats {
  MwVideoStageStats decode;
  MwVideoStageStats process;
} MwFileVideoStats;

typedef struct MwFileAudioStats {
  MwAudioStageStats decode;
  MwAudioStageStats process;
} MwFileAudioStats;

typedef struct MwFileStats {
  int64_t interval_ns;
  uint8_t progress_available;
  int64_t processed_position_us;
  int64_t duration_us;
  double progress;
  uint8_t processing_speed_available;
  double processing_speed;
  uint8_t has_video;
  MwFileVideoStats video;
  uint8_t has_audio;
  MwFileAudioStats audio;
} MwFileStats;

// Returned text belongs to the library and remains valid until the next C API
// call on the same thread. It is empty when that call succeeded.
MW_STREAMER_API const char* mw_last_error(void);

// A null config_path uses the default process configuration. Otherwise the
// configuration is loaded from TOML. Initialization remains one-shot, matching
// the C++ API. All handles must be destroyed before mw_shutdown().
MW_STREAMER_API MwResult mw_init(const char* config_path);
MW_STREAMER_API void mw_shutdown(void);

// config_path is borrowed only for the create call. Each handle has one owner
// control thread. Processor and status callback contexts remain owned by the
// caller and must outlive the handle's final callback.
MW_STREAMER_API MwResult mw_streaming_create(const char* config_path,
                                             MwStreaming** output);
MW_STREAMER_API MwResult mw_streaming_on_status(MwStreaming* streaming,
                                                MwStatusCallback callback,
                                                void* user_context);
MW_STREAMER_API MwResult mw_streaming_set_processor(
    MwStreaming* streaming,
    const MwStreamerStreamingProcessorCallbacks* callbacks);
// Registers one independent Sink before start. sink_id must be non-empty and
// unique. Callback function pointers may be null.
MW_STREAMER_API MwResult
mw_streaming_add_output_sink(MwStreaming* streaming, const char* sink_id,
                             const MwStreamerOutputSinkCallbacks* callbacks);
// Start only reports whether the asynchronous request was accepted. Runtime
// failures are reported through kMwPipelineStatusFailed.
MW_STREAMER_API MwResult mw_streaming_start(MwStreaming* streaming);
// Reloads the TOML file supplied to create and applies only processor.config.
// Parsing failures leave the current Processor configuration unchanged.
MW_STREAMER_API MwResult mw_streaming_reload(MwStreaming* streaming);
// Deep-copies and asynchronously delivers one event to the Processor. Success
// means accepted by the mailbox, not handled by the Processor.
MW_STREAMER_API MwResult mw_streaming_submit_output_event(
    MwStreaming* streaming, const MwStreamerOutputEvent* event);
MW_STREAMER_API MwResult mw_streaming_status(const MwStreaming* streaming,
                                             MwPipelineStatus* output);
// A successful collection allocates an independent snapshot. The caller must
// release it with the matching destroy function.
MW_STREAMER_API MwResult mw_streaming_stats(MwStreaming* streaming,
                                            MwStreamingStats** output);
MW_STREAMER_API void mw_streaming_stats_destroy(MwStreamingStats* stats);
MW_STREAMER_API void mw_streaming_stop(MwStreaming* streaming);
// Destroy stops the Pipeline before releasing it and accepts null.
MW_STREAMER_API void mw_streaming_destroy(MwStreaming* streaming);

MW_STREAMER_API MwResult mw_remux_create(const char* config_path,
                                         MwRemux** output);
MW_STREAMER_API MwResult mw_remux_on_status(MwRemux* remux,
                                            MwStatusCallback callback,
                                            void* user_context);
MW_STREAMER_API MwResult mw_remux_start(MwRemux* remux);
MW_STREAMER_API MwResult mw_remux_status(const MwRemux* remux,
                                         MwPipelineStatus* output);
// A successful collection allocates an independent snapshot. The caller must
// release it with the matching destroy function.
MW_STREAMER_API MwResult mw_remux_stats(MwRemux* remux, MwRemuxStats** output);
MW_STREAMER_API void mw_remux_stats_destroy(MwRemuxStats* stats);
MW_STREAMER_API void mw_remux_stop(MwRemux* remux);
// Destroy stops the Pipeline before releasing it and accepts null.
MW_STREAMER_API void mw_remux_destroy(MwRemux* remux);

MW_STREAMER_API MwResult mw_file_create(const char* config_path,
                                        MwFile** output);
MW_STREAMER_API MwResult mw_file_on_status(MwFile* file,
                                           MwStatusCallback callback,
                                           void* user_context);
MW_STREAMER_API MwResult mw_file_set_processor(
    MwFile* file, const MwStreamerFileProcessorCallbacks* callbacks);
MW_STREAMER_API MwResult mw_file_start(MwFile* file);
// Reloads the TOML file supplied to create and applies only processor.config.
// Parsing failures leave the current Processor configuration unchanged.
MW_STREAMER_API MwResult mw_file_reload(MwFile* file);
MW_STREAMER_API MwResult mw_file_status(const MwFile* file,
                                        MwPipelineStatus* output);
// A successful collection allocates an independent snapshot. The caller must
// release it with the matching destroy function.
MW_STREAMER_API MwResult mw_file_stats(MwFile* file, MwFileStats** output);
MW_STREAMER_API void mw_file_stats_destroy(MwFileStats* stats);
MW_STREAMER_API void mw_file_stop(MwFile* file);
// Destroy stops the Pipeline before releasing it and accepts null.
MW_STREAMER_API void mw_file_destroy(MwFile* file);

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_INCLUDE_MW_C_API_H_
