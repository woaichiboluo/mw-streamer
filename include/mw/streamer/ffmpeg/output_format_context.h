#ifndef MW_STREAMER_FFMPEG_OUTPUT_FORMAT_CONTEXT_H_
#define MW_STREAMER_FFMPEG_OUTPUT_FORMAT_CONTEXT_H_

#include <string_view>

extern "C" {
#include <libavformat/avformat.h>
}

namespace mw::streamer::ffmpeg {

class OutputFormatContext final {
 public:
  explicit OutputFormatContext(std::string_view format_name);
  ~OutputFormatContext() noexcept;

  OutputFormatContext(const OutputFormatContext&) = delete;
  OutputFormatContext& operator=(const OutputFormatContext&) = delete;
  OutputFormatContext(OutputFormatContext&& other) noexcept;
  OutputFormatContext& operator=(OutputFormatContext&& other) noexcept;

  [[nodiscard]] AVFormatContext* get() noexcept;
  [[nodiscard]] const AVFormatContext* get() const noexcept;

 private:
  AVFormatContext* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_OUTPUT_FORMAT_CONTEXT_H_
