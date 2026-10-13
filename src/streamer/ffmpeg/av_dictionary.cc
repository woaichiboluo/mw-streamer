#include "mw/streamer/ffmpeg/av_dictionary.h"

#include <stdexcept>
#include <utility>

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

void CheckText(std::string_view text) {
  if (text.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("AVDictionary 键值不能包含空字符");
  }
}

}  // namespace

AvDictionary::AvDictionary(const std::map<std::string, std::string>& values)
    : AvDictionary() {
  for (const auto& [key, value] : values) Set(key, value);
}

AvDictionary::AvDictionary(const AVDictionary* source) : AvDictionary() {
  FfmpegException::throwIfError(
      av_dict_copy(&dictionary_, source, AV_DICT_MULTIKEY),
      "复制 AVDictionary");
}

AvDictionary::~AvDictionary() { av_dict_free(&dictionary_); }

AvDictionary::AvDictionary(const AvDictionary& other)
    : AvDictionary(other.dictionary_) {}

AvDictionary& AvDictionary::operator=(const AvDictionary& other) {
  if (this != &other) {
    AvDictionary copy(other);
    std::swap(dictionary_, copy.dictionary_);
  }
  return *this;
}

AvDictionary::AvDictionary(AvDictionary&& other) noexcept
    : dictionary_(std::exchange(other.dictionary_, nullptr)) {}

AvDictionary& AvDictionary::operator=(AvDictionary&& other) noexcept {
  if (this != &other) {
    av_dict_free(&dictionary_);
    dictionary_ = std::exchange(other.dictionary_, nullptr);
  }
  return *this;
}

void AvDictionary::Set(std::string_view key, std::string_view value) {
  CheckText(key);
  CheckText(value);
  const std::string name(key);
  const std::string content(value);
  FfmpegException::throwIfError(
      av_dict_set(&dictionary_, name.c_str(), content.c_str(),
                  AV_DICT_MATCH_CASE),
      "设置 AVDictionary");
}

std::optional<std::string> AvDictionary::Get(std::string_view key) const {
  CheckText(key);
  const std::string name(key);
  const auto* entry =
      av_dict_get(dictionary_, name.c_str(), nullptr, AV_DICT_MATCH_CASE);
  if (!entry) return std::nullopt;
  return std::string(entry->value);
}

std::map<std::string, std::string> AvDictionary::ToMap() const {
  return ToMap(dictionary_);
}

std::map<std::string, std::string> AvDictionary::ToMap(
    const AVDictionary* source) {
  std::map<std::string, std::string> result;
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(source, "", entry, AV_DICT_IGNORE_SUFFIX))) {
    result.emplace(entry->key, entry->value);
  }
  return result;
}

AVDictionary* AvDictionary::get() noexcept { return dictionary_; }
const AVDictionary* AvDictionary::get() const noexcept { return dictionary_; }
AVDictionary** AvDictionary::address() noexcept { return &dictionary_; }

}  // namespace mw::streamer::ffmpeg
