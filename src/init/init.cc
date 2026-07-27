#include "mw/init/init.h"

#include <mutex>
#include <optional>
#include <stdexcept>

#include "srt/SrtEpollReactor.h"

namespace mw::streamer {
namespace {

class Initializer final {
 public:
  static Initializer& Instance() {
    static Initializer initializer;
    return initializer;
  }

  void Init(const InitConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logging_) {
      return;
    }
    if (IsSrtReactorStopped()) {
      throw std::logic_error(
          "mw-streamer cannot be initialized after the SRT reactor was "
          "stopped");
    }

    std::call_once(init_once_,
                   [this, &config]() { logging_.emplace(config.log); });
    if (!logging_) {
      throw std::logic_error(
          "mw-streamer cannot be initialized after it was shut down");
    }
  }

  void Shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    mediakit::SrtEpollReactor::shutdownIfCreated();
    logging_.reset();
  }

  bool IsInitialized() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return logging_.has_value();
  }

 private:
  Initializer() = default;

  static bool IsSrtReactorStopped() noexcept {
    return mediakit::SrtEpollReactor::isCreated() &&
           !mediakit::SrtEpollReactor::Instance().available();
  }

  mutable std::mutex mutex_;
  std::once_flag init_once_;
  std::optional<log::Logging> logging_;
};

}  // namespace

void Init(const InitConfig& config) { Initializer::Instance().Init(config); }

void Shutdown() noexcept { Initializer::Instance().Shutdown(); }

bool IsInitialized() noexcept {
  return Initializer::Instance().IsInitialized();
}

}  // namespace mw::streamer
