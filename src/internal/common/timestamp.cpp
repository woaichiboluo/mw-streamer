#include "mw/streamer/internal/common/timestamp.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace mw::streamer::internal {

bool IsValidTimeBase(AVRational time_base) noexcept {
  return time_base.num > 0 && time_base.den > 0;
}

int64_t RescaleTimestamp(int64_t timestamp, AVRational source_time_base,
                         AVRational target_time_base) noexcept {
  if (timestamp == AV_NOPTS_VALUE || !IsValidTimeBase(source_time_base) ||
      !IsValidTimeBase(target_time_base)) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(timestamp, source_time_base, target_time_base);
}

int64_t ToMicroseconds(int64_t timestamp, AVRational time_base) noexcept {
  return RescaleTimestamp(timestamp, time_base, kMicrosecondsTimeBase);
}

int64_t AddTimestampOffset(int64_t timestamp, AVRational timestamp_time_base,
                           int64_t offset,
                           AVRational offset_time_base) noexcept {
  if (timestamp == AV_NOPTS_VALUE || !IsValidTimeBase(timestamp_time_base) ||
      !IsValidTimeBase(offset_time_base)) {
    return AV_NOPTS_VALUE;
  }
  return timestamp +
         av_rescale_q(offset, offset_time_base, timestamp_time_base);
}

}  // namespace mw::streamer::internal
