#include <memory>

#include "mw/streamer.h"
#include "mw/streamer/platform/blocking_queue.h"
#include "mw/streamer/platform/thread.h"
#include "mw/streamer/platform/wait_signal.h"
#include "mw/streamer/platform/write_signal.h"

int main() {
  auto wake = std::make_shared<mw::streamer::internal::WaitSignal>();
  mw::streamer::internal::WriteSignal writable(wake);
  const auto observed = wake->GetRevision();
  writable.Notify();
  if (wake->GetRevision() == observed) return 1;
  mw::streamer::RealtimeConfig realtime;
  if (realtime.jitter_margin.count() != 0) return 1;
  mw::streamer::InitConfig config;
  config.performance.stages = {{mw::streamer::PerformanceStage::kEncoderVideo,
                                mw::streamer::PerformanceMode::kSummary}};
  config.enable_cpu_affinity = false;
  auto* context = mw::streamer::Init(config);
  int result = 0;
  {
    mw::streamer::BlockingQueue<int> queue(1);
    queue.SetReadSignal(wake);
    mw::streamer::Thread worker;
    if (!worker.Start([&queue] {
          queue.Push(42);
          queue.Close();
        })) {
      result = 1;
    } else {
      int value = 0;
      const auto read = queue.Pop(value);
      worker.Join();
      if (read != mw::streamer::BlockingQueueResult::kSuccess || value != 42 ||
          queue.Pop(value) != mw::streamer::BlockingQueueResult::kClosed) {
        result = 1;
      }
    }
    mw::streamer::ffmpeg::Frame frame;
    mw::streamer::ffmpeg::Packet packet;
    mw::streamer::ffmpeg::AvDictionary dictionary;
    dictionary.Set("installed", "yes");
    mw::streamer::Remuxer remuxer;
    const auto probe = mw::streamer::AvProbe::Probe("");
    if (!frame.get() || !packet.get() || dictionary.Get("installed") != "yes" ||
        !remuxer.output_info().output_urls.empty() || probe.info ||
        !probe.error) {
      result = 1;
    }
    MW_LOG_INFO_DEFAULT("Installed streamer SDK: {}",
                        dictionary.Get("installed").value());
  }
  mw::streamer::Shutdown(context);
  return result;
}
