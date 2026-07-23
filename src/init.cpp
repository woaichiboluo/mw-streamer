#include "mw/init.hpp"

#include <mutex>
#include <optional>
#include <stdexcept>

#include "srt/SrtEpollReactor.h"

namespace mw {
namespace {

class Initializer final {
 public:
  static Initializer& instance() {
    static Initializer initializer;
    return initializer;
  }

  void init(const InitConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logging_) {
      return;
    }
    if (srtReactorStopped()) {
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

  void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    mediakit::SrtEpollReactor::shutdownIfCreated();
    logging_.reset();
  }

  bool initialized() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return logging_.has_value();
  }

 private:
  Initializer() = default;

  static bool srtReactorStopped() noexcept {
    return mediakit::SrtEpollReactor::isCreated() &&
           !mediakit::SrtEpollReactor::Instance().available();
  }

  mutable std::mutex mutex_;
  std::once_flag init_once_;
  std::optional<log::Logging> logging_;
};

}  // namespace

void init(const InitConfig& config) { Initializer::instance().init(config); }

void shutdown() noexcept { Initializer::instance().shutdown(); }

bool initialized() noexcept { return Initializer::instance().initialized(); }

}  // namespace mw
