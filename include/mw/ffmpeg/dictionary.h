#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_DICTIONARY_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_DICTIONARY_H_

extern "C" {
#include <libavutil/dict.h>
}

namespace mw::streamer::ffmpeg {

class Dictionary final {
 public:
  Dictionary() = default;
  ~Dictionary();

  Dictionary(const Dictionary&) = delete;
  Dictionary& operator=(const Dictionary&) = delete;
  Dictionary(Dictionary&& other) noexcept;
  Dictionary& operator=(Dictionary&& other) noexcept;

  void Set(const char* key, const char* value);

  const AVDictionary* get() const noexcept;
  AVDictionary** address() noexcept;

 private:
  AVDictionary* dictionary_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_DICTIONARY_H_
