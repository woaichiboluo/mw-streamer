#include "Util/logger.h"

namespace {

class NullLogChannel : public toolkit::LogChannel {
 public:
  NullLogChannel() : LogChannel("null") {}

  void write(const toolkit::Logger &,
             const toolkit::LogContextPtr &) override {}
};

class EarlyLoggerUser {
 public:
  EarlyLoggerUser() {
    auto &logger = toolkit::getLogger();
    logger.add(std::make_shared<NullLogChannel>());
  }
};

#if defined(__GNUC__)
// Run before ordinary dynamic initialization so Logger is destroyed after
// logger.cpp's file-scope objects. ASan then detects module-name regressions.
__attribute__((init_priority(101)))
#endif
EarlyLoggerUser early_logger_user;

}  // namespace

int main() { return 0; }
