#include "mw/streamer/ffmpeg/filter_graph.h"

#include <new>
#include <utility>

namespace mw::streamer::ffmpeg {

FilterGraph::FilterGraph() : graph_(avfilter_graph_alloc()) {
  if (!graph_) throw std::bad_alloc();
}

FilterGraph::~FilterGraph() { avfilter_graph_free(&graph_); }

FilterGraph::FilterGraph(FilterGraph&& other) noexcept
    : graph_(std::exchange(other.graph_, nullptr)) {}

FilterGraph& FilterGraph::operator=(FilterGraph&& other) noexcept {
  if (this != &other) {
    avfilter_graph_free(&graph_);
    graph_ = std::exchange(other.graph_, nullptr);
  }
  return *this;
}

const AVFilterGraph* FilterGraph::get() const noexcept { return graph_; }

AVFilterGraph* FilterGraph::get() noexcept { return graph_; }

}  // namespace mw::streamer::ffmpeg
