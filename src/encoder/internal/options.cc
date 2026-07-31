#include "mw/encoder/internal/options.h"

#include "mw/log/logging.h"

namespace mw::streamer::encoder::internal {

void WarnUnusedOptions(const AVDictionary* options,
                       std::string_view encoder_kind,
                       const char* encoder_name) {
  using Log = log::Module<log::LogModule::kStreamer>;

  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX))) {
    Log::Warning("{}编码器未消费属性，已忽略: encoder_name={}, {}={}",
                 encoder_kind, encoder_name, entry->key, entry->value);
  }
}

}  // namespace mw::streamer::encoder::internal
