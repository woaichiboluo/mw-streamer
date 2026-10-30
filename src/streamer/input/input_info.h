#ifndef MW_SRC_STREAMER_INPUT_INPUT_INFO_H_
#define MW_SRC_STREAMER_INPUT_INPUT_INFO_H_

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/input/av_input.h"

namespace mw::streamer::internal {

// Copies a successfully probed input without retaining its connection.
AvInputInfo MakeInputInfo(AVFormatContext& format);

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_INPUT_INPUT_INFO_H_
