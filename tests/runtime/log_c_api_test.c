#include "mw/log.h"

int main(void) {
  static const char modules[] = "c-client;";
  MwLogConfig config;
  if (mw_log_should_log(kMwLogLevelCritical, "default",
                        sizeof("default") - 1)) {
    return 1;
  }
  mw_log_default_config(&config);
  config.modules = modules;
  config.modules_size = sizeof(modules) - 1;
  config.console_enabled = 0;
  if (mw_log_initialize(&config) != kMwLogSuccess) {
    return 1;
  }
  if (!mw_log_should_log(kMwLogLevelInfo, "c-client", sizeof("c-client") - 1) ||
      mw_log_should_log(kMwLogLevelWarning, "unconfigured",
                        sizeof("unconfigured") - 1) ||
      !mw_log_should_log(kMwLogLevelError, "unconfigured",
                         sizeof("unconfigured") - 1) ||
      mw_log_should_log(kMwLogLevelDebug, "default", sizeof("default") - 1) ||
      !mw_log_should_log(kMwLogLevelInfo, "default", sizeof("default") - 1)) {
    mw_log_shutdown();
    return 1;
  }
  MW_LOG_INFO("c-client", "plain C log message");
  MW_LOG_INFO_DEFAULT("default C log message");
  mw_log_shutdown();
  return mw_log_should_log(kMwLogLevelCritical, "default",
                           sizeof("default") - 1)
             ? 1
             : 0;
}
