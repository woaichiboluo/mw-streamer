#ifndef MW_STREAMER_STATUS_H_
#define MW_STREAMER_STATUS_H_

#include <string>

namespace mw::streamer {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kNotFound,
  kUnsupported,
  kUnavailable,
  kAlreadyExists,
  kInvalidState,
  kFfmpegError,
  kProcessorStartCallbackFailed,
  kProcessorConfigUpdateCallbackFailed,
  kInternal,
};

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string message);

  static Status Ok();

  [[nodiscard]] bool ok() const noexcept;
  [[nodiscard]] StatusCode code() const noexcept;
  [[nodiscard]] const std::string& message() const noexcept;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_STATUS_H_
