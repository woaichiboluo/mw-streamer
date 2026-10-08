#include "mw/log.h"

void WriteCLogProbe(void) {
  MW_LOG_INFO("c.module", "c-info-probe");
  MW_LOG_DEBUG("c.module", "c-debug-probe");
  MW_LOG_ERROR("c.module", "c-error-probe");
}
