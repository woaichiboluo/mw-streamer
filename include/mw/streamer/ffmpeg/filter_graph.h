#ifndef MW_STREAMER_FFMPEG_FILTER_GRAPH_H_
#define MW_STREAMER_FFMPEG_FILTER_GRAPH_H_

#include <cstdint>

extern "C" {
#include <libavfilter/avfilter.h>
}

#include "mw/export.h"

namespace mw::streamer::ffmpeg {

// Owns a graph and its filter contexts. Allocation failure throws bad_alloc.
// Native pointers are borrowed; contexts must not outlive this graph. Serialize
// graph operations externally. Moving transfers ownership without relocating
// filter contexts; a moved-from graph's get() returns nullptr.
class MW_STREAMER_API FilterGraph final {
 public:
  FilterGraph();
  ~FilterGraph();

  FilterGraph(const FilterGraph&) = delete;
  FilterGraph& operator=(const FilterGraph&) = delete;
  FilterGraph(FilterGraph&& other) noexcept;
  FilterGraph& operator=(FilterGraph&& other) noexcept;

  const AVFilterGraph* get() const noexcept;
  AVFilterGraph* get() noexcept;

 private:
  AVFilterGraph* graph_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_FILTER_GRAPH_H_
