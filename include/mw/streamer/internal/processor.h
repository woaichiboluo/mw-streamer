#ifndef MW_STREAMER_INTERNAL_PROCESSOR_H_
#define MW_STREAMER_INTERNAL_PROCESSOR_H_

#include <cstddef>
#include <string_view>
#include <vector>

#include "mw/streamer/processor.h"

namespace mw::streamer::internal {

class Processor {
 public:
  Processor(const MwProcessorCallbacks& callbacks,
            const MwProcessorContext& context);

  Processor(const Processor&) = delete;
  Processor& operator=(const Processor&) = delete;
  Processor(Processor&&) = delete;
  Processor& operator=(Processor&&) = delete;

  [[nodiscard]] const MwProcessorContext& context() const noexcept;
  [[nodiscard]] bool has_video_callback() const noexcept;
  [[nodiscard]] bool has_audio_callback() const noexcept;

  void Start(std::string_view initial_config);
  void UpdateConfig(std::string_view config);
  void ProcessVideo(const MwGpuNv12FrameView* inputs, std::size_t input_count,
                    MwGpuNv12FrameView* output) const;
  void ProcessAudio(const MwAudioFrameView* inputs, std::size_t input_count,
                    MwAudioFrameView* output) const;
  void Stop() const;

 private:
  MwProcessorCallbacks callbacks_{};
  std::vector<MwInputSourceInfo> input_sources_;
  MwProcessorContext context_{};
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_PROCESSOR_H_
