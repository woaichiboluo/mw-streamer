#include <stddef.h>

#include "mw/c_api.h"

_Static_assert(kMwResultSuccess == 0, "成功结果必须为0");
_Static_assert(kMwPipelineStatusIdle == 0, "Idle状态必须为0");
_Static_assert(sizeof(MwLatencyStats) > 0, "性能结构必须是完整C类型");
_Static_assert(sizeof(MwStreamerOutputSinkCallbacks) > 0,
               "Output Sink回调必须是完整C类型");

int main(void) {
  MwStreaming* streaming = NULL;
  MwRemux* remux = NULL;
  MwFile* file = NULL;
  MwPipelineStatus status = kMwPipelineStatusIdle;
  const MwResult result = mw_streaming_status(streaming, &status);
  return result != kMwResultInvalidArgument || remux != NULL || file != NULL;
}
