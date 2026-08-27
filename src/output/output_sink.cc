#include "mw/output/output_sink.h"

#include <utility>

namespace mw::streamer::output {

OutputSink::~OutputSink() = default;

bool OutputSink::NotifyEvent(
    std::string_view type, const void* payload, std::size_t payload_size,
    std::optional<MwStreamerMediaTimestamp> timestamp) const {
  return event_submitter_ &&
         event_submitter_(type, payload, payload_size, std::move(timestamp));
}

void OutputSink::BindEventSubmitter(EventSubmitter submitter) {
  event_submitter_ = std::move(submitter);
}

void internal::OutputSinkAccess::BindEventSubmitter(
    OutputSink& sink, OutputSink::EventSubmitter submitter) {
  sink.BindEventSubmitter(std::move(submitter));
}

}  // namespace mw::streamer::output
