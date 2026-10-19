#include "mw/log.h"

int main() {
  mw::log::Logging logging;
  MW_LOG_INFO_DEFAULT("Installed log SDK: {}", 42);
  return mw::log::ShouldLog("default", mw::log::LogLevel::kInfo) ? 0 : 1;
}
