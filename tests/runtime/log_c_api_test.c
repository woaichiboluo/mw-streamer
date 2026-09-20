#include "mw/log.h"

int main(void) {
  const MwLogModuleConfig modules[] = {
      {"c-client", sizeof("c-client") - 1, kMwLogLevelInfo},
  };
  MwLogConfig config;
  mw_log_default_config(&config);
  config.modules = modules;
  config.module_count = sizeof(modules) / sizeof(modules[0]);
  config.console_level = kMwLogLevelOff;
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
  return 0;
}
