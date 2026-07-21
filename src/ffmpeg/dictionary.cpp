#include "mw/streamer/ffmpeg/dictionary.h"

#include <string>
#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

Dictionary::~Dictionary() noexcept { av_dict_free(&dictionary_); }

Dictionary::Dictionary(Dictionary&& other) noexcept
    : dictionary_(std::exchange(other.dictionary_, nullptr)) {}

Dictionary& Dictionary::operator=(Dictionary&& other) noexcept {
  if (this != &other) {
    av_dict_free(&dictionary_);
    dictionary_ = std::exchange(other.dictionary_, nullptr);
  }
  return *this;
}

void Dictionary::Set(std::string_view key, std::string_view value) {
  const std::string null_terminated_key(key);
  const std::string null_terminated_value(value);
  internal::CheckFfmpeg(av_dict_set(&dictionary_, null_terminated_key.c_str(),
                                    null_terminated_value.c_str(), 0),
                        "failed to set FFmpeg dictionary entry");
}

AVDictionary* Dictionary::get() noexcept { return dictionary_; }

const AVDictionary* Dictionary::get() const noexcept { return dictionary_; }

AVDictionary** Dictionary::inout() noexcept { return &dictionary_; }

bool Dictionary::empty() const noexcept {
  return av_dict_count(dictionary_) == 0;
}

std::vector<std::string> Dictionary::keys() const {
  std::vector<std::string> result;
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(dictionary_, "", entry, AV_DICT_IGNORE_SUFFIX)) !=
         nullptr) {
    result.emplace_back(entry->key);
  }
  return result;
}

}  // namespace mw::streamer::ffmpeg
