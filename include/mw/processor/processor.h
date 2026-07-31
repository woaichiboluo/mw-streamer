#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_H_

#include <stdint.h>

#include "mw/media/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MwStreamerMediaTimestamp {
  int64_t pts;
  int64_t duration;
  MwStreamerRational time_base;
} MwStreamerMediaTimestamp;

typedef enum MwStreamerMemoryType {
  kMwStreamerMemoryHost = 0,
  kMwStreamerMemoryCuda,
} MwStreamerMemoryType;

typedef enum MwStreamerVideoStorageType {
  kMwStreamerVideoStorageLinear = 0,
  kMwStreamerVideoStorageNativeSurface,
} MwStreamerVideoStorageType;

typedef enum MwStreamerVideoPixelFormat {
  kMwStreamerVideoPixelFormatUnknown = 0,
  kMwStreamerVideoPixelFormatNv12,
  kMwStreamerVideoPixelFormatP010,
  kMwStreamerVideoPixelFormatYuv420p,
  kMwStreamerVideoPixelFormatYuv422p,
  kMwStreamerVideoPixelFormatYuv444p,
  kMwStreamerVideoPixelFormatYuv420p10le,
  kMwStreamerVideoPixelFormatYuv422p10le,
  kMwStreamerVideoPixelFormatYuv444p10le,
} MwStreamerVideoPixelFormat;

typedef enum MwStreamerColorRange {
  kMwStreamerColorRangeUnknown = 0,
  kMwStreamerColorRangeLimited,
  kMwStreamerColorRangeFull,
} MwStreamerColorRange;

typedef enum MwStreamerColorSpace {
  kMwStreamerColorSpaceUnknown = 0,
  kMwStreamerColorSpaceRgb,
  kMwStreamerColorSpaceBt709,
  kMwStreamerColorSpaceFcc,
  kMwStreamerColorSpaceBt470bg,
  kMwStreamerColorSpaceSmpte170m,
  kMwStreamerColorSpaceSmpte240m,
  kMwStreamerColorSpaceYcgco,
  kMwStreamerColorSpaceBt2020Ncl,
  kMwStreamerColorSpaceBt2020Cl,
  kMwStreamerColorSpaceSmpte2085,
  kMwStreamerColorSpaceChromaDerivedNcl,
  kMwStreamerColorSpaceChromaDerivedCl,
  kMwStreamerColorSpaceIctcp,
  kMwStreamerColorSpaceIptC2,
  kMwStreamerColorSpaceYcgcoRe,
  kMwStreamerColorSpaceYcgcoRo,
} MwStreamerColorSpace;

typedef enum MwStreamerColorPrimaries {
  kMwStreamerColorPrimariesUnknown = 0,
  kMwStreamerColorPrimariesBt709,
  kMwStreamerColorPrimariesBt470m,
  kMwStreamerColorPrimariesBt470bg,
  kMwStreamerColorPrimariesSmpte170m,
  kMwStreamerColorPrimariesSmpte240m,
  kMwStreamerColorPrimariesFilm,
  kMwStreamerColorPrimariesBt2020,
  kMwStreamerColorPrimariesSmpte428,
  kMwStreamerColorPrimariesSmpte431,
  kMwStreamerColorPrimariesSmpte432,
  kMwStreamerColorPrimariesEbu3213,
} MwStreamerColorPrimaries;

typedef enum MwStreamerColorTransfer {
  kMwStreamerColorTransferUnknown = 0,
  kMwStreamerColorTransferBt709,
  kMwStreamerColorTransferGamma22,
  kMwStreamerColorTransferGamma28,
  kMwStreamerColorTransferSmpte170m,
  kMwStreamerColorTransferSmpte240m,
  kMwStreamerColorTransferLinear,
  kMwStreamerColorTransferLog,
  kMwStreamerColorTransferLogSqrt,
  kMwStreamerColorTransferIec61966_2_4,
  kMwStreamerColorTransferBt1361Ecg,
  kMwStreamerColorTransferIec61966_2_1,
  kMwStreamerColorTransferBt2020_10,
  kMwStreamerColorTransferBt2020_12,
  kMwStreamerColorTransferSmpte2084,
  kMwStreamerColorTransferSmpte428,
  kMwStreamerColorTransferAribStdB67,
} MwStreamerColorTransfer;

typedef enum MwStreamerChromaLocation {
  kMwStreamerChromaLocationUnknown = 0,
  kMwStreamerChromaLocationLeft,
  kMwStreamerChromaLocationCenter,
  kMwStreamerChromaLocationTopLeft,
  kMwStreamerChromaLocationTop,
  kMwStreamerChromaLocationBottomLeft,
  kMwStreamerChromaLocationBottom,
} MwStreamerChromaLocation;

typedef enum MwStreamerExecutionType {
  kMwStreamerExecutionCpu = 0,
  kMwStreamerExecutionCuda,
} MwStreamerExecutionType;

typedef enum MwStreamerProcessorStartResult {
  kMwStreamerProcessorStartSuccess = 0,
  kMwStreamerProcessorStartFailed,
} MwStreamerProcessorStartResult;

typedef enum MwStreamerProcessorBoundaryReason {
  kMwStreamerProcessorTimelineReset = 0,
  kMwStreamerProcessorEndOfInput,
} MwStreamerProcessorBoundaryReason;

// The native handle is backend-specific and borrowed for the Processor
// lifetime. It is zero for synchronous CPU execution and a
// cudaStream_t-compatible value for CUDA execution.
typedef struct MwStreamerExecutionContext {
  MwStreamerExecutionType type;
  uintptr_t native_handle;
} MwStreamerExecutionContext;

typedef struct MwStreamerVideoPlaneView {
  uintptr_t address;
  // Byte distance between adjacent rows. It may be negative for host frames.
  int32_t stride_bytes;
  // Valid payload bytes in one row, excluding stride padding.
  uint32_t row_bytes;
  // Number of valid rows in this plane.
  uint32_t row_count;
} MwStreamerVideoPlaneView;

// Host and CUDA frames currently use addressable linear planes.
typedef struct MwStreamerLinearVideoStorageView {
  const MwStreamerVideoPlaneView* planes;
  uint32_t plane_count;
} MwStreamerLinearVideoStorageView;

// Native surfaces are backend-specific and borrowed for one process callback.
// descriptor is null when handle and subresource_index fully describe the
// surface.
typedef struct MwStreamerNativeVideoStorageView {
  uintptr_t handle;
  uint32_t subresource_index;
  const void* descriptor;
} MwStreamerNativeVideoStorageView;

typedef union MwStreamerVideoStorageView {
  MwStreamerLinearVideoStorageView linear;
  MwStreamerNativeVideoStorageView native_surface;
} MwStreamerVideoStorageView;

// The storage descriptor and its payload are borrowed for one process
// callback. Linear input addresses are read-only by contract; linear output
// addresses refer to writable framework-owned storage. storage_type selects
// the active member of storage.
typedef struct MwStreamerVideoBufferView {
  MwStreamerMemoryType memory_type;
  MwStreamerVideoStorageType storage_type;
  MwStreamerVideoPixelFormat pixel_format;
  uint32_t width;
  uint32_t height;
  MwStreamerVideoStorageView storage;
} MwStreamerVideoBufferView;

typedef struct MwStreamerVideoColorInfo {
  MwStreamerColorRange range;
  MwStreamerColorSpace space;
  MwStreamerColorPrimaries primaries;
  MwStreamerColorTransfer transfer;
  MwStreamerChromaLocation chroma_location;
} MwStreamerVideoColorInfo;

// Color metadata and decoded storage properties describe this concrete frame,
// independently of the source-reported track information.
typedef struct MwStreamerVideoFrameView {
  MwStreamerVideoBufferView buffer;
  MwStreamerVideoColorInfo color;
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
  // Source-reported dimensions; concrete decoded dimensions belong to each
  // MwStreamerVideoFrameView.
  uint32_t width;
  uint32_t height;
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

// Output dimensions are fixed for the Streaming Processor lifetime. They are
// zero when the stream has no video. config is a null-terminated opaque user
// string; the Pipeline copies it during creation and updates only that field
// while running.
typedef struct MwStreamerStreamingProcessorConfig {
  uint32_t output_width;
  uint32_t output_height;
  const char* config;
} MwStreamerStreamingProcessorConfig;

typedef struct MwStreamerFileProcessorConfig {
  const char* config;
} MwStreamerFileProcessorConfig;

typedef struct MwStreamerStreamingVideoProcessRequest {
  const MwStreamerVideoFrameView* input;
  // The framework attaches input->timestamp to the completed output frame.
  MwStreamerVideoBufferView* output;
} MwStreamerStreamingVideoProcessRequest;

typedef struct MwStreamerStreamingAudioProcessRequest {
  const MwStreamerAudioFrameView* input;
  // The framework attaches input->timestamp to the completed output frame.
  MwStreamerAudioBufferView* output;
} MwStreamerStreamingAudioProcessRequest;

typedef struct MwStreamerStreamingProcessorStartRequest {
  const MwStreamerProcessorSourceInfo* source_info;
  const MwStreamerStreamingProcessorConfig* config;
  const MwStreamerExecutionContext* execution;
} MwStreamerStreamingProcessorStartRequest;

typedef struct MwStreamerFileProcessorStartRequest {
  const MwStreamerProcessorSourceInfo* source_info;
  const MwStreamerFileProcessorConfig* config;
  const MwStreamerExecutionContext* execution;
} MwStreamerFileProcessorStartRequest;

// A failed callback must release any partially initialized user resources
// before returning. on_stop is paired only with a successful on_start.
typedef MwStreamerProcessorStartResult (
    *MwStreamerStreamingProcessorStartCallback)(
    const MwStreamerStreamingProcessorStartRequest* request,
    void* user_context);

typedef MwStreamerProcessorStartResult (*MwStreamerFileProcessorStartCallback)(
    const MwStreamerFileProcessorStartRequest* request, void* user_context);

// Every Streaming callback must completely produce one output for one input.
// CPU output is ready when the callback returns. For asynchronous backends,
// the final output write must have been submitted to the supplied execution
// sequence.
typedef void (*MwStreamerStreamingProcessVideoCallback)(
    const MwStreamerStreamingVideoProcessRequest* request, void* user_context);

// Audio presented to Streaming Processor is always 48 kHz float32 interleaved.
// The output has the same channel count and samples_per_channel as the input.
typedef void (*MwStreamerStreamingProcessAudioCallback)(
    const MwStreamerStreamingAudioProcessRequest* request, void* user_context);

// File callbacks consume decoded input without allocating or producing an
// output media frame.
typedef void (*MwStreamerFileProcessVideoCallback)(
    const MwStreamerVideoFrameView* input, void* user_context);
typedef void (*MwStreamerFileProcessAudioCallback)(
    const MwStreamerAudioFrameView* input, void* user_context);

// Optional input boundary notification. Timeline reset is called after all
// work from the old timeline and before the first callback of the new
// timeline; discard temporal state and incomplete batches. End of input is
// called after the last process callback; complete any partial final batch.
typedef void (*MwStreamerProcessorBoundaryCallback)(
    MwStreamerProcessorBoundaryReason reason, void* user_context);

typedef void (*MwStreamerProcessorUpdateConfigCallback)(const char* config,
                                                        void* user_context);

typedef void (*MwStreamerProcessorStopCallback)(void* user_context);

typedef struct MwStreamerStreamingProcessorCallbacks {
  // Borrowed user data returned unchanged to every callback. The framework
  // never reads or releases it.
  void* user_context;

  // on_start receives source information, the fixed execution context, and the
  // initial config before the first process callback. User code that needs the
  // backend stream must copy it into user_context here. All request views are
  // borrowed for the callback.
  MwStreamerStreamingProcessorStartCallback on_start;
  MwStreamerStreamingProcessVideoCallback process_video;
  MwStreamerStreamingProcessAudioCallback process_audio;

  // Optional. A boundary is emitted once for the whole Processor, not once per
  // audio or video stream.
  MwStreamerProcessorBoundaryCallback on_boundary;

  // Runtime updates originate from the Pipeline control thread and may run
  // concurrently with audio and video processing. The null-terminated string
  // is borrowed for the callback; user code must copy data it needs after
  // returning and synchronize access to its own runtime state.
  MwStreamerProcessorUpdateConfigCallback update_config;

  // Called once after a successful on_start, after all process callbacks and
  // their submitted output work have completed. Exceptions are logged and
  // suppressed by the framework.
  MwStreamerProcessorStopCallback on_stop;
} MwStreamerStreamingProcessorCallbacks;

typedef struct MwStreamerFileProcessorCallbacks {
  // Borrowed user data returned unchanged to every callback. The framework
  // never reads or releases it.
  void* user_context;

  MwStreamerFileProcessorStartCallback on_start;
  MwStreamerFileProcessVideoCallback process_video;
  MwStreamerFileProcessAudioCallback process_audio;

  // Optional. A boundary is emitted once for the whole Processor, not once per
  // audio or video stream.
  MwStreamerProcessorBoundaryCallback on_boundary;

  // Runtime updates may run concurrently with audio and video processing. The
  // null-terminated string is borrowed for the callback.
  MwStreamerProcessorUpdateConfigCallback update_config;

  // Called once after a successful on_start and all processing has completed.
  MwStreamerProcessorStopCallback on_stop;
} MwStreamerFileProcessorCallbacks;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_H_
