#ifndef MW_STREAMER_PIPELINE_H_
#define MW_STREAMER_PIPELINE_H_

#include <memory>
#include <string>

#include "mw/streamer/pipeline_config.h"
#include "mw/streamer/status.h"

namespace mw::streamer {

class Pipeline final {
 public:
  explicit Pipeline(PipelineConfig config);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  Pipeline(Pipeline&&) = delete;
  Pipeline& operator=(Pipeline&&) = delete;

  [[nodiscard]] Status Start();
  [[nodiscard]] Status Stop();
  [[nodiscard]] Status Wait();
  [[nodiscard]] Status UpdateProcessorConfig(std::string config);
  [[nodiscard]] PipelineState state() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_H_
