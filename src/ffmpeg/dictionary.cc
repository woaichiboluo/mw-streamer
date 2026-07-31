#include "mw/ffmpeg/dictionary.h"

#include <stdexcept>
#include <utility>

#include "mw/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {

Dictionary::~Dictionary() { av_dict_free(&dictionary_); }

Dictionary::Dictionary(Dictionary&& other) noexcept
    : dictionary_(std::exchange(other.dictionary_, nullptr)) {}

Dictionary& Dictionary::operator=(Dictionary&& other) noexcept {
  if (this != &other) {
    av_dict_free(&dictionary_);
    dictionary_ = std::exchange(other.dictionary_, nullptr);
  }
  return *this;
}

void Dictionary::Set(const char* key, const char* value) {
  if (!key || !value) {
    throw std::invalid_argument("FFmpeg字典键和值不能为空");
  }
  ThrowIfError(av_dict_set(&dictionary_, key, value, 0), "设置FFmpeg字典");
}

const AVDictionary* Dictionary::get() const noexcept { return dictionary_; }

AVDictionary** Dictionary::address() noexcept { return &dictionary_; }

}  // namespace mw::streamer::ffmpeg
