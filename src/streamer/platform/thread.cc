#include "mw/streamer/platform/thread.h"

#include <future>
#include <system_error>
#include <utility>

#include "mw/log.h"

namespace mw::streamer {

Thread::~Thread() { Join(); }

bool Thread::Start(std::function<void()> task) noexcept {
  if (!task || thread_.joinable()) {
    MW_LOG_ERROR("streamer", "Thread 启动失败: 任务为空或旧线程尚未 Join");
    return false;
  }
  std::promise<void> started;
  auto ready = started.get_future();
  try {
    thread_ = std::thread([started = std::move(started),
                           task = std::move(task)]() mutable noexcept {
      started.set_value();
      task();
    });
  } catch (const std::system_error& error) {
    MW_LOG_ERROR("streamer", "Thread 创建失败: {}", error.what());
    return false;
  }
  ready.wait();
  return true;
}

void Thread::Join() noexcept {
  if (thread_.joinable()) thread_.join();
}

bool Thread::joinable() const noexcept { return thread_.joinable(); }

}  // namespace mw::streamer
