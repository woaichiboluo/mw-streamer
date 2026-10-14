#include <Windows.h>
#include <mmsystem.h>

#include <stdexcept>
#include <system_error>

#include "mw/streamer/platform/runtime.h"

namespace mw::streamer::internal {

void IgnoreSigpipe() {}

void EnableUtf8Console() {
  if (GetConsoleCP() == 0) return;
  if (!SetConsoleCP(CP_UTF8)) {
    throw std::system_error(GetLastError(), std::system_category(),
                            "设置 Windows 控制台 UTF-8 输入代码页失败");
  }
  if (!SetConsoleOutputCP(CP_UTF8)) {
    throw std::system_error(GetLastError(), std::system_category(),
                            "设置 Windows 控制台 UTF-8 输出代码页失败");
  }
}

ScopedTimerResolution::ScopedTimerResolution() {
  if (timeBeginPeriod(1) != TIMERR_NOERROR) {
    throw std::runtime_error("无法申请Windows 1ms定时精度");
  }
}

ScopedTimerResolution::~ScopedTimerResolution() { timeEndPeriod(1); }

}  // namespace mw::streamer::internal
