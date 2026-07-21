#include "mw/streamer/pipeline.h"

#include <mutex>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mw/streamer/pipeline_config.h"
#include "mw/streamer/status.h"

namespace mw::streamer {
namespace {

TEST(PipelineTest, StopBeforeStartMakesOneShotPipelineTerminal) {
  Pipeline pipeline(PipelineConfig{});

  EXPECT_TRUE(pipeline.Stop().ok());
  EXPECT_EQ(pipeline.state(), PipelineState::kStopped);
  EXPECT_TRUE(pipeline.Wait().ok());

  const Status start_status = pipeline.Start();
  EXPECT_EQ(start_status.code(), StatusCode::kInvalidState);
}

TEST(PipelineTest, ConvertsStartupValidationFailureToStatus) {
  std::mutex states_mutex;
  std::vector<PipelineState> states;
  PipelineConfig config;
  config.callbacks.on_state_changed = [&states_mutex, &states](
                                          PipelineState state, const Status&) {
    std::lock_guard<std::mutex> lock(states_mutex);
    states.push_back(state);
  };
  Pipeline pipeline(std::move(config));

  const Status start_status = pipeline.Start();
  EXPECT_EQ(start_status.code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(pipeline.state(), PipelineState::kFailed);
  EXPECT_EQ(pipeline.Wait().code(), StatusCode::kInvalidArgument);

  std::lock_guard<std::mutex> lock(states_mutex);
  ASSERT_EQ(states.size(), 2U);
  EXPECT_EQ(states[0], PipelineState::kStarting);
  EXPECT_EQ(states[1], PipelineState::kFailed);
}

}  // namespace
}  // namespace mw::streamer
