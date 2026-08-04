#include "mw/pipeline/file_pipeline.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include "mw/common/thread.h"
#include "mw/decoder/audio_decoder.h"
#include "mw/decoder/video_decoder.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/log/logging.h"
#include "mw/performance/internal/local_file_collector.h"
#include "mw/performance/internal/stopwatch.h"
#include "mw/processor/file_processor_handler.h"
#include "mw/processor/internal/source_info_adapter.h"
#include "mw/resampler/audio_resampler.h"

namespace mw::streamer::pipeline {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

constexpr AVRational kMicrosecondTimeBase{1, AV_TIME_BASE};

class ProcessingCancelled final : public std::exception {};

ffmpeg::StreamInfo MakeStreamInfo(const AVStream& stream) {
  ffmpeg::StreamInfo info;
  info.stream_index = stream.index;
  info.codec_parameters = ffmpeg::CodecParameters(*stream.codecpar);
  info.time_base = stream.time_base;
  info.Validate();
  return info;
}

int FindBestStream(AVFormatContext& context, AVMediaType media_type) {
  const int best =
      av_find_best_stream(&context, media_type, -1, -1, nullptr, 0);
  if (best < 0) {
    return -1;
  }
  if (media_type != AVMEDIA_TYPE_VIDEO ||
      (context.streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
    return best;
  }
  for (unsigned int index = 0; index < context.nb_streams; ++index) {
    const auto* stream = context.streams[index];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

std::chrono::microseconds RescaleToMicroseconds(std::int64_t value,
                                                AVRational time_base) {
  return std::chrono::microseconds{
      av_rescale_q(value, time_base, kMicrosecondTimeBase)};
}

std::chrono::microseconds FindTimelineOrigin(
    const AVFormatContext& context, const std::optional<int>& audio_index,
    const std::optional<int>& video_index) {
  if (context.start_time != AV_NOPTS_VALUE) {
    return std::chrono::microseconds{context.start_time};
  }

  std::optional<std::chrono::microseconds> origin;
  for (const auto index : {audio_index, video_index}) {
    if (!index || context.streams[*index]->start_time == AV_NOPTS_VALUE) {
      continue;
    }
    const auto value =
        RescaleToMicroseconds(context.streams[*index]->start_time,
                              context.streams[*index]->time_base);
    origin = origin ? std::min(*origin, value) : value;
  }
  return origin.value_or(std::chrono::microseconds::zero());
}

std::optional<std::chrono::microseconds> FindDuration(
    const AVFormatContext& context, const std::optional<int>& audio_index,
    const std::optional<int>& video_index,
    std::chrono::microseconds timeline_origin) {
  if (context.duration != AV_NOPTS_VALUE && context.duration > 0) {
    return std::chrono::microseconds{context.duration};
  }

  std::optional<std::chrono::microseconds> duration;
  for (const auto index : {audio_index, video_index}) {
    if (!index || context.streams[*index]->duration == AV_NOPTS_VALUE ||
        context.streams[*index]->duration <= 0) {
      continue;
    }
    const auto* stream = context.streams[*index];
    const auto start =
        stream->start_time == AV_NOPTS_VALUE
            ? timeline_origin
            : RescaleToMicroseconds(stream->start_time, stream->time_base);
    const auto end =
        start + RescaleToMicroseconds(stream->duration, stream->time_base);
    const auto value =
        std::max(end - timeline_origin, std::chrono::microseconds::zero());
    duration = duration ? std::max(*duration, value) : value;
  }
  return duration;
}

std::optional<std::chrono::microseconds> FrameEndPosition(
    const ffmpeg::Frame& frame, std::chrono::microseconds origin) {
  if (!frame.get() || frame->pts == AV_NOPTS_VALUE ||
      frame->time_base.num <= 0 || frame->time_base.den <= 0) {
    return std::nullopt;
  }
  auto end = RescaleToMicroseconds(frame->pts, frame->time_base);
  if (frame->duration > 0) {
    end += RescaleToMicroseconds(frame->duration, frame->time_base);
  }
  return std::max(end - origin, std::chrono::microseconds::zero());
}

}  // namespace

class FilePipeline::Impl final {
 public:
  explicit Impl(LocalFilePipelineConfig config) : config_(std::move(config)) {}

  void SetProcessorCallbacks(
      const MwStreamerFileProcessorCallbacks& callbacks) {
    RequireIdle("设置Processor回调");
    processor_callbacks_ = callbacks;
  }

  void SetOnStatus(OnStatus callback) {
    RequireIdle("设置Pipeline状态回调");
    on_status_ = std::move(callback);
  }

  void Start() {
    RequireIdle("启动FilePipeline");
    if (config_.input_path.empty()) {
      throw std::invalid_argument("FilePipeline输入文件路径不能为空");
    }

    performance_.Reset();
    exit_reason_.store(ExitReason::kNone, std::memory_order_release);
    SetStatus(FilePipelineStatus::kStarting);
    try {
      worker_ = std::make_unique<common::Thread>("file-pipeline",
                                                 [this]() { Run(); });
    } catch (...) {
      SetStatus(FilePipelineStatus::kFailed);
      throw;
    }
    Log::Info("FilePipeline开始启动: input={}", config_.input_path);
  }

  void UpdateProcessorConfig(std::string config) {
    if (status() == FilePipelineStatus::kIdle) {
      config_.processor.config = std::move(config);
      return;
    }
    if (status() != FilePipelineStatus::kStarting &&
        status() != FilePipelineStatus::kRunning) {
      throw std::logic_error("更新File Processor配置只能在启动前或运行中执行");
    }

    std::lock_guard<std::mutex> lock(processor_lifecycle_mutex_);
    const auto current_status = status();
    if (current_status != FilePipelineStatus::kStarting &&
        current_status != FilePipelineStatus::kRunning) {
      throw std::logic_error("更新File Processor配置只能在启动前或运行中执行");
    }
    if (current_status == FilePipelineStatus::kRunning && !processor_) {
      throw std::logic_error("File Processor已经停止");
    }
    config_.processor.config = std::move(config);
    if (processor_) {
      processor_->UpdateConfig(config_.processor.config);
    }
  }

  void Stop() noexcept {
    ExitReason expected = ExitReason::kNone;
    static_cast<void>(exit_reason_.compare_exchange_strong(
        expected, ExitReason::kCancelled, std::memory_order_acq_rel));

    if (worker_) {
      try {
        worker_->Join();
      } catch (const std::exception& error) {
        Log::Error("FilePipeline等待工作线程失败: {}", error.what());
      } catch (...) {
        Log::Error("FilePipeline等待工作线程失败: 未知异常");
      }
      worker_.reset();
    }

    if (status() != FilePipelineStatus::kFailed &&
        status() != FilePipelineStatus::kStopped) {
      SetStatus(FilePipelineStatus::kStopped);
    }
  }

  FilePipelineStatus status() const noexcept {
    return status_.load(std::memory_order_acquire);
  }

  performance::LocalFilePipelineSnapshot CollectPerformance() {
    return performance_.Collect();
  }

 private:
  enum class ExitReason {
    kNone,
    kNatural,
    kCancelled,
    kFailed,
  };

  struct ProcessingContext {
    std::optional<ffmpeg::StreamInfo> audio_stream;
    std::optional<ffmpeg::StreamInfo> video_stream;
    std::unique_ptr<decoder::VideoDecoder> video_decoder;
    std::unique_ptr<decoder::AudioDecoder> audio_decoder;
    std::unique_ptr<resampler::AudioResampler> audio_resampler;
    std::unique_ptr<processor::FileProcessorHandler> processor;
    int audio_stream_index = -1;
    int video_stream_index = -1;
    std::chrono::microseconds timeline_origin{0};
  };

  void RequireIdle(const char* operation) const {
    if (status() != FilePipelineStatus::kIdle) {
      throw std::logic_error(fmt::format("{}只能在Idle状态执行", operation));
    }
  }

  static int InterruptIo(void* opaque) noexcept {
    const auto* self = static_cast<const Impl*>(opaque);
    return self && self->exit_reason_.load(std::memory_order_acquire) ==
                       ExitReason::kCancelled;
  }

  ffmpeg::InputFormatContext OpenInput() {
    ffmpeg::InputFormatContext context(config_.input_path, {InterruptIo, this});
    context.FindStreamInfo();
    return context;
  }

  ProcessingContext BuildProcessingContext(AVFormatContext& format_context) {
    ProcessingContext context;
    const int audio_index = FindBestStream(format_context, AVMEDIA_TYPE_AUDIO);
    const int video_index = FindBestStream(format_context, AVMEDIA_TYPE_VIDEO);
    if (audio_index < 0 && video_index < 0) {
      throw std::invalid_argument("FilePipeline输入不包含可处理的音频或视频");
    }
    const std::optional<int> optional_audio_index =
        audio_index >= 0 ? std::optional<int>{audio_index} : std::nullopt;
    const std::optional<int> optional_video_index =
        video_index >= 0 ? std::optional<int>{video_index} : std::nullopt;

    if (optional_audio_index) {
      context.audio_stream =
          MakeStreamInfo(*format_context.streams[*optional_audio_index]);
      context.audio_stream_index = *optional_audio_index;
    }
    if (optional_video_index) {
      context.video_stream =
          MakeStreamInfo(*format_context.streams[*optional_video_index]);
      context.video_stream_index = *optional_video_index;
      context.video_decoder = std::make_unique<decoder::VideoDecoder>(
          *context.video_stream, config_.video_decoder);
    }
    if (context.audio_stream) {
      context.audio_decoder = std::make_unique<decoder::AudioDecoder>(
          *context.audio_stream, config_.audio_decoder);
      context.audio_resampler = std::make_unique<resampler::AudioResampler>(
          context.audio_decoder->stream_info());
    }

    context.timeline_origin = FindTimelineOrigin(
        format_context, optional_audio_index, optional_video_index);
    const auto duration =
        FindDuration(format_context, optional_audio_index, optional_video_index,
                     context.timeline_origin);
    performance_.Configure(
        context.audio_stream.has_value(), context.video_stream.has_value(),
        duration.has_value(), duration.value_or(std::chrono::microseconds{}));

    context.processor = std::make_unique<processor::FileProcessorHandler>(
        processor::internal::MakeProcessorSourceInfo(context.audio_stream,
                                                     context.video_stream),
        context.video_decoder ? context.video_decoder->hardware_context()
                              : nullptr);
    return context;
  }

  void StartProcessor(ProcessingContext& context) {
    std::lock_guard<std::mutex> lock(processor_lifecycle_mutex_);
    const MwStreamerFileProcessorConfig processor_config{
        config_.processor.config.c_str()};
    if (context.processor->Start(processor_config, processor_callbacks_) !=
        kMwStreamerProcessorStartSuccess) {
      throw std::runtime_error("Processor拒绝启动");
    }
    BindProcessingCallbacks(context);
    processor_ = context.processor.get();
  }

  void BindProcessingCallbacks(ProcessingContext& context) {
    if (context.audio_decoder) {
      context.audio_decoder->SetOnFrame([&context](const ffmpeg::Frame& frame) {
        context.audio_resampler->Resample(frame);
      });
      context.audio_resampler->SetOnFrame(
          [this, &context](const ffmpeg::Frame& frame) {
            ProcessAudioFrame(context, frame);
          });
    }
    if (context.video_decoder) {
      context.video_decoder->SetOnFrame(
          [this, &context](const ffmpeg::Frame& frame) {
            ProcessVideoFrame(context, frame);
          });
    }
  }

  void ProcessAudioFrame(ProcessingContext& context,
                         const ffmpeg::Frame& frame) {
    ThrowIfCancelled();
    performance::internal::Stopwatch stopwatch;
    stopwatch.Measure(
        [&context, &frame]() { context.processor->ProcessAudio(frame); });
    performance_.audio().process().Record(
        static_cast<std::uint64_t>(frame->nb_samples), stopwatch.elapsed());
    if (const auto position =
            FrameEndPosition(frame, context.timeline_origin)) {
      performance_.RecordAudioPosition(*position);
    }
    ThrowIfCancelled();
  }

  void ProcessVideoFrame(ProcessingContext& context,
                         const ffmpeg::Frame& frame) {
    ThrowIfCancelled();
    performance::internal::Stopwatch stopwatch;
    stopwatch.Measure(
        [&context, &frame]() { context.processor->ProcessVideo(frame); });
    performance_.video().process().Record(1, stopwatch.elapsed());
    if (const auto position =
            FrameEndPosition(frame, context.timeline_origin)) {
      performance_.RecordVideoPosition(*position);
    }
    ThrowIfCancelled();
  }

  void DecodePacket(ProcessingContext& context, const ffmpeg::Packet& packet) {
    if (packet->stream_index == context.audio_stream_index) {
      const auto result = context.audio_decoder->Decode(packet);
      performance_.audio().decode().Record(result.samples, result.service_time);
    } else if (packet->stream_index == context.video_stream_index) {
      const auto result = context.video_decoder->Decode(packet);
      performance_.video().decode().Record(result.frames, result.service_time);
    }
  }

  void Drain(ProcessingContext& context) {
    if (context.audio_decoder) {
      const auto result = context.audio_decoder->Drain();
      performance_.audio().decode().Record(result.samples, result.service_time);
      context.audio_resampler->Drain();
    }
    if (context.video_decoder) {
      const auto result = context.video_decoder->Drain();
      performance_.video().decode().Record(result.frames, result.service_time);
    }
  }

  void ProcessFile() {
    auto format_context = OpenInput();
    ThrowIfCancelled();
    auto processing = BuildProcessingContext(*format_context.get());
    ThrowIfCancelled();
    StartProcessor(processing);
    try {
      ThrowIfCancelled();
      SetStatus(FilePipelineStatus::kRunning);

      ffmpeg::Packet packet;
      for (;;) {
        ThrowIfCancelled();
        packet.Unref();
        if (!format_context.ReadPacket(packet)) {
          ExitReason expected = ExitReason::kNone;
          if (!exit_reason_.compare_exchange_strong(
                  expected, ExitReason::kNatural, std::memory_order_acq_rel)) {
            ThrowIfCancelled();
          }
          break;
        }
        DecodePacket(processing, packet);
      }

      Drain(processing);
      processing.processor->NotifyBoundary(kMwStreamerProcessorEndOfInput);
      performance_.MarkCompleted();
    } catch (...) {
      StopProcessor();
      throw;
    }
    StopProcessor();
  }

  void ThrowIfCancelled() const {
    if (exit_reason_.load(std::memory_order_acquire) ==
        ExitReason::kCancelled) {
      throw ProcessingCancelled();
    }
  }

  void StopProcessor() noexcept {
    std::lock_guard<std::mutex> lock(processor_lifecycle_mutex_);
    if (processor_) {
      processor_->Stop();
      processor_ = nullptr;
    }
  }

  void Run() noexcept {
    try {
      ProcessFile();
    } catch (const ProcessingCancelled&) {
    } catch (const std::exception& error) {
      ExitReason reason = exit_reason_.load(std::memory_order_acquire);
      if (reason != ExitReason::kCancelled) {
        exit_reason_.store(ExitReason::kFailed, std::memory_order_release);
        Log::Error("FilePipeline处理失败: input={}, error={}",
                   config_.input_path, error.what());
      }
    } catch (...) {
      ExitReason reason = exit_reason_.load(std::memory_order_acquire);
      if (reason != ExitReason::kCancelled) {
        exit_reason_.store(ExitReason::kFailed, std::memory_order_release);
        Log::Error("FilePipeline处理失败: input={}, error=未知异常",
                   config_.input_path);
      }
    }

    StopProcessor();
    if (exit_reason_.load(std::memory_order_acquire) == ExitReason::kFailed) {
      SetStatus(FilePipelineStatus::kFailed);
    } else {
      SetStatus(FilePipelineStatus::kStopped);
    }
    Log::Info("FilePipeline工作线程结束: input={}, failed={}",
              config_.input_path, status() == FilePipelineStatus::kFailed);
  }

  void SetStatus(FilePipelineStatus status) noexcept {
    const auto previous = status_.exchange(status, std::memory_order_acq_rel);
    if (previous == status || !on_status_) {
      return;
    }
    try {
      on_status_(status);
    } catch (const std::exception& error) {
      Log::Error("FilePipeline状态回调异常，已隔离: {}", error.what());
    } catch (...) {
      Log::Error("FilePipeline状态回调异常，已隔离: 未知异常");
    }
  }

  LocalFilePipelineConfig config_;
  MwStreamerFileProcessorCallbacks processor_callbacks_{};
  OnStatus on_status_;
  std::atomic<FilePipelineStatus> status_{FilePipelineStatus::kIdle};
  std::atomic<ExitReason> exit_reason_{ExitReason::kNone};
  std::mutex processor_lifecycle_mutex_;
  processor::FileProcessorHandler* processor_ = nullptr;
  performance::internal::LocalFileCollector performance_;
  std::unique_ptr<common::Thread> worker_;
};

FilePipeline::FilePipeline(LocalFilePipelineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

FilePipeline::~FilePipeline() {
  if (impl_) {
    impl_->Stop();
  }
}

void FilePipeline::SetProcessorCallbacks(
    const MwStreamerFileProcessorCallbacks& callbacks) {
  impl_->SetProcessorCallbacks(callbacks);
}

void FilePipeline::SetOnStatus(OnStatus callback) {
  impl_->SetOnStatus(std::move(callback));
}

void FilePipeline::Start() { impl_->Start(); }

void FilePipeline::UpdateProcessorConfig(std::string config) {
  impl_->UpdateProcessorConfig(std::move(config));
}

void FilePipeline::Stop() noexcept { impl_->Stop(); }

FilePipelineStatus FilePipeline::status() const noexcept {
  return impl_->status();
}

performance::LocalFilePipelineSnapshot FilePipeline::CollectPerformance() {
  return impl_->CollectPerformance();
}

}  // namespace mw::streamer::pipeline
