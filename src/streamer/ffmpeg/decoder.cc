#include "mw/streamer/ffmpeg/decoder.h"

#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

const AVCodec* FindDecoder(const StreamInfo& stream, AVMediaType type,
                           std::string_view decoder_name) {
  stream.Validate();
  const auto* parameters = stream.codec_parameters.get();
  if (parameters->codec_type != type) {
    throw std::invalid_argument("解码器类型与输入轨道类型不匹配");
  }
  const std::string name(decoder_name);
  const auto* codec = name.empty() ? avcodec_find_decoder(parameters->codec_id)
                                   : avcodec_find_decoder_by_name(name.c_str());
  if (!codec) {
    throw FfmpegException(AVERROR_DECODER_NOT_FOUND, "查找解码器 " + name);
  }
  if (codec->id != parameters->codec_id ||
      codec->type != parameters->codec_type) {
    throw std::invalid_argument("指定解码器与输入轨道编码不匹配");
  }
  return codec;
}

}  // namespace

Decoder::Decoder(const StreamInfo& stream, AVMediaType type,
                 std::string_view decoder_name)
    : context_(FindDecoder(stream, type, decoder_name)),
      time_base_(stream.time_base),
      stream_index_(stream.stream_index),
      performance_module_(type == AVMEDIA_TYPE_VIDEO ? "perf.decoder.video"
                                                     : "perf.decoder.audio") {
  auto* context = context_.get();
  FfmpegException::throwIfError(
      avcodec_parameters_to_context(context, stream.codec_parameters.get()),
      "avcodec_parameters_to_context");
  context->pkt_timebase = time_base_;
  context->thread_count = 0;
}

Decoder::~Decoder() { ReportPerformance(true); }

AVCodecContext* Decoder::context() noexcept { return context_.get(); }

void Decoder::Open() {
  const auto* codec = context_.get()->codec;
  if ((codec->capabilities & AV_CODEC_CAP_HARDWARE) &&
      !context_.get()->hw_device_ctx) {
    throw std::invalid_argument("硬件解码器需要外部硬件设备上下文");
  }
  FfmpegException::throwIfError(avcodec_open2(context_.get(), codec, nullptr),
                                "avcodec_open2");
  performance_enabled_ =
      mw::log::ShouldLog(performance_module_, mw::log::LogLevel::kInfo);
  performance_trace_enabled_ =
      mw::log::ShouldLog(performance_module_, mw::log::LogLevel::kTrace);
  if (performance_enabled_) performance_.Reset(PerformanceClock::now());
}

VideoDecoder::VideoDecoder(const StreamInfo& stream,
                           std::string_view decoder_name)
    : Decoder(stream, AVMEDIA_TYPE_VIDEO, decoder_name) {
  Open();
}

VideoDecoder::VideoDecoder(const StreamInfo& stream,
                           const HwDeviceContext& device,
                           std::string_view decoder_name)
    : Decoder(stream, AVMEDIA_TYPE_VIDEO, decoder_name) {
  auto* codec_context = context();
  if (device.get()) {
    const auto* native =
        reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
    if (native->type != AV_HWDEVICE_TYPE_CUDA) {
      throw std::invalid_argument("硬解只支持CUDA视频设备");
    }
    for (int index = 0;; ++index) {
      const auto* config = avcodec_get_hw_config(codec_context->codec, index);
      if (!config) {
        throw std::invalid_argument("指定解码器不支持CUDA设备");
      }
      if (config->device_type == native->type &&
          config->pix_fmt == AV_PIX_FMT_CUDA &&
          (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
        break;
      }
    }
    codec_context->hw_device_ctx = av_buffer_ref(device.get());
    if (!codec_context->hw_device_ctx) {
      throw std::bad_alloc();
    }
    codec_context->get_format = &VideoDecoder::GetHardwareFormat;
    // Frame threads add decode surfaces; automatic CPU threading can exceed
    // NVDEC's surface limit. CUDA performs decoding on the device.
    codec_context->thread_count = 2;
  }

  Open();
}

AudioDecoder::AudioDecoder(const StreamInfo& stream,
                           std::string_view decoder_name)
    : Decoder(stream, AVMEDIA_TYPE_AUDIO, decoder_name) {
  Open();
}

AVPixelFormat VideoDecoder::GetHardwareFormat(
    AVCodecContext*, const AVPixelFormat* formats) noexcept {
  for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    if (*format == AV_PIX_FMT_CUDA) {
      return *format;
    }
  }
  return AV_PIX_FMT_NONE;
}

bool Decoder::SendPacket(const Packet& packet) {
  const auto started = BeginPerformanceWork();
  if (!packet.get() || !packet->data || packet->size <= 0) {
    FinishPerformanceWork(started, true);
    throw std::invalid_argument("解码输入Packet不能为空，请使用Drain提交EOF");
  }
  const int result = avcodec_send_packet(context_.get(), packet.get());
  if (result == AVERROR(EAGAIN)) {
    if (performance_enabled_) ++performance_.counters().again;
    FinishPerformanceWork(started);
    return false;
  }
  if (performance_enabled_ && result >= 0) {
    auto& counters = performance_.counters();
    ++counters.packets;
    counters.bytes += packet->size;
  }
  FinishPerformanceWork(started, result < 0);
  FfmpegException::throwIfError(result, "avcodec_send_packet");
  return true;
}

DecodeResult Decoder::ReceiveFrame(Frame& frame) {
  const auto started = BeginPerformanceWork();
  const int result = avcodec_receive_frame(context_.get(), frame.get());
  if (result == AVERROR(EAGAIN)) {
    if (performance_enabled_) ++performance_.counters().again;
    FinishPerformanceWork(started);
    return DecodeResult::kNeedInput;
  }
  if (result == AVERROR_EOF) {
    if (performance_enabled_) ++performance_.counters().eof;
    FinishPerformanceWork(started, false, true);
    return DecodeResult::kEnd;
  }
  if (result < 0) FinishPerformanceWork(started, true);
  FfmpegException::throwIfError(result, "avcodec_receive_frame");
  if (context_.get()->hw_device_ctx &&
      (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx)) {
    frame.Unref();
    FinishPerformanceWork(started, true);
    throw FfmpegException(AVERROR_INVALIDDATA, "硬解未交付硬件帧");
  }
  frame->time_base = time_base_;
  if (performance_enabled_) CountPerformanceFrame(*frame.get());
  FinishPerformanceWork(started);
  return DecodeResult::kFrame;
}

bool Decoder::Drain() {
  const auto started = BeginPerformanceWork();
  const int result = avcodec_send_packet(context_.get(), nullptr);
  if (result == AVERROR(EAGAIN)) {
    if (performance_enabled_) ++performance_.counters().again;
    FinishPerformanceWork(started);
    return false;
  }
  FinishPerformanceWork(started, result < 0 && result != AVERROR_EOF);
  if (result != AVERROR_EOF) {
    FfmpegException::throwIfError(result, "avcodec_send_packet EOF");
  }
  return true;
}

void Decoder::Flush() noexcept {
  context_.FlushBuffers();
  if (performance_enabled_) {
    performance_previous_pts_ = AV_NOPTS_VALUE;
    performance_final_ = false;
  }
}

Decoder::PerformanceClock::time_point Decoder::BeginPerformanceWork()
    const noexcept {
  return performance_enabled_ ? PerformanceClock::now()
                              : PerformanceClock::time_point{};
}

void Decoder::FinishPerformanceWork(PerformanceClock::time_point started,
                                    bool error, bool final) noexcept {
  if (!performance_enabled_) return;
  const auto now = PerformanceClock::now();
  performance_.AddWork(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - started)
          .count());
  if (error) ++performance_.counters().errors;
  ReportPerformance(final);
}

void Decoder::CountPerformanceFrame(const AVFrame& frame) noexcept {
  auto& counters = performance_.counters();
  ++counters.frames;
  std::int64_t media_ns = 0;
  if (context_.get()->codec_type == AVMEDIA_TYPE_AUDIO) {
    if (frame.nb_samples > 0) counters.samples += frame.nb_samples;
    if (frame.nb_samples > 0 && frame.sample_rate > 0)
      media_ns = av_rescale_q(frame.nb_samples, {1, frame.sample_rate},
                              {1, 1000000000});
  } else {
    const auto pts = frame.best_effort_timestamp != AV_NOPTS_VALUE
                         ? frame.best_effort_timestamp
                         : frame.pts;
    if (frame.duration > 0) {
      media_ns = av_rescale_q(frame.duration, time_base_, {1, 1000000000});
    } else if (pts != AV_NOPTS_VALUE &&
               performance_previous_pts_ != AV_NOPTS_VALUE &&
               pts > performance_previous_pts_) {
      media_ns = av_rescale_q(pts - performance_previous_pts_, time_base_,
                              {1, 1000000000});
    }
    // Backward PTS establishes a fresh segment; Flush also clears this base
    // so seeking cannot contribute its skipped timestamp interval as work.
    performance_previous_pts_ = pts;
  }
  if (media_ns > 0) {
    counters.media_ns += media_ns;
    counters.has_media = true;
  }
}

void Decoder::ReportPerformance(bool final) noexcept {
  if (!performance_enabled_ || performance_final_) return;
  internal::PerformanceReport report;
  if (!performance_.Sample(report, final, PerformanceClock::now())) return;
  const auto rate = [](std::uint64_t count, double seconds) noexcept {
    return seconds > 0 ? static_cast<double>(count) / seconds : 0.0;
  };
  const auto& selected = final ? report.total : report.interval;
  const auto seconds = final ? report.elapsed_seconds : report.interval_seconds;
  char selected_speed[32] = "N/A";
  if (selected.has_media && selected.media_ns > 0 && seconds > 0) {
    std::snprintf(selected_speed, sizeof(selected_speed), "%.3f",
                  selected.media_ns / 1000000000.0 / seconds);
  }
  const auto* codec = context_.get()->codec;
  const auto* backend = context_.get()->hw_device_ctx ? "cuda" : "cpu";
  const auto api_ms_per_frame =
      selected.frames ? selected.work_ns / 1000000.0 / selected.frames : 0.0;
  if (context_.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
    MW_LOG_INFO(performance_module_,
                "Decoder instance={} report={} codec={} backend={} fps={:.1f} "
                "speed={} api_ms_per_frame={:.2f} errors={}",
                static_cast<const void*>(this), final ? "summary" : "interval",
                codec ? codec->name : "?", backend,
                rate(selected.frames, seconds), selected_speed,
                api_ms_per_frame, selected.errors);
  } else {
    MW_LOG_INFO(
        performance_module_,
        "Decoder instance={} report={} codec={} backend={} "
        "samples_per_second={:.1f} speed={} api_ms_per_frame={:.2f} errors={}",
        static_cast<const void*>(this), final ? "summary" : "interval",
        codec ? codec->name : "?", backend, rate(selected.samples, seconds),
        selected_speed, api_ms_per_frame, selected.errors);
  }
  if (final) performance_final_ = true;
  if (!performance_trace_enabled_) return;
  char speed[32] = "N/A";
  if (report.total.has_media && report.elapsed_seconds > 0) {
    std::snprintf(
        speed, sizeof(speed), "%.3f",
        report.total.media_ns / 1000000000.0 / report.elapsed_seconds);
  }
  MW_LOG_TRACE(
      performance_module_,
      "Decoder instance={} stream={} codec={} backend={} final={} packets={} "
      "bytes={} frames={} fps={:.3f} fps_now={:.3f} samples={} "
      "samples_per_second={:.3f} samples_per_second_now={:.3f} speed={} "
      "elapsed_s={:.3f} interval_s={:.3f} api_work_calls={} api_work_ms={:.3f} "
      "api_work_mean_ms={:.3f} "
      "api_work_ms_now={:.3f} api_work_max_ms={:.3f} "
      "api_work_max_ms_now={:.3f} eagain={} eof={} errors={} (API wall time, "
      "not GPU "
      "kernel "
      "time)",
      static_cast<const void*>(this), stream_index_, codec ? codec->name : "?",
      context_.get()->hw_device_ctx ? "cuda" : "cpu", final,
      report.total.packets, report.total.bytes, report.total.frames,
      rate(report.total.frames, report.elapsed_seconds),
      rate(report.interval.frames, report.interval_seconds),
      report.total.samples, rate(report.total.samples, report.elapsed_seconds),
      rate(report.interval.samples, report.interval_seconds), speed,
      report.elapsed_seconds, report.interval_seconds, report.total.work_calls,
      report.total.work_ns / 1000000.0,
      report.total.work_calls
          ? report.total.work_ns / 1000000.0 / report.total.work_calls
          : 0.0,
      report.interval.work_ns / 1000000.0, report.total.max_work_ns / 1000000.0,
      report.interval.max_work_ns / 1000000.0, report.total.again,
      report.total.eof, report.total.errors);
}

}  // namespace mw::streamer::ffmpeg
