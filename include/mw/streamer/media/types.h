#ifndef MW_STREAMER_MEDIA_TYPES_H_
#define MW_STREAMER_MEDIA_TYPES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MwStreamerRational {
  int32_t num;
  int32_t den;
} MwStreamerRational;

typedef struct MwStreamerMediaTimestamp {
  int64_t pts;
  int64_t duration;
  MwStreamerRational time_base;
} MwStreamerMediaTimestamp;

typedef enum MwStreamerCodec {
  kMwStreamerCodecUnknown = 0,
  kMwStreamerCodecH264,
  kMwStreamerCodecH265,
  kMwStreamerCodecAv1,
  kMwStreamerCodecAac,
  kMwStreamerCodecG711A,
  kMwStreamerCodecG711U,
  kMwStreamerCodecOpus,
  kMwStreamerCodecMjpeg,
  kMwStreamerCodecVp8,
  kMwStreamerCodecVp9,
} MwStreamerCodec;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_MEDIA_TYPES_H_
