#ifndef MW_STREAMER_PROCESSOR_H_
#define MW_STREAMER_PROCESSOR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MW_PROCESSOR_ERROR_MESSAGE_CAPACITY 512
#define MW_TIMESTAMP_UNAVAILABLE INT64_MIN

#if defined(_WIN32) && defined(_MSC_VER)
#define MW_PROCESSOR_CALL __cdecl
#else
#define MW_PROCESSOR_CALL
#endif

typedef struct MwRational {
  int32_t numerator;
  int32_t denominator;
} MwRational;

typedef int32_t MwVideoColorRange;
#define MW_VIDEO_COLOR_RANGE_UNSPECIFIED ((MwVideoColorRange)0)
#define MW_VIDEO_COLOR_RANGE_LIMITED ((MwVideoColorRange)1)
#define MW_VIDEO_COLOR_RANGE_FULL ((MwVideoColorRange)2)

typedef int32_t MwVideoColorSpace;
#define MW_VIDEO_COLOR_SPACE_UNSPECIFIED ((MwVideoColorSpace)0)
#define MW_VIDEO_COLOR_SPACE_BT601 ((MwVideoColorSpace)1)
#define MW_VIDEO_COLOR_SPACE_BT709 ((MwVideoColorSpace)2)
#define MW_VIDEO_COLOR_SPACE_BT2020_NCL ((MwVideoColorSpace)3)
#define MW_VIDEO_COLOR_SPACE_BT2020_CL ((MwVideoColorSpace)4)

typedef struct MwVideoSourceInfo {
  int32_t present;
  int32_t width;
  int32_t height;
  MwRational frame_rate;
  MwVideoColorRange color_range;
  MwVideoColorSpace color_space;
} MwVideoSourceInfo;

typedef struct MwAudioSourceInfo {
  int32_t present;
  int32_t sample_rate;
  int32_t channel_count;
} MwAudioSourceInfo;

typedef struct MwInputSourceInfo {
  size_t input_index;
  MwVideoSourceInfo video;
  MwAudioSourceInfo audio;
} MwInputSourceInfo;

typedef struct MwVideoOutputInfo {
  int32_t width;
  int32_t height;
  MwRational frame_rate;
  MwVideoColorRange color_range;
  MwVideoColorSpace color_space;
} MwVideoOutputInfo;

typedef struct MwAudioOutputInfo {
  /* All fields are zero when the pipeline has no audio track. */
  int32_t sample_rate;
  int32_t channel_count;
  size_t block_frame_count;
} MwAudioOutputInfo;

typedef struct MwProcessorContext {
  int32_t device_id;
  const MwInputSourceInfo* inputs;
  size_t input_count;
  MwVideoOutputInfo video_output;
  MwAudioOutputInfo audio_output;
} MwProcessorContext;

/* Input timestamps describe the decoded source frame. Output timestamps are
 * initialized to zero and ignored by the framework. Frame memory is valid only
 * during the callback that receives this view. */
typedef struct MwGpuNv12FrameView {
  int32_t device_id;
  int32_t width;
  int32_t height;
  int64_t pts;
  MwRational time_base;
  int64_t pts_us;
  uint8_t* y;
  uint8_t* uv;
  size_t y_pitch_bytes;
  size_t uv_pitch_bytes;
  MwVideoColorRange color_range;
  MwVideoColorSpace color_space;
} MwGpuNv12FrameView;

/* samples contains frame_count * channel_count float32 values in frame-major
 * interleaved order. Input samples are read-only by contract; output samples
 * are writable. The memory is valid only during the current callback. */
typedef struct MwAudioFrameView {
  float* samples;
  size_t frame_count;
  int32_t sample_rate;
  int32_t channel_count;
  int64_t pts;
  MwRational time_base;
  int64_t pts_us;
} MwAudioFrameView;

/* context and context->inputs remain valid until on_stop returns. user_data is
 * owned by the caller and must remain valid for the same lifetime. */
typedef int(MW_PROCESSOR_CALL* MwProcessorOnStart)(
    const MwProcessorContext* context, const char* config, char* error_buffer,
    size_t error_buffer_capacity, void* user_data);

typedef void(MW_PROCESSOR_CALL* MwProcessorOnVideo)(
    const MwGpuNv12FrameView* inputs, size_t input_count,
    MwGpuNv12FrameView* output, void* user_data);

typedef void(MW_PROCESSOR_CALL* MwProcessorOnAudio)(
    const MwAudioFrameView* inputs, size_t input_count,
    MwAudioFrameView* output, void* user_data);

typedef int(MW_PROCESSOR_CALL* MwProcessorOnConfigUpdate)(
    const char* config, char* error_buffer, size_t error_buffer_capacity,
    void* user_data);

typedef void(MW_PROCESSOR_CALL* MwProcessorOnStop)(void* user_data);

typedef struct MwProcessorCallbacks {
  MwProcessorOnStart on_start;
  MwProcessorOnVideo on_video;
  MwProcessorOnAudio on_audio;
  MwProcessorOnConfigUpdate on_config_update;
  MwProcessorOnStop on_stop;
  void* user_data;
} MwProcessorCallbacks;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_PROCESSOR_H_
