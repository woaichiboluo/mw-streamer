#include "mw/streamer.h"

int main() {
  mw::streamer::InitConfig config;
  config.enable_cpu_affinity = false;
  auto* context = mw::streamer::Init(config);
  int result = 0;
  {
    mw::streamer::ffmpeg::Frame frame;
    mw::streamer::ffmpeg::Packet packet;
    mw::streamer::ffmpeg::AvDictionary dictionary;
    dictionary.Set("installed", "yes");
    mw::streamer::Remuxer remuxer;
    if (!frame.get() || !packet.get() || dictionary.Get("installed") != "yes" ||
        !remuxer.output_info().output_urls.empty()) {
      result = 1;
    }
    MW_LOG_INFO_DEFAULT("Installed streamer SDK: {}",
                        dictionary.Get("installed").value());
  }
  mw::streamer::Shutdown(context);
  return result;
}
