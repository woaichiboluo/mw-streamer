#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "mw/export.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/zlm_input.h"

extern "C" MW_EXPORT int RunLifecycle(const char* sample_path,
                                      const char* log_path) {
  try {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    config.log.async_enabled = 1;
    config.log.async_queue_size = 64;
    config.log.rotating_file_enabled = 1;
    config.log.rotating_file_path = log_path;
    config.log.rotating_file_path_size = std::strlen(log_path);
    config.log.rotating_file_max_size = 1024 * 1024;
    config.log.rotating_file_max_files = 2;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        context(mw::streamer::Init(config), &mw::streamer::Shutdown);
    {
      std::mutex mutex;
      std::condition_variable changed;
      bool received = false;
      bool failed = false;
      mw::streamer::ZlmInput input;
      input.SetOnPacket([&](const mw::streamer::ffmpeg::Packet& packet) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          received = packet->buf && packet->data && packet->size > 0;
        }
        changed.notify_all();
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
          });
      input.Start(sample_path);
      bool completed;
      {
        std::unique_lock<std::mutex> lock(mutex);
        completed = changed.wait_for(lock, std::chrono::seconds(8),
                                     [&] { return received || failed; });
      }
      input.Stop();
      if (!completed || !received || failed) {
        throw std::runtime_error(
            "Unload fixture failed to receive a file packet");
      }
    }
    MW_LOG_INFO_DEFAULT("DLL lifecycle completed");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Unload fixture: " << error.what() << '\n';
    return 1;
  }
}
