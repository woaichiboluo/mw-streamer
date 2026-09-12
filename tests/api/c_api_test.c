#include "mw/streamer.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static int Check(int condition) { return condition ? 0 : 1; }

static MwPerformanceSnapshot Snapshot(int64_t sampled_at_ns,
                                      uint64_t input_count,
                                      uint64_t completed_calls,
                                      int64_t total_time_ns) {
  MwPerformanceSnapshot snapshot = {0};
  snapshot.pipeline_id = 7;
  snapshot.sampled_at_ns = sampled_at_ns;
  snapshot.node_count = 1;
  strcpy(snapshot.nodes[0].id, "input");
  strcpy(snapshot.nodes[0].name, "Input");
  snapshot.nodes[0].parent_index = MW_STREAMER_PERFORMANCE_NO_PARENT;
  snapshot.nodes[0].operation_count = 1;
  snapshot.nodes[0].operations[0].type = kMwPerformanceInput;
  snapshot.nodes[0].operations[0].input_unit = kMwPerformanceUnitPacket;
  snapshot.nodes[0].operations[0].output_unit = kMwPerformanceUnitPacket;
  snapshot.nodes[0].operations[0].input_count = input_count;
  snapshot.nodes[0].operations[0].output_count = input_count;
  snapshot.nodes[0].operations[0].started_calls = completed_calls;
  snapshot.nodes[0].operations[0].completed_calls = completed_calls;
  snapshot.nodes[0].operations[0].total_time_ns = total_time_ns;
  return snapshot;
}

int main(void) {
  MwPipeline* pipeline = (MwPipeline*)1;
  MwPipelineCreateInfo create_info = {0};
  if (Check(mw_pipeline_create_from_toml(NULL, &pipeline) ==
            kMwResultInvalidArgument) ||
      Check(pipeline == NULL) || Check(strlen(mw_last_error()) != 0)) {
    return 1;
  }

  create_info.toml_path = "unused.toml";
  create_info.analysis_processor_count = 1;
  pipeline = (MwPipeline*)1;
  if (Check(mw_pipeline_create_from_toml(&create_info, &pipeline) ==
            kMwResultInvalidArgument) ||
      Check(pipeline == NULL)) {
    return 1;
  }

  const MwAnalysisProcessorBinding duplicates[] = {
      {.processor_id = "analysis"},
      {.processor_id = "analysis"},
  };
  create_info.analysis_processors = duplicates;
  create_info.analysis_processor_count = 2;
  if (Check(mw_pipeline_create_from_toml(&create_info, &pipeline) ==
            kMwResultInvalidArgument)) {
    return 1;
  }

  if (Check(mw_pipeline_start(NULL) == kMwResultInvalidArgument) ||
      Check(mw_pipeline_get_state(NULL, NULL) == kMwResultInvalidArgument) ||
      Check(mw_pipeline_set_processor_config(NULL, "id", "config") ==
            kMwResultInvalidArgument) ||
      Check(mw_pipeline_get_performance(NULL, NULL) ==
            kMwResultInvalidArgument)) {
    return 1;
  }
  mw_pipeline_stop(NULL);
  mw_pipeline_destroy(NULL);

  MwPerformanceSnapshot previous = Snapshot(1000000000, 10, 4, 400);
  MwPerformanceSnapshot current = Snapshot(3000000000, 18, 8, 1000);
  if (Check(mw_performance_calculate_rates(&current, &previous) ==
            kMwResultSuccess) ||
      Check(current.interval_ns == 2000000000) ||
      Check(current.nodes[0].operations[0].rates_available == 1) ||
      Check(fabs(current.nodes[0].operations[0].input_per_second - 4.0) <
            0.000001) ||
      Check(fabs(current.nodes[0].operations[0].calls_per_second - 2.0) <
            0.000001) ||
      Check(current.nodes[0].operations[0].interval_mean_time_ns == 150)) {
    return 1;
  }

  current = Snapshot(3000000000, 9, 8, 1000);
  if (Check(mw_performance_calculate_rates(&current, &previous) ==
            kMwResultInvalidArgument)) {
    return 1;
  }
  current = Snapshot(3000000000, 18, 8, 1000);
  strcpy(current.nodes[0].id, "other");
  if (Check(mw_performance_calculate_rates(&current, &previous) ==
            kMwResultInvalidArgument)) {
    return 1;
  }
  return 0;
}
