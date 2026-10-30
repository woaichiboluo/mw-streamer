#ifndef MW_SRC_STREAMER_PLATFORM_RUNTIME_H_
#define MW_SRC_STREAMER_PLATFORM_RUNTIME_H_

namespace mw::streamer::internal {

// Ignores SIGPIPE process-wide on POSIX; a no-op on Windows. Init calls this
// before starting media workers. The signal disposition is not restored.
void IgnoreSigpipe();

// Sets the attached Windows console's input/output code pages to UTF-8.
// No-op without a console or on POSIX. The code pages are not restored.
void EnableUtf8Console();

// Owned by Init/Shutdown; destroy after all media workers have stopped.
class ScopedTimerResolution final {
 public:
  ScopedTimerResolution();
  ~ScopedTimerResolution();
  ScopedTimerResolution(const ScopedTimerResolution&) = delete;
  ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_PLATFORM_RUNTIME_H_
