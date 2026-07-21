#include "mw/streamer/status.h"

#include <utility>

namespace mw::streamer {

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() { return {}; }

bool Status::ok() const noexcept { return code_ == StatusCode::kOk; }

StatusCode Status::code() const noexcept { return code_; }

const std::string& Status::message() const noexcept { return message_; }

}  // namespace mw::streamer
