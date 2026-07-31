#include <catch2/catch_test_macros.hpp>
#include <string_view>

#include "mw/processor/internal/enum_converter.h"

namespace {

using mw::streamer::processor::internal::ExecutionTypeFromName;
using mw::streamer::processor::internal::ProcessorBoundaryFromName;
using mw::streamer::processor::internal::ToName;

TEST_CASE("Processor枚举名称支持双向转换") {
  CHECK(ToName(kMwStreamerProcessorTimelineReset) == "timeline_reset");
  CHECK(ToName(kMwStreamerProcessorEndOfInput) == "end_of_input");
  CHECK(ProcessorBoundaryFromName("timeline_reset") ==
        kMwStreamerProcessorTimelineReset);
  CHECK(ProcessorBoundaryFromName("end_of_input") ==
        kMwStreamerProcessorEndOfInput);

  CHECK(ToName(kMwStreamerExecutionCpu) == "cpu");
  CHECK(ToName(kMwStreamerExecutionCuda) == "cuda");
  CHECK(ExecutionTypeFromName("cpu") == kMwStreamerExecutionCpu);
  CHECK(ExecutionTypeFromName("cuda") == kMwStreamerExecutionCuda);
}

TEST_CASE("Processor枚举转换拒绝未知值和名称") {
  CHECK(ToName(static_cast<MwStreamerProcessorBoundaryReason>(-1)) ==
        "unknown");
  CHECK(ToName(static_cast<MwStreamerExecutionType>(-1)) == "unknown");

  CHECK_FALSE(ProcessorBoundaryFromName("unknown"));
  CHECK_FALSE(ExecutionTypeFromName("unknown"));
}

}  // namespace
