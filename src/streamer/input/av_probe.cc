#include "mw/streamer/input/av_probe.h"

#include <cerrno>
#include <chrono>
#include <new>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/av_dictionary.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/input/internal/input_info.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kProbeTimeout = std::chrono::seconds(30);

struct ProbeInput {
  ~ProbeInput() { avformat_close_input(&format); }

  AVFormatContext* format = nullptr;
  Clock::time_point deadline = Clock::now() + kProbeTimeout;

  static int Interrupt(void* opaque) {
    const auto* input = static_cast<const ProbeInput*>(opaque);
    return Clock::now() >= input->deadline;
  }

  void CheckIo(int code, const char* operation) const {
    if (Clock::now() >= deadline) code = AVERROR(ETIMEDOUT);
    ffmpeg::FfmpegException::throwIfError(code, operation);
  }
};

}  // namespace

AvProbeResult AvProbe::Probe(std::string_view url) noexcept {
  return Probe(url, {});
}

AvProbeResult AvProbe::Probe(
    std::string_view url,
    const std::map<std::string, std::string>& options) noexcept {
  try {
    if (url.empty() || url.find('\0') != std::string_view::npos ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      throw std::invalid_argument("AvProbe 地址不能为空或包含空字符");
    }
    const std::string address(url);
    ProbeInput input;
    input.format = avformat_alloc_context();
    if (!input.format) throw std::bad_alloc();
    input.format->interrupt_callback = {&ProbeInput::Interrupt, &input};
    ffmpeg::AvDictionary dictionary(options);
    input.CheckIo(avformat_open_input(&input.format, address.c_str(), nullptr,
                                      dictionary.address()),
                  "avformat_open_input");
    input.CheckIo(avformat_find_stream_info(input.format, nullptr),
                  "avformat_find_stream_info");
    return {internal::MakeInputInfo(*input.format), {}};
  } catch (...) {
    return {std::nullopt, std::current_exception()};
  }
}

}  // namespace mw::streamer
