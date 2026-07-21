#ifndef MW_STREAMER_FFMPEG_DICTIONARY_H_
#define MW_STREAMER_FFMPEG_DICTIONARY_H_

#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/dict.h>
}

namespace mw::streamer::ffmpeg {

class Dictionary final {
 public:
  Dictionary() noexcept = default;
  ~Dictionary() noexcept;

  Dictionary(const Dictionary&) = delete;
  Dictionary& operator=(const Dictionary&) = delete;
  Dictionary(Dictionary&& other) noexcept;
  Dictionary& operator=(Dictionary&& other) noexcept;

  void Set(std::string_view key, std::string_view value);

  [[nodiscard]] AVDictionary* get() noexcept;
  [[nodiscard]] const AVDictionary* get() const noexcept;
  [[nodiscard]] AVDictionary** inout() noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::vector<std::string> keys() const;

 private:
  AVDictionary* dictionary_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_DICTIONARY_H_
