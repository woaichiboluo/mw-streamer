#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "mw/export.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

extern "C" MW_EXPORT int RunLifecycle(const char* sample_path,
                                      const char* log_path) {
  try {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    config.log.async.enabled = true;
    config.log.async.queue_size = 64;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = log_path;
    config.log.rotating_file.max_size = 1024 * 1024;
    config.log.rotating_file.max_files = 2;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        context(mw::streamer::Init(config), &mw::streamer::Shutdown);
    {
      std::mutex mutex;
      std::condition_variable changed;
      bool received = false;
      bool failed = false;
      mw::streamer::Processor processor;
      mw::streamer::Scheduler output;
      mw::streamer::ffmpeg::HwDeviceContext cpu(
          mw::streamer::ffmpeg::HwDeviceType::kCpu);
      const auto observe = [&](const mw::streamer::ffmpeg::Frame& frame) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received = frame->buf[0] && frame->data[0] &&
                     (frame->width > 0 || frame->nb_samples > 0);
        }
        changed.notify_all();
      };
      output.SetOnFrame([&](const mw::streamer::MediaFrame& frame) {
        if (frame.video) observe(processor.ProcessVideo(*frame.video));
        if (frame.audio) observe(processor.ProcessAudio(*frame.audio));
      });
      output.SetOnEnded([&] { processor.End(); });
      output.SetOnReady([&](const auto& streams) {
        if (!processor.Start(streams, cpu)) {
          std::lock_guard<std::mutex> lock(mutex);
          failed = true;
          changed.notify_all();
        }
      });
      output.SetOnError([&](int, std::string_view) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        changed.notify_all();
      });
      mw::streamer::FfmpegInput input;
      input.SetOnDecodedReady([&](const auto& source) {
        if (!output.Start(source)) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
          }
          changed.notify_all();
        }
      });
      input.SetOnStateChanged(
          [&](mw::streamer::InputState state, int, std::string_view) {
            if (state == mw::streamer::InputState::kFailed) {
              {
                std::lock_guard<std::mutex> lock(mutex);
                failed = true;
              }
              changed.notify_all();
            }
            if (state == mw::streamer::InputState::kEnded) output.Drain();
          });
      input.Start(sample_path);
      bool completed;
      {
        std::unique_lock<std::mutex> lock(mutex);
        completed = changed.wait_for(lock, std::chrono::seconds(8),
                                     [&] { return received || failed; });
      }
      output.RequestStop();
      input.Stop();
      output.Stop();
      processor.Stop();
      if (!completed || !received || failed) {
        throw std::runtime_error(
            "Unload fixture failed to decode a file frame");
      }
    }
    MW_LOG_INFO_DEFAULT("DLL lifecycle completed");
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Unload fixture: {}\n", error.what());
    return 1;
  }
}
