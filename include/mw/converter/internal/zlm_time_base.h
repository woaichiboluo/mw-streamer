#ifndef MW_STREAMER_INCLUDE_MW_CONVERTER_INTERNAL_ZLM_TIME_BASE_H_
#define MW_STREAMER_INCLUDE_MW_CONVERTER_INTERNAL_ZLM_TIME_BASE_H_

extern "C" {
#include <libavutil/rational.h>
}

namespace mw::streamer::converter::internal {

inline constexpr AVRational kZlmTimeBase{1, 1000};

}  // namespace mw::streamer::converter::internal

#endif  // MW_STREAMER_INCLUDE_MW_CONVERTER_INTERNAL_ZLM_TIME_BASE_H_
