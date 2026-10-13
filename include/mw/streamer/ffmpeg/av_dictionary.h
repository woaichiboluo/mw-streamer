#ifndef MW_STREAMER_FFMPEG_AV_DICTIONARY_H_
#define MW_STREAMER_FFMPEG_AV_DICTIONARY_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>

extern "C" {
#include <libavutil/dict.h>
}

#include "mw/export.h"

namespace mw::streamer::ffmpeg {

// Owns an AVDictionary. Copies own independent entries; moves transfer
// ownership. Access, including native operations through get/address, must be
// serialized.
class MW_STREAMER_API AvDictionary final {
 public:
  AvDictionary() noexcept = default;
  explicit AvDictionary(const std::map<std::string, std::string>& values);
  // Deep copies a borrowed dictionary. nullptr represents an empty dictionary.
  explicit AvDictionary(const AVDictionary* source);
  ~AvDictionary();

  AvDictionary(const AvDictionary& other);
  AvDictionary& operator=(const AvDictionary& other);
  AvDictionary(AvDictionary&& other) noexcept;
  AvDictionary& operator=(AvDictionary&& other) noexcept;

  // Exact, case-sensitive keys. Embedded NULs throw std::invalid_argument;
  // native allocation failures throw FfmpegException.
  void Set(std::string_view key, std::string_view value);
  // Missing keys return nullopt, distinct from an existing empty value. The
  // returned string is independent of subsequent dictionary changes.
  std::optional<std::string> Get(std::string_view key) const;
  std::map<std::string, std::string> ToMap() const;
  // Reads borrowed metadata without copying an intermediate AVDictionary.
  // nullptr produces an empty map; duplicate keys keep their first value.
  static std::map<std::string, std::string> ToMap(const AVDictionary* source);

  // Borrowed pointers, possibly null. Native mutations may invalidate entries.
  AVDictionary* get() noexcept;
  const AVDictionary* get() const noexcept;
  // For FFmpeg APIs that consume or replace the dictionary through **options.
  // Ownership remains here; the address must not outlive this object.
  AVDictionary** address() noexcept;

 private:
  AVDictionary* dictionary_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_AV_DICTIONARY_H_
