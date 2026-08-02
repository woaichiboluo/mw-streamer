#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STOPWATCH_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STOPWATCH_H_

#include <chrono>
#include <functional>
#include <type_traits>
#include <utility>

namespace mw::streamer::performance::internal {

class Stopwatch final {
 public:
  template <typename Function>
  auto Measure(Function&& function) {
    const auto started = std::chrono::steady_clock::now();
    using Result = std::invoke_result_t<Function>;
    if constexpr (std::is_void_v<Result>) {
      std::invoke(std::forward<Function>(function));
      elapsed_ += std::chrono::steady_clock::now() - started;
    } else {
      auto result = std::invoke(std::forward<Function>(function));
      elapsed_ += std::chrono::steady_clock::now() - started;
      return result;
    }
  }

  std::chrono::nanoseconds elapsed() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_);
  }

 private:
  std::chrono::steady_clock::duration elapsed_{};
};

}  // namespace mw::streamer::performance::internal

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STOPWATCH_H_
