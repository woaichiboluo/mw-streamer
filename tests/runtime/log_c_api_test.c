#include "mw/log.h"

int main(void) {
  MwLogConfig config;
  mw_log_default_config(&config);
  config.console_level = kMwLogLevelOff;
  if (mw_log_initialize(&config) != kMwLogSuccess) {
    return 1;
  }
  MW_LOG_INFO("c-client", "plain C log message");
  MW_LOG_INFO_DEFAULT("default C log message");
  mw_log_shutdown();
  return 0;
}
