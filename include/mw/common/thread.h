#ifndef MW_STREAMER_INCLUDE_MW_COMMON_THREAD_H_
#define MW_STREAMER_INCLUDE_MW_COMMON_THREAD_H_

#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "Util/util.h"

namespace mw::streamer::common {

class Thread final {
 public:
  using Entry = std::function<void()>;

  Thread(std::string name, Entry entry) {
    if (name.empty()) {
      throw std::invalid_argument("线程名称不能为空");
    }
    if (!entry) {
      throw std::invalid_argument("线程入口不能为空");
    }

    thread_ = std::thread(
        [name = std::move(name), entry = std::move(entry)]() mutable {
          toolkit::setThreadName(name.c_str());
          entry();
        });
  }

  ~Thread() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(Thread&&) = delete;

  void Join() {
    if (!thread_.joinable()) {
      return;
    }
    if (IsCurrent()) {
      throw std::logic_error("线程不能Join自身");
    }
    thread_.join();
  }

  bool joinable() const noexcept { return thread_.joinable(); }

  bool IsCurrent() const noexcept {
    return thread_.joinable() && thread_.get_id() == std::this_thread::get_id();
  }

 private:
  std::thread thread_;
};

}  // namespace mw::streamer::common

#endif  // MW_STREAMER_INCLUDE_MW_COMMON_THREAD_H_
