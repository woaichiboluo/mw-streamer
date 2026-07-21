#ifndef MW_STREAMER_INTERNAL_COMMON_TIMESTAMP_H_
#define MW_STREAMER_INTERNAL_COMMON_TIMESTAMP_H_

#include <cstdint>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace mw::streamer::internal {

inline constexpr AVRational kMicrosecondsTimeBase{1, 1'000'000};

[[nodiscard]] bool IsValidTimeBase(AVRational time_base) noexcept;
[[nodiscard]] int64_t RescaleTimestamp(int64_t timestamp,
                                       AVRational source_time_base,
                                       AVRational target_time_base) noexcept;
[[nodiscard]] int64_t ToMicroseconds(int64_t timestamp,
                                     AVRational time_base) noexcept;
[[nodiscard]] int64_t AddTimestampOffset(int64_t timestamp,
                                         AVRational timestamp_time_base,
                                         int64_t offset,
                                         AVRational offset_time_base) noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMMON_TIMESTAMP_H_
