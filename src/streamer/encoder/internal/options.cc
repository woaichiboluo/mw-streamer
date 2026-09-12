#include "mw/streamer/encoder/internal/options.h"

#include "mw/log.h"

namespace mw::streamer::internal {

void WarnUnusedOptions(const AVDictionary* options,
                       std::string_view encoder_kind,
                       const char* encoder_name) {
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX))) {
    MW_LOG_WARNING("streamer",
                   "{}编码器未消费属性，已忽略: encoder_name={}, {}={}",
                   encoder_kind, encoder_name, entry->key, entry->value);
  }
}

}  // namespace mw::streamer::internal
