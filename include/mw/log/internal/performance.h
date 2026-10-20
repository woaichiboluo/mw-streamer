#ifndef MW_LOG_INTERNAL_PERFORMANCE_H_
#define MW_LOG_INTERNAL_PERFORMANCE_H_

#include "mw/log.h"

namespace mw::log::internal {

// Performance instrumentation has already checked its independent stage mode.
// Reuses the active logger/sinks/async queue without applying module levels.
// Logging must remain alive until concurrent performance output has stopped.
MW_LOG_API void WritePerformanceFormattedArgs(std::string_view label,
                                              LogLevel level, const char* file,
                                              std::uint32_t line,
                                              fmt::string_view format,
                                              fmt::format_args args) noexcept;

template <typename... Args>
void WritePerformance(std::string_view label, LogLevel level, const char* file,
                      std::uint32_t line, fmt::format_string<Args...> format,
                      Args&&... args) noexcept {
  WritePerformanceFormattedArgs(label, level, file, line, format.get(),
                                fmt::make_format_args(args...));
}

}  // namespace mw::log::internal

#define MW_PERF_LOG_INFO(label, ...)               \
  ::mw::log::internal::WritePerformance(           \
      label, ::mw::log::LogLevel::kInfo, __FILE__, \
      static_cast<std::uint32_t>(__LINE__), __VA_ARGS__)
#define MW_PERF_LOG_TRACE(label, ...)               \
  ::mw::log::internal::WritePerformance(            \
      label, ::mw::log::LogLevel::kTrace, __FILE__, \
      static_cast<std::uint32_t>(__LINE__), __VA_ARGS__)

#endif  // MW_LOG_INTERNAL_PERFORMANCE_H_
