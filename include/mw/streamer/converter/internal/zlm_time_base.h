#ifndef MW_STREAMER_CONVERTER_INTERNAL_ZLM_TIME_BASE_H_
#define MW_STREAMER_CONVERTER_INTERNAL_ZLM_TIME_BASE_H_

extern "C" {
#include <libavutil/rational.h>
}

namespace mw::streamer::internal {

inline constexpr AVRational kZlmTimeBase{1, 1000};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_CONVERTER_INTERNAL_ZLM_TIME_BASE_H_
