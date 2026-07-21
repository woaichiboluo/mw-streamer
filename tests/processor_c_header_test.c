#include "mw/streamer/processor.h"

_Static_assert(sizeof(MwVideoColorRange) == sizeof(int32_t),
               "color range must have a fixed ABI width");
_Static_assert(sizeof(MwVideoColorSpace) == sizeof(int32_t),
               "color space must have a fixed ABI width");

static int MW_PROCESSOR_CALL OnStart(const MwProcessorContext* context,
                                     const char* config, char* error_buffer,
                                     size_t error_buffer_capacity,
                                     void* user_data) {
  (void)context;
  (void)config;
  (void)error_buffer;
  (void)error_buffer_capacity;
  (void)user_data;
  return 0;
}

int main(void) {
  MwProcessorCallbacks callbacks = {0};
  MwGpuNv12FrameView video = {0};
  MwAudioFrameView audio = {0};
  callbacks.on_start = OnStart;
  return callbacks.on_start == NULL || callbacks.user_data != NULL ||
         video.pts != 0 || audio.pts != 0;
}
