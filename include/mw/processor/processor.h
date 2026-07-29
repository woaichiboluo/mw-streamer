#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MwStreamerRational {
  int32_t num;
  int32_t den;
} MwStreamerRational;

typedef struct MwStreamerMediaTimestamp {
  int64_t pts;
  int64_t duration;
  MwStreamerRational time_base;
} MwStreamerMediaTimestamp;

typedef enum MwStreamerCodec {
  kMwStreamerCodecUnknown = 0,
  kMwStreamerCodecH264,
  kMwStreamerCodecH265,
  kMwStreamerCodecAv1,
  kMwStreamerCodecAac,
  kMwStreamerCodecG711A,
  kMwStreamerCodecG711U,
  kMwStreamerCodecOpus,
  kMwStreamerCodecMjpeg,
  kMwStreamerCodecVp8,
  kMwStreamerCodecVp9,
} MwStreamerCodec;

typedef enum MwStreamerMemoryType {
  kMwStreamerMemoryHost = 0,
  kMwStreamerMemoryCuda,
} MwStreamerMemoryType;

typedef enum MwStreamerVideoPixelFormat {
  kMwStreamerVideoPixelFormatUnknown = 0,
  kMwStreamerVideoPixelFormatNv12,
  kMwStreamerVideoPixelFormatP010,
  kMwStreamerVideoPixelFormatYuv420p,
} MwStreamerVideoPixelFormat;

typedef enum MwStreamerExecutionType {
  kMwStreamerExecutionCpu = 0,
  kMwStreamerExecutionCuda,
} MwStreamerExecutionType;

typedef enum MwStreamerProcessorStartResult {
  kMwStreamerProcessorStartSuccess = 0,
  kMwStreamerProcessorStartFailed,
} MwStreamerProcessorStartResult;

// The native handle is backend-specific and borrowed for one process callback.
// It is zero for synchronous CPU execution and a cudaStream_t-compatible value
// for CUDA execution.
typedef struct MwStreamerExecutionContext {
  MwStreamerExecutionType type;
  uintptr_t native_handle;
} MwStreamerExecutionContext;

typedef struct MwStreamerVideoPlaneView {
  uintptr_t address;
  int32_t stride;
} MwStreamerVideoPlaneView;

// Plane descriptors and their payload are borrowed for one process callback.
typedef struct MwStreamerVideoBufferView {
  MwStreamerMemoryType memory_type;
  MwStreamerVideoPixelFormat pixel_format;
  uint32_t width;
  uint32_t height;
  const MwStreamerVideoPlaneView* planes;
  uint32_t plane_count;
} MwStreamerVideoBufferView;

typedef struct MwStreamerVideoFrameView {
  MwStreamerVideoBufferView buffer;
  MwStreamerMediaTimestamp timestamp;
} MwStreamerVideoFrameView;

typedef struct MwStreamerAudioFrameView {
  const float* data;
  uint32_t sample_rate;
  uint32_t channel_count;
  uint32_t samples_per_channel;
  MwStreamerMediaTimestamp timestamp;
} MwStreamerAudioFrameView;

typedef struct MwStreamerAudioBufferView {
  float* data;
  uint32_t channel_count;
  uint32_t samples_per_channel;
} MwStreamerAudioBufferView;

typedef struct MwStreamerVideoSourceInfo {
  MwStreamerCodec codec;
  uint32_t width;
  uint32_t height;
  MwStreamerVideoPixelFormat pixel_format;
  MwStreamerRational frame_rate;
  MwStreamerRational time_base;
} MwStreamerVideoSourceInfo;

typedef struct MwStreamerAudioSourceInfo {
  MwStreamerCodec codec;
  uint32_t sample_rate;
  uint32_t channel_count;
  MwStreamerRational time_base;
} MwStreamerAudioSourceInfo;

typedef struct MwStreamerProcessorSourceInfo {
  uint8_t has_video;
  uint8_t has_audio;
  MwStreamerVideoSourceInfo video;
  MwStreamerAudioSourceInfo audio;
} MwStreamerProcessorSourceInfo;

// output_width and output_height are fixed for the Processor lifetime and are
// zero when no video stream exists. config is a null-terminated opaque user
// string; the Pipeline copies it during creation and updates only that field
// while running.
typedef struct MwStreamerProcessorConfig {
  uint32_t output_width;
  uint32_t output_height;
  const char* config;
} MwStreamerProcessorConfig;

typedef struct MwStreamerVideoProcessRequest {
  const MwStreamerVideoFrameView* input;
  // The framework attaches input->timestamp to the completed output frame.
  MwStreamerVideoBufferView output;
  const MwStreamerExecutionContext* execution;
} MwStreamerVideoProcessRequest;

typedef struct MwStreamerAudioProcessRequest {
  const MwStreamerAudioFrameView* input;
  // The framework attaches input->timestamp to the completed output frame.
  MwStreamerAudioBufferView output;
} MwStreamerAudioProcessRequest;

typedef struct MwStreamerProcessorStartRequest {
  const MwStreamerProcessorSourceInfo* source_info;
  const MwStreamerProcessorConfig* config;
  const MwStreamerExecutionContext* execution;
} MwStreamerProcessorStartRequest;

// A failed callback must release any partially initialized user resources
// before returning. on_stop is paired only with a successful on_start.
typedef MwStreamerProcessorStartResult (*MwStreamerProcessorStartCallback)(
    const MwStreamerProcessorStartRequest* request, void* user_context);

// Every callback must completely produce one output for one input. CPU output
// is ready when the callback returns. For asynchronous backends, the final
// output write must have been submitted to the supplied execution sequence.
typedef void (*MwStreamerProcessVideoCallback)(
    const MwStreamerVideoProcessRequest* request, void* user_context);

// Audio presented to Processor is always 48 kHz float32 interleaved. The output
// has the same channel count and samples_per_channel as the input.
typedef void (*MwStreamerProcessAudioCallback)(
    const MwStreamerAudioProcessRequest* request, void* user_context);

typedef void (*MwStreamerProcessorUpdateConfigCallback)(const char* config,
                                                        void* user_context);

typedef void (*MwStreamerProcessorStopCallback)(void* user_context);

typedef struct MwStreamerProcessorCallbacks {
  // Borrowed user data returned unchanged to every callback. The framework
  // never reads or releases it.
  void* user_context;

  // on_start receives source information and the initial config before the
  // first process callback. All request views are borrowed for the callback.
  MwStreamerProcessorStartCallback on_start;
  MwStreamerProcessVideoCallback process_video;
  MwStreamerProcessAudioCallback process_audio;

  // Runtime updates are serialized with audio and video processing. The
  // null-terminated string is borrowed for the callback; user code must copy
  // data it needs after returning.
  MwStreamerProcessorUpdateConfigCallback update_config;

  // Called once after a successful on_start, after all process callbacks and
  // their submitted output work have completed.
  MwStreamerProcessorStopCallback on_stop;
} MwStreamerProcessorCallbacks;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_H_
