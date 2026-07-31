#ifndef MW_STREAMER_INCLUDE_MW_ENCODER_INTERNAL_OPTIONS_H_
#define MW_STREAMER_INCLUDE_MW_ENCODER_INTERNAL_OPTIONS_H_

#include <string_view>

extern "C" {
#include <libavutil/dict.h>
}

namespace mw::streamer::encoder::internal {

void WarnUnusedOptions(const AVDictionary* options,
                       std::string_view encoder_kind, const char* encoder_name);

}  // namespace mw::streamer::encoder::internal

#endif  // MW_STREAMER_INCLUDE_MW_ENCODER_INTERNAL_OPTIONS_H_
