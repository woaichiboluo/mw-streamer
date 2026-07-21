#include "mw/streamer/pipeline.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

#include "fmt/format.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/internal/aac_encoder.h"
#include "mw/streamer/internal/aligned_audio_buffer.h"
#include "mw/streamer/internal/audio_auto_decoder.h"
#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/common/blocking_queue.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/reconnect_backoff.h"
#include "mw/streamer/internal/common/timestamp.h"
#include "mw/streamer/internal/compressed_packet_delay_buffer.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/demuxer.h"
#include "mw/streamer/internal/gpu_frame_utils.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"
#include "mw/streamer/internal/input_mode.h"
#include "mw/streamer/internal/input_options.h"
#include "mw/streamer/internal/nv_decoder.h"
#include "mw/streamer/internal/nv_encoder.h"
#include "mw/streamer/internal/output_worker.h"
#include "mw/streamer/internal/pipeline_config_validator.h"
#include "mw/streamer/internal/processor.h"
#include "mw/streamer/internal/processor_context.h"
#include "mw/streamer/internal/standby_image.h"
#include "mw/streamer/internal/video_color.h"
#include "mw/streamer/internal/video_synchronizer.h"
#include "spdlog/spdlog.h"

namespace mw::streamer {
namespace {

Status ExceptionToStatus() noexcept {
  try {
    throw;
  } catch (const internal::Error& error) {
    return Status(error.code(), error.what());
  } catch (const std::exception& error) {
    return Status(StatusCode::kInternal, error.what());
  } catch (...) {
    return Status(StatusCode::kInternal, "unknown pipeline failure");
  }
}

internal::NvEncoderConfig MakeNvEncoderConfig(
    const VideoEncoderConfig& config, const MwVideoOutputInfo& output_info) {
  internal::NvEncoderConfig result;
  result.codec_id =
      config.codec == VideoCodec::kH264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
  result.width = config.width;
  result.height = config.height;
  result.frame_rate = {config.frame_rate.numerator,
                       config.frame_rate.denominator};
  result.gop_size = config.gop_size;
  if (result.gop_size == 0) {
    result.gop_size = static_cast<int>(
        av_rescale_q(2, AVRational{1, 1}, av_inv_q(result.frame_rate)));
  }
  result.bit_rate = config.bit_rate;
  result.max_bit_rate = config.maximum_bit_rate;
  result.vbv_buffer_size = config.vbv_buffer_size;
  result.rate_control = config.rate_control == VideoRateControl::kCbr
                            ? internal::NvEncoderRateControl::kCbr
                            : internal::NvEncoderRateControl::kVbr;
  result.preset = config.preset;
  result.max_b_frames = config.maximum_b_frames;
  result.color_range = internal::ToAvColorRange(output_info.color_range);
  result.color_space = internal::ToAvColorSpace(output_info.color_space);
  result.options = config.options;
  return result;
}

internal::StreamInfo CloneStreamInfo(const internal::StreamInfo& source) {
  internal::StreamInfo result;
  result.index = source.index;
  result.time_base = source.time_base;
  result.frame_rate = source.frame_rate;
  result.codec_parameters = source.codec_parameters.Clone();
  return result;
}

bool IsRetryableInputError(const Status& status) noexcept {
  return status.code() == StatusCode::kUnavailable ||
         status.code() == StatusCode::kFfmpegError;
}

}  // namespace

class Pipeline::Impl final {
 public:
  explicit Impl(PipelineConfig config) : config_(std::move(config)) {}

  ~Impl() {
    Stop();
    JoinCoordinator();
    JoinNotificationThread();
  }

  Status Start() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (state_.load() != PipelineState::kCreated) {
      return Status(StatusCode::kInvalidState,
                    "pipeline can only be started once");
    }

    StartNotificationThread();
    SetState(PipelineState::kStarting, Status::Ok());
    try {
      internal::ValidatePipelineConfig(config_);
      Initialize();
      LaunchWorkers();
      SetState(PipelineState::kRunning, Status::Ok());
      coordinator_thread_ = std::thread([this] { Coordinate(); });
      return Status::Ok();
    } catch (...) {
      const Status status = ExceptionToStatus();
      stop_requested_.store(true);
      CancelAll();
      lifecycle_lock.unlock();
      JoinWorkers();
      StopProcessor();
      Complete(PipelineState::kFailed, status);
      JoinNotificationThread();
      return status;
    }
  }

  Status Stop() {
    {
      std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
      const PipelineState current = state_.load();
      if (current == PipelineState::kCreated) {
        state_.store(PipelineState::kStopped);
        SetTerminalStatus(Status::Ok());
        return Status::Ok();
      }
      if (IsTerminal(current)) {
        return TerminalStatus();
      }
      if (!stop_requested_.exchange(true)) {
        SetState(PipelineState::kStopping, Status::Ok());
        CancelAll();
      }
    }

    JoinCoordinator();
    JoinNotificationThread();
    return TerminalStatus();
  }

  Status Wait() {
    if (state_.load() == PipelineState::kCreated) {
      return Status(StatusCode::kInvalidState, "pipeline has not been started");
    }
    {
      std::unique_lock<std::mutex> terminal_lock(terminal_mutex_);
      terminal_cv_.wait(terminal_lock, [this] { return terminal_; });
    }
    JoinCoordinator();
    JoinNotificationThread();
    return TerminalStatus();
  }

  Status UpdateProcessorConfig(std::string config) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (state_.load() != PipelineState::kRunning || processor_ == nullptr) {
      return Status(StatusCode::kInvalidState,
                    "Processor configuration can only be updated while the "
                    "pipeline is running");
    }
    try {
      processor_->UpdateConfig(config);
      return Status::Ok();
    } catch (...) {
      return ExceptionToStatus();
    }
  }

  PipelineState state() const noexcept { return state_.load(); }

 private:
  enum class ReconnectResult {
    kReconnected,
    kStopped,
    kExhausted,
  };

  struct StateNotification {
    PipelineState state = PipelineState::kCreated;
    Status status;
  };

  struct VideoPacketMessage {
    std::uint64_t generation = 0;
    std::shared_ptr<const internal::StreamInfo> stream;
    std::optional<std::int64_t> timeline_offset_us;
    ffmpeg::Packet packet;
  };

  struct AudioPacketMessage {
    std::uint64_t generation = 0;
    std::shared_ptr<const internal::StreamInfo> stream;
    std::optional<std::int64_t> timeline_offset_us;
    ffmpeg::Packet packet;
  };

  struct DecodedVideoMessage {
    std::size_t input_index = 0;
    std::int64_t timeline_pts_us = 0;
    ffmpeg::Frame frame;
  };

  struct GenerationInfo {
    std::shared_ptr<const internal::StreamInfo> video;
    std::shared_ptr<const internal::StreamInfo> audio;
  };

  using Notification = std::variant<StateNotification, EndpointEvent>;

  struct InputRuntime {
    std::size_t input_index = 0;
    std::unique_ptr<internal::FfmpegInterrupt> interrupt;
    std::unique_ptr<internal::Demuxer> demuxer;
    std::unique_ptr<internal::NvDecoder> video_decoder;
    std::unique_ptr<internal::AudioAutoDecoder> audio_decoder;
    std::unique_ptr<internal::AudioConverter> audio_converter;
    std::unique_ptr<internal::AlignedAudioBuffer> aligned_audio;
    std::unique_ptr<internal::BlockingQueue<VideoPacketMessage>> video_packets;
    std::unique_ptr<internal::BlockingQueue<AudioPacketMessage>> audio_packets;
    std::unique_ptr<internal::BlockingQueue<DecodedVideoMessage>>
        decoded_video_frames;
    std::shared_ptr<const internal::StreamInfo> current_video_stream;
    std::shared_ptr<const internal::StreamInfo> current_audio_stream;
    std::uint64_t video_decoder_generation = 0;
    std::uint64_t audio_decoder_generation = 0;
    std::int64_t video_timeline_offset_us = 0;
    bool video_timeline_offset_initialized = true;
    std::int64_t audio_timeline_offset_frames = 0;
    bool audio_timeline_offset_initialized = true;
    std::uint64_t generation = 0;
    std::uint64_t packet_sequence = 0;
    std::optional<std::int64_t> generation_offset_us;
    AVCodecID original_video_codec_id = AV_CODEC_ID_NONE;
    AVCodecID original_audio_codec_id = AV_CODEC_ID_NONE;
    std::map<std::uint64_t, GenerationInfo> generation_infos;
    std::mutex generation_mutex;
    std::mutex audio_mutex;
    std::condition_variable audio_space_cv;
    std::atomic<std::int64_t> latest_video_timeline_us{AV_NOPTS_VALUE};
    std::atomic<bool> audio_end_of_stream{false};
    std::thread demux_thread;
    std::thread delay_thread;
    std::thread video_decode_thread;
    std::thread audio_decode_thread;
  };

  static bool IsTerminal(PipelineState state) noexcept {
    return state == PipelineState::kCompleted ||
           state == PipelineState::kFailed || state == PipelineState::kStopped;
  }

  InputRuntime& PrimaryInput() noexcept { return *inputs_.front(); }

  const InputRuntime& PrimaryInput() const noexcept { return *inputs_.front(); }

  bool UsesDelayedPacketScheduler() const noexcept {
    return config_.live_delay.count() > 0 &&
           std::all_of(config_.inputs.begin(), config_.inputs.end(),
                       internal::IsLiveInput);
  }

  bool IsLiveProcessing(std::size_t input_index = 0) const noexcept {
    return internal::IsLiveInput(config_.inputs[input_index]);
  }

  std::chrono::microseconds VideoFrameInterval() const noexcept {
    const std::int64_t numerator =
        static_cast<std::int64_t>(
            config_.video_encoder.frame_rate.denominator) *
        1'000'000;
    return std::chrono::microseconds(std::max<std::int64_t>(
        1, numerator / config_.video_encoder.frame_rate.numerator));
  }

  static std::chrono::microseconds AudioFrameInterval() noexcept {
    return std::chrono::microseconds(
        static_cast<std::int64_t>(internal::kProcessorAudioBlockFrameCount) *
        1'000'000 / internal::kProcessorAudioSampleRate);
  }

  void Initialize() {
    cuda_device_ = std::make_unique<internal::CudaDeviceContext>();
    cuda_device_->Create(config_.device_id);

    bool has_audio = false;
    std::vector<MwInputSourceInfo> sources;
    sources.reserve(config_.inputs.size());
    for (std::size_t input_index = 0; input_index < config_.inputs.size();
         ++input_index) {
      InitializeInput(input_index, &sources, &has_audio);
    }
    frame_pool_ = std::make_unique<internal::GpuNv12FramePool>();
    frame_pool_->Create(
        *cuda_device_, config_.video_encoder.width,
        config_.video_encoder.height,
        static_cast<int>(config_.queues.decoded_video_capacity));

    const MwProcessorContext context = internal::MakeProcessorContext(
        config_.device_id, sources,
        config_.synchronization.primary_video_input_index,
        config_.video_encoder, has_audio);
    processor_ = std::make_unique<internal::Processor>(
        config_.processor_callbacks, context);

    black_frame_ =
        std::make_unique<ffmpeg::Frame>(internal::CreateBlackGpuNv12Frame(
            frame_pool_.get(),
            internal::ToAvColorRange(context.video_output.color_range),
            internal::ToAvColorSpace(context.video_output.color_space)));
    standby_image_ = std::make_unique<internal::StandbyImage>();
    standby_image_->Load(
        config_.standby_image_path, frame_pool_.get(),
        internal::ToAvColorRange(context.video_output.color_range),
        internal::ToAvColorSpace(context.video_output.color_space));
    processor_started_ = true;
    processor_->Start(config_.processor_initial_config);

    video_encoder_ = std::make_unique<internal::NvEncoder>();
    video_encoder_->Open(
        MakeNvEncoderConfig(config_.video_encoder, context.video_output),
        *cuda_device_, *frame_pool_);
    if (has_audio) {
      audio_encoder_ = std::make_unique<internal::AacEncoder>();
      audio_encoder_->Open(config_.audio_encoder.bit_rate);
    }

    for (const OutputConfig& output : config_.outputs) {
      auto worker = std::make_unique<internal::OutputWorker>(
          output, video_encoder_->codec_parameters(),
          video_encoder_->time_base(),
          has_audio ? &audio_encoder_->codec_parameters() : nullptr,
          has_audio ? audio_encoder_->time_base() : AVRational{0, 1},
          [this, endpoint_id = output.id](EndpointEventType type,
                                          const Status& status) {
            NotifyEndpointEvent(EndpointKind::kOutput, endpoint_id, type,
                                status);
          });
      output_workers_.push_back(std::move(worker));
      output_workers_.back()->Start();
    }
    if (UsesDelayedPacketScheduler()) {
      delayed_packets_ =
          std::make_unique<internal::CompressedPacketDelayBuffer>(
              inputs_.size(), config_.live_delay);
      for (const std::unique_ptr<InputRuntime>& input : inputs_) {
        StoreGenerationInfo(input->input_index, input->generation);
      }
    }
    remaining_encoder_workers_.store(has_audio ? 2 : 1);
  }

  void InitializeInput(std::size_t input_index,
                       std::vector<MwInputSourceInfo>* sources,
                       bool* pipeline_has_audio) {
    const InputConfig& config = config_.inputs[input_index];
    auto input = std::make_unique<InputRuntime>();
    input->input_index = input_index;
    input->interrupt = std::make_unique<internal::FfmpegInterrupt>();
    input->demuxer = std::make_unique<internal::Demuxer>();
    input->demuxer->Open(config.url, internal::MakeDemuxerOpenOptions(
                                         config, input->interrupt.get()));

    input->current_video_stream = std::make_shared<const internal::StreamInfo>(
        CloneStreamInfo(input->demuxer->video_stream()));
    input->video_decoder = std::make_unique<internal::NvDecoder>();
    input->video_decoder->Open(*input->current_video_stream, *cuda_device_);
    const bool has_audio = input->demuxer->has_audio();
    if (has_audio) {
      input->current_audio_stream =
          std::make_shared<const internal::StreamInfo>(
              CloneStreamInfo(input->demuxer->audio_stream()));
      input->audio_decoder = std::make_unique<internal::AudioAutoDecoder>();
      input->audio_decoder->Open(*input->current_audio_stream);
      input->audio_converter = std::make_unique<internal::AudioConverter>();
      input->aligned_audio = std::make_unique<internal::AlignedAudioBuffer>(
          config_.queues.audio_fifo_capacity_frames);
      input->audio_packets =
          std::make_unique<internal::BlockingQueue<AudioPacketMessage>>(
              config.packet_queue_capacity);
    }

    input->original_video_codec_id =
        input->demuxer->video_stream().codec_parameters.get()->codec_id;
    if (has_audio) {
      input->original_audio_codec_id =
          input->demuxer->audio_stream().codec_parameters.get()->codec_id;
    }
    input->video_packets =
        std::make_unique<internal::BlockingQueue<VideoPacketMessage>>(
            config.packet_queue_capacity);
    input->decoded_video_frames =
        std::make_unique<internal::BlockingQueue<DecodedVideoMessage>>(
            config_.queues.decoded_video_capacity);
    sources->push_back(internal::MakeInputSourceInfo(
        input_index, input->demuxer->video_stream(),
        has_audio ? &input->demuxer->audio_stream() : nullptr));
    *pipeline_has_audio = *pipeline_has_audio || has_audio;
    inputs_.push_back(std::move(input));
  }

  void LaunchWorkers() {
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      const std::size_t input_index = input->input_index;
      if (delayed_packets_ != nullptr) {
        input->delay_thread = std::thread([this, input_index] {
          RunGuarded([this, input_index] { DelayLoop(input_index); });
        });
      }
      input->demux_thread = std::thread([this, input_index] {
        RunGuarded([this, input_index] { DemuxLoop(input_index); });
      });
      input->video_decode_thread = std::thread([this, input_index] {
        RunGuarded([this, input_index] { VideoDecodeLoop(input_index); });
      });
      if (input->audio_decoder != nullptr) {
        input->audio_decode_thread = std::thread([this, input_index] {
          RunGuarded([this, input_index] { AudioDecodeLoop(input_index); });
        });
      }
    }
    video_thread_ =
        std::thread([this] { RunEncoderGuarded([this] { VideoLoop(); }); });
    if (audio_encoder_ != nullptr) {
      audio_thread_ =
          std::thread([this] { RunEncoderGuarded([this] { AudioLoop(); }); });
    }
  }

  template <typename Function>
  void RunGuarded(Function function) noexcept {
    try {
      function();
    } catch (...) {
      if (!stop_requested_.load()) {
        Fail(ExceptionToStatus());
      }
    }
  }

  template <typename Function>
  void RunEncoderGuarded(Function function) noexcept {
    RunGuarded(std::move(function));
    if (remaining_encoder_workers_.fetch_sub(1) == 1) {
      for (const std::unique_ptr<internal::OutputWorker>& output :
           output_workers_) {
        output->Close();
      }
    }
  }

  void DemuxLoop(std::size_t input_index) {
    InputRuntime& input = *inputs_[input_index];
    internal::ReconnectBackoff reconnect_backoff(
        config_.inputs[input_index].reconnect);
    bool recovering = false;
    while (!stop_requested_.load() && !failed_.load()) {
      std::optional<ffmpeg::Packet> packet;
      try {
        packet = input.demuxer->Read();
      } catch (...) {
        const Status status = ExceptionToStatus();
        if (!CanReconnectAfterError(input_index, status)) {
          internal::ThrowError(status.code(), status.message());
        }
        const ReconnectResult reconnect_result =
            ReconnectInput(input_index, status, &reconnect_backoff);
        if (reconnect_result != ReconnectResult::kReconnected) {
          return;
        }
        recovering = true;
        continue;
      }
      if (!packet.has_value()) {
        if (ShouldReconnectAfterEof(input_index)) {
          const Status status(StatusCode::kUnavailable,
                              "live input reached an unexpected end of stream");
          if (ReconnectInput(input_index, status, &reconnect_backoff) !=
              ReconnectResult::kReconnected) {
            return;
          }
          recovering = true;
          continue;
        }
        input.video_packets->Close();
        if (input.audio_packets != nullptr) {
          input.audio_packets->Close();
        }
        return;
      }

      const int stream_index = packet->get()->stream_index;
      if (recovering) {
        recovering = false;
        reconnect_backoff.Reset();
        NotifyInputEndpointEvent(input_index, EndpointEventType::kRecovered,
                                 Status::Ok());
      }
      if (!QueueCompressedPacket(input_index, std::move(*packet),
                                 stream_index)) {
        return;
      }
    }
  }

  bool QueueCompressedPacket(std::size_t input_index, ffmpeg::Packet packet,
                             int stream_index) {
    InputRuntime& input = *inputs_[input_index];
    const bool is_video = stream_index == input.demuxer->video_stream().index;
    const bool is_audio = input.audio_decoder != nullptr &&
                          stream_index == input.demuxer->audio_stream().index;
    if (!is_video && !is_audio) {
      return true;
    }
    if (delayed_packets_ == nullptr) {
      if (is_video) {
        VideoPacketMessage message{input.generation, input.current_video_stream,
                                   std::nullopt, std::move(packet)};
        return IsLiveProcessing(input_index)
                   ? input.video_packets->PushDroppingOldest(std::move(message))
                   : input.video_packets->Push(std::move(message));
      }
      AudioPacketMessage message{input.generation, input.current_audio_stream,
                                 std::nullopt, std::move(packet)};
      return IsLiveProcessing(input_index)
                 ? input.audio_packets->PushDroppingOldest(std::move(message))
                 : input.audio_packets->Push(std::move(message));
    }

    const AVPacket* raw_packet = packet.get();
    if (raw_packet->dts == AV_NOPTS_VALUE ||
        !internal::IsValidTimeBase(raw_packet->time_base)) {
      internal::ThrowError(
          StatusCode::kUnsupported,
          "live delayed input packet does not contain a valid DTS");
    }
    const std::int64_t source_dts_us =
        internal::ToMicroseconds(raw_packet->dts, raw_packet->time_base);
    if (!input.generation_offset_us.has_value()) {
      std::int64_t target_dts_us = source_dts_us;
      if (const std::optional<std::int64_t> last =
              delayed_packets_->last_timeline_dts_us(input_index)) {
        target_dts_us = *last + 1;
      }
      if (const std::optional<std::int64_t> cursor =
              delayed_packets_->playback_cursor_us()) {
        const std::int64_t delayed_cursor =
            *cursor + std::chrono::duration_cast<std::chrono::microseconds>(
                          config_.live_delay)
                          .count();
        target_dts_us = std::max(target_dts_us, delayed_cursor);
      }
      input.generation_offset_us = target_dts_us - source_dts_us;
    }

    internal::ScheduledPacket scheduled;
    scheduled.input_index = input_index;
    scheduled.generation = input.generation;
    scheduled.media_type = is_video ? internal::CompressedMediaType::kVideo
                                    : internal::CompressedMediaType::kAudio;
    scheduled.packet = std::move(packet);
    scheduled.source_dts_us = source_dts_us;
    scheduled.timeline_dts_us = source_dts_us + *input.generation_offset_us;
    scheduled.sequence = input.packet_sequence++;
    return delayed_packets_->Push(std::move(scheduled));
  }

  bool CanReconnectAfterError(std::size_t input_index,
                              const Status& status) const noexcept {
    return internal::IsLiveInput(config_.inputs[input_index]) &&
           IsRetryableInputError(status);
  }

  bool ShouldReconnectAfterEof(std::size_t input_index) const noexcept {
    return internal::IsLiveInput(config_.inputs[input_index]);
  }

  ReconnectResult ReconnectInput(std::size_t input_index,
                                 const Status& initial_error,
                                 internal::ReconnectBackoff* backoff) {
    InputRuntime& input = *inputs_[input_index];
    Status current_error = initial_error;
    input.demuxer.reset();
    while (!stop_requested_.load() && !failed_.load() && backoff->CanRetry()) {
      const std::chrono::milliseconds delay =
          backoff->NextDelay(jitter_distribution_(random_engine_));
      NotifyInputEndpointEvent(
          input_index, EndpointEventType::kReconnecting,
          Status(StatusCode::kUnavailable,
                 fmt::format("input reconnect attempt {} in {} ms: {}",
                             backoff->retry_count(), delay.count(),
                             current_error.message())));
      {
        std::unique_lock<std::mutex> reconnect_lock(reconnect_mutex_);
        if (reconnect_cv_.wait_for(reconnect_lock, delay, [this] {
              return stop_requested_.load() || failed_.load();
            })) {
          return ReconnectResult::kStopped;
        }
      }

      try {
        auto candidate = std::make_unique<internal::Demuxer>();
        candidate->Open(
            config_.inputs[input_index].url,
            internal::MakeDemuxerOpenOptions(config_.inputs[input_index],
                                             input.interrupt.get()));
        ValidateReconnectedInput(input_index, *candidate);
        ++input.generation;
        input.current_video_stream =
            std::make_shared<const internal::StreamInfo>(
                CloneStreamInfo(candidate->video_stream()));
        if (candidate->has_audio()) {
          input.current_audio_stream =
              std::make_shared<const internal::StreamInfo>(
                  CloneStreamInfo(candidate->audio_stream()));
        }
        if (delayed_packets_ != nullptr) {
          StoreGenerationInfo(input_index, input.generation);
          input.generation_offset_us.reset();
        }
        input.demuxer = std::move(candidate);
        return ReconnectResult::kReconnected;
      } catch (...) {
        current_error = ExceptionToStatus();
        if (!IsRetryableInputError(current_error)) {
          NotifyInputEndpointEvent(input_index, EndpointEventType::kFailed,
                                   current_error);
          return ReconnectResult::kExhausted;
        }
      }
    }

    if (stop_requested_.load() || failed_.load()) {
      return ReconnectResult::kStopped;
    }
    const Status exhausted(
        StatusCode::kUnavailable,
        fmt::format("input '{}' exhausted {} reconnect attempts: {}",
                    config_.inputs[input_index].id, backoff->retry_count(),
                    current_error.message()));
    NotifyInputEndpointEvent(input_index, EndpointEventType::kFailed,
                             exhausted);
    return ReconnectResult::kExhausted;
  }

  void ValidateReconnectedInput(std::size_t input_index,
                                const internal::Demuxer& candidate) const {
    const InputRuntime& input = *inputs_[input_index];
    const AVCodecParameters* video =
        candidate.video_stream().codec_parameters.get();
    const MwInputSourceInfo& source = processor_->context().inputs[input_index];
    if (video == nullptr || video->codec_id != input.original_video_codec_id ||
        video->width != source.video.width ||
        video->height != source.video.height ||
        candidate.has_audio() != (source.audio.present != 0)) {
      internal::ThrowError(
          StatusCode::kUnsupported,
          "reconnected input does not match its original video/audio contract");
    }
    if (candidate.has_audio()) {
      const AVCodecParameters* audio =
          candidate.audio_stream().codec_parameters.get();
      if (audio == nullptr ||
          audio->codec_id != input.original_audio_codec_id ||
          audio->sample_rate != source.audio.sample_rate ||
          audio->ch_layout.nb_channels != source.audio.channel_count) {
        internal::ThrowError(
            StatusCode::kUnsupported,
            "reconnected input does not match its original audio contract");
      }
    }
  }

  void StoreGenerationInfo(std::size_t input_index, std::uint64_t generation) {
    InputRuntime& input = *inputs_[input_index];
    GenerationInfo info;
    info.video = input.current_video_stream;
    info.audio = input.current_audio_stream;
    std::lock_guard<std::mutex> generation_lock(input.generation_mutex);
    input.generation_infos.emplace(generation, std::move(info));
  }

  GenerationInfo GetGenerationInfo(std::size_t input_index,
                                   std::uint64_t generation) {
    InputRuntime& input = *inputs_[input_index];
    std::lock_guard<std::mutex> generation_lock(input.generation_mutex);
    auto position = input.generation_infos.find(generation);
    if (position == input.generation_infos.end()) {
      internal::ThrowError(StatusCode::kInternal,
                           "delayed packet generation metadata is missing");
    }
    return position->second;
  }

  void EraseGenerationInfoBefore(std::size_t input_index,
                                 std::uint64_t generation) {
    InputRuntime& input = *inputs_[input_index];
    std::lock_guard<std::mutex> generation_lock(input.generation_mutex);
    auto position = input.generation_infos.begin();
    while (position != input.generation_infos.end() &&
           position->first < generation) {
      position = input.generation_infos.erase(position);
    }
  }

  void DelayLoop(std::size_t input_index) {
    InputRuntime& input = *inputs_[input_index];
    std::optional<std::uint64_t> released_generation;
    while (std::optional<internal::ScheduledPacket> scheduled =
               delayed_packets_->WaitPop(input_index)) {
      const GenerationInfo info =
          GetGenerationInfo(input_index, scheduled->generation);
      if (!released_generation.has_value() ||
          *released_generation != scheduled->generation) {
        EraseGenerationInfoBefore(input_index, scheduled->generation);
        released_generation = scheduled->generation;
      }
      if (scheduled->media_type == internal::CompressedMediaType::kVideo) {
        VideoPacketMessage message{
            scheduled->generation, info.video,
            scheduled->timeline_dts_us - scheduled->source_dts_us,
            std::move(scheduled->packet)};
        if (!input.video_packets->PushDroppingOldest(std::move(message))) {
          return;
        }
      } else {
        AudioPacketMessage message{
            scheduled->generation, info.audio,
            scheduled->timeline_dts_us - scheduled->source_dts_us,
            std::move(scheduled->packet)};
        if (!input.audio_packets->PushDroppingOldest(std::move(message))) {
          return;
        }
      }
    }
  }

  void VideoDecodeLoop(std::size_t input_index) {
    InputRuntime& input_runtime = *inputs_[input_index];
    while (true) {
      std::optional<VideoPacketMessage> input =
          IsLiveProcessing(input_index)
              ? input_runtime.video_packets->PopFor(VideoFrameInterval())
              : input_runtime.video_packets->Pop();
      if (!input.has_value()) {
        if (input_runtime.video_packets->is_closed()) {
          break;
        }
        continue;
      }
      if (input->generation != input_runtime.video_decoder_generation) {
        StartVideoGeneration(input_index, input->generation, input->stream,
                             input->timeline_offset_us);
      }
      SendVideoPacket(input_index, input->packet);
      if (stop_requested_.load() || failed_.load()) {
        return;
      }
    }
    if (stop_requested_.load() || failed_.load()) {
      return;
    }
    DrainVideoDecoder(input_index);
    input_runtime.decoded_video_frames->Close();
  }

  void StartVideoGeneration(
      std::size_t input_index, std::uint64_t generation,
      const std::shared_ptr<const internal::StreamInfo>& stream,
      std::optional<std::int64_t> timeline_offset_us) {
    if (stream == nullptr) {
      internal::ThrowError(StatusCode::kInternal,
                           "video packet generation has no stream metadata");
    }
    DrainVideoDecoder(input_index);
    InputRuntime& input = *inputs_[input_index];
    input.video_decoder = std::make_unique<internal::NvDecoder>();
    input.video_decoder->Open(*stream, *cuda_device_);
    input.video_decoder_generation = generation;
    input.video_timeline_offset_initialized = timeline_offset_us.has_value();
    input.video_timeline_offset_us = timeline_offset_us.value_or(0);
  }

  void SendVideoPacket(std::size_t input_index, const ffmpeg::Packet& packet) {
    internal::NvDecoder& decoder = *inputs_[input_index]->video_decoder;
    while (!decoder.Send(packet)) {
      if (ReceiveDecodedVideo(input_index) == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "NVDEC made no progress while sending a packet");
      }
    }
    ReceiveDecodedVideo(input_index);
  }

  std::size_t ReceiveDecodedVideo(std::size_t input_index) {
    InputRuntime& input = *inputs_[input_index];
    internal::NvDecoder& decoder = *input.video_decoder;
    std::size_t count = 0;
    while (std::optional<ffmpeg::Frame> frame = decoder.Receive()) {
      const std::int64_t source_pts_us =
          internal::ToMicroseconds(frame->get()->pts, frame->get()->time_base);
      std::int64_t timeline_pts_us = source_pts_us;
      if (source_pts_us == AV_NOPTS_VALUE) {
        if (inputs_.size() != 1) {
          internal::ThrowError(
              StatusCode::kUnsupported,
              "multi-input video synchronization requires decoded PTS");
        }
        const std::int64_t latest = input.latest_video_timeline_us.load();
        timeline_pts_us = latest == AV_NOPTS_VALUE
                              ? 0
                              : latest + VideoFrameInterval().count();
      } else {
        if (!input.video_timeline_offset_initialized) {
          input.video_timeline_offset_us =
              LatestVideoTimelineAnchorUs(input_index) - source_pts_us;
          input.video_timeline_offset_initialized = true;
        }
        timeline_pts_us += input.video_timeline_offset_us;
      }
      input.latest_video_timeline_us.store(timeline_pts_us);
      DecodedVideoMessage message{input_index, timeline_pts_us,
                                  std::move(*frame)};
      const bool pushed =
          IsLiveProcessing(input_index)
              ? input.decoded_video_frames->PushDroppingOldest(
                    std::move(message))
              : input.decoded_video_frames->Push(std::move(message));
      if (!pushed) {
        return count;
      }
      ++count;
    }
    return count;
  }

  std::int64_t LatestVideoTimelineAnchorUs(
      std::size_t reconnecting_input_index) const noexcept {
    std::int64_t anchor = 0;
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      const std::int64_t latest = input->latest_video_timeline_us.load();
      if (latest != AV_NOPTS_VALUE) {
        anchor = std::max(anchor, latest);
      }
    }
    const std::int64_t own_latest =
        inputs_[reconnecting_input_index]->latest_video_timeline_us.load();
    if (own_latest != AV_NOPTS_VALUE) {
      anchor = std::max(anchor, own_latest + VideoFrameInterval().count());
    }
    return anchor;
  }

  void VideoLoop() {
    internal::VideoSynchronizerConfig synchronization_config;
    synchronization_config.input_count = inputs_.size();
    synchronization_config.primary_input_index =
        config_.synchronization.primary_video_input_index;
    synchronization_config.pts_tolerance =
        config_.synchronization.pts_tolerance;
    synchronization_config.maximum_wait = config_.synchronization.maximum_wait;
    synchronization_config.queue_capacity =
        config_.queues.decoded_video_capacity;
    internal::VideoSynchronizer synchronizer(synchronization_config);
    InputRuntime& primary =
        *inputs_[config_.synchronization.primary_video_input_index];
    const std::chrono::microseconds poll_interval =
        std::min(VideoFrameInterval(),
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     config_.synchronization.maximum_wait));

    last_business_video_time_ = std::chrono::steady_clock::now();
    if (UsesDelayedPacketScheduler()) {
      last_business_video_time_ -= config_.standby_timeout;
    }
    while (!stop_requested_.load() && !failed_.load()) {
      std::optional<DecodedVideoMessage> primary_frame =
          IsLiveProcessing(primary.input_index)
              ? primary.decoded_video_frames->PopFor(poll_interval)
              : primary.decoded_video_frames->Pop();
      if (primary_frame.has_value()) {
        synchronizer.Push(primary_frame->input_index,
                          std::move(primary_frame->frame),
                          primary_frame->timeline_pts_us);
      }
      for (const std::unique_ptr<InputRuntime>& input : inputs_) {
        if (input->input_index == primary.input_index) {
          continue;
        }
        if (std::optional<DecodedVideoMessage> frame =
                input->decoded_video_frames->TryPop()) {
          synchronizer.Push(frame->input_index, std::move(frame->frame),
                            frame->timeline_pts_us);
        }
      }
      while (std::optional<internal::SynchronizedVideoFrames> frames =
                 synchronizer.Pop()) {
        ProcessVideoFrames(std::move(frames->frames));
      }
      const bool all_closed =
          std::all_of(inputs_.begin(), inputs_.end(), [](const auto& input) {
            return input->decoded_video_frames->is_closed() &&
                   input->decoded_video_frames->size() == 0;
          });
      if (all_closed) {
        break;
      }
      MaybeEncodeStandbyVideo();
    }
    if (!stop_requested_.load() && !failed_.load()) {
      DrainVideoEncoder();
      video_finished_.store(true);
      media_progress_cv_.notify_all();
    }
  }

  void ProcessVideoFrames(std::vector<ffmpeg::Frame> inputs) {
    ffmpeg::Frame output;
    if (processor_->has_video_callback()) {
      output = frame_pool_->Acquire();
      std::vector<MwGpuNv12FrameView> input_views;
      input_views.reserve(inputs.size());
      for (const ffmpeg::Frame& input : inputs) {
        input_views.push_back(
            internal::MakeInputGpuNv12FrameView(input, config_.device_id));
      }
      MwGpuNv12FrameView output_view = internal::MakeOutputGpuNv12FrameView(
          &output, config_.device_id, processor_->context().video_output);
      processor_->ProcessVideo(input_views.data(), input_views.size(),
                               &output_view);
    } else {
      output = black_frame_->Ref();
    }

    last_business_video_time_ = std::chrono::steady_clock::now();
    standby_active_.store(false);
    EncodeVideoFrame(std::move(output));
  }

  void MaybeEncodeStandbyVideo() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_business_video_time_ < config_.standby_timeout) {
      return;
    }
    if (!standby_active_.exchange(true)) {
      next_standby_video_time_ = now;
    }
    if (now < next_standby_video_time_) {
      return;
    }
    EncodeVideoFrame(standby_image_->Ref());
    next_standby_video_time_ += VideoFrameInterval();
    if (next_standby_video_time_ < now) {
      next_standby_video_time_ = now + VideoFrameInterval();
    }
  }

  void EncodeVideoFrame(ffmpeg::Frame output) {
    output.get()->pts = next_video_pts_++;
    output.get()->time_base = video_encoder_->time_base();
    while (!video_encoder_->Send(output)) {
      if (ReceiveEncodedVideo() == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "NVENC made no progress while sending a frame");
      }
    }
    ReceiveEncodedVideo();
    media_progress_cv_.notify_all();
  }

  std::size_t ReceiveEncodedVideo() {
    std::size_t count = 0;
    while (std::optional<ffmpeg::Packet> packet = video_encoder_->Receive()) {
      DispatchEncodedPacket(internal::EncodedMediaType::kVideo,
                            std::move(*packet));
      ++count;
    }
    return count;
  }

  void DrainVideoDecoder(std::size_t input_index) {
    internal::NvDecoder& decoder = *inputs_[input_index]->video_decoder;
    while (!decoder.SendEndOfStream()) {
      if (ReceiveDecodedVideo(input_index) == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "NVDEC made no progress while draining");
      }
    }
    ReceiveDecodedVideo(input_index);
  }

  void DrainVideoEncoder() {
    while (!video_encoder_->SendEndOfStream()) {
      if (ReceiveEncodedVideo() == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "NVENC made no progress while draining");
      }
    }
    ReceiveEncodedVideo();
  }

  void AudioDecodeLoop(std::size_t input_index) {
    InputRuntime& input_runtime = *inputs_[input_index];
    while (true) {
      std::optional<AudioPacketMessage> input =
          IsLiveProcessing(input_index)
              ? input_runtime.audio_packets->PopFor(AudioFrameInterval())
              : input_runtime.audio_packets->Pop();
      if (!input.has_value()) {
        if (input_runtime.audio_packets->is_closed()) {
          break;
        }
        continue;
      }
      if (input->generation != input_runtime.audio_decoder_generation) {
        StartAudioGeneration(input_index, input->generation, input->stream,
                             input->timeline_offset_us);
      }
      SendAudioPacket(input_index, input->packet);
      if (stop_requested_.load() || failed_.load()) {
        return;
      }
    }
    if (stop_requested_.load() || failed_.load()) {
      return;
    }
    FinishAudioGeneration(input_index);
    input_runtime.audio_end_of_stream.store(true);
    media_progress_cv_.notify_all();
  }

  void StartAudioGeneration(
      std::size_t input_index, std::uint64_t generation,
      const std::shared_ptr<const internal::StreamInfo>& stream,
      std::optional<std::int64_t> timeline_offset_us) {
    if (stream == nullptr) {
      internal::ThrowError(StatusCode::kInternal,
                           "audio packet generation has no stream metadata");
    }
    FinishAudioGeneration(input_index);
    InputRuntime& input = *inputs_[input_index];
    input.audio_decoder = std::make_unique<internal::AudioAutoDecoder>();
    input.audio_decoder->Open(*stream);
    input.audio_converter = std::make_unique<internal::AudioConverter>();
    input.audio_decoder_generation = generation;
    input.audio_timeline_offset_initialized = timeline_offset_us.has_value();
    input.audio_timeline_offset_frames =
        timeline_offset_us.has_value()
            ? internal::RescaleTimestamp(*timeline_offset_us,
                                         internal::kMicrosecondsTimeBase,
                                         AVRational{1, 48'000})
            : 0;
  }

  void FinishAudioGeneration(std::size_t input_index) {
    InputRuntime& input = *inputs_[input_index];
    DrainAudioDecoder(input_index);
    while (std::optional<internal::NormalizedAudioFrame> converted =
               input.audio_converter->Flush()) {
      PushConvertedAudio(input_index, std::move(*converted));
    }
  }

  void SendAudioPacket(std::size_t input_index, const ffmpeg::Packet& packet) {
    internal::AudioAutoDecoder& decoder = *inputs_[input_index]->audio_decoder;
    while (!decoder.Send(packet)) {
      if (ReceiveDecodedAudio(input_index) == 0) {
        internal::ThrowError(
            StatusCode::kInternal,
            "audio decoder made no progress while sending a packet");
      }
    }
    ReceiveDecodedAudio(input_index);
  }

  std::size_t ReceiveDecodedAudio(std::size_t input_index) {
    InputRuntime& input = *inputs_[input_index];
    std::size_t count = 0;
    while (std::optional<ffmpeg::Frame> frame =
               input.audio_decoder->Receive()) {
      if (std::optional<internal::NormalizedAudioFrame> converted =
              input.audio_converter->Convert(*frame)) {
        PushConvertedAudio(input_index, std::move(*converted));
      }
      ++count;
    }
    return count;
  }

  void PushConvertedAudio(std::size_t input_index,
                          internal::NormalizedAudioFrame frame) {
    InputRuntime& input = *inputs_[input_index];
    if (audio_consumption_finished_.load()) {
      return;
    }
    const AVRational time_base{frame.time_base.numerator,
                               frame.time_base.denominator};
    if (frame.pts == MW_TIMESTAMP_UNAVAILABLE ||
        !internal::IsValidTimeBase(time_base)) {
      internal::ThrowError(StatusCode::kUnsupported,
                           "audio synchronization requires decoded PTS");
    }
    const std::int64_t source_pts =
        internal::RescaleTimestamp(frame.pts, time_base, AVRational{1, 48'000});
    if (!input.audio_timeline_offset_initialized) {
      input.audio_timeline_offset_frames =
          next_audio_block_pts_.load() - source_pts;
      input.audio_timeline_offset_initialized = true;
    }
    frame.pts = internal::AddTimestampOffset(frame.pts, time_base,
                                             input.audio_timeline_offset_frames,
                                             AVRational{1, 48'000});
    frame.pts_us = internal::AddTimestampOffset(
        frame.pts_us, internal::kMicrosecondsTimeBase,
        input.audio_timeline_offset_frames, AVRational{1, 48'000});
    if (IsLiveProcessing(input_index)) {
      {
        std::lock_guard<std::mutex> audio_lock(input.audio_mutex);
        input.aligned_audio->PushDroppingOldest(std::move(frame));
      }
      media_progress_cv_.notify_all();
      return;
    }
    {
      std::unique_lock<std::mutex> audio_lock(input.audio_mutex);
      input.audio_space_cv.wait(audio_lock, [this, &input, &frame] {
        return stop_requested_.load() || failed_.load() ||
               audio_consumption_finished_.load() ||
               input.aligned_audio->remaining_capacity_frames() >=
                   frame.frame_count;
      });
      if (stop_requested_.load() || failed_.load() ||
          audio_consumption_finished_.load()) {
        return;
      }
      input.aligned_audio->Push(std::move(frame));
    }
    media_progress_cv_.notify_all();
  }

  void AudioLoop() {
    while (!stop_requested_.load() && !failed_.load()) {
      const std::int64_t video_duration_frames = VideoDurationAudioFrames();
      while (next_audio_block_pts_.load() < video_duration_frames) {
        ProcessAudioInterval(next_audio_block_pts_.load());
        next_audio_block_pts_.fetch_add(static_cast<std::int64_t>(
            internal::kProcessorAudioBlockFrameCount));
      }
      if (video_finished_.load() &&
          next_audio_block_pts_.load() >= video_duration_frames) {
        break;
      }
      std::unique_lock<std::mutex> progress_lock(media_progress_mutex_);
      media_progress_cv_.wait_for(progress_lock, AudioFrameInterval(), [this] {
        return stop_requested_.load() || failed_.load() ||
               video_finished_.load() ||
               next_audio_block_pts_.load() < VideoDurationAudioFrames();
      });
    }
    if (stop_requested_.load() || failed_.load()) {
      return;
    }
    audio_consumption_finished_.store(true);
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      input->audio_space_cv.notify_all();
    }
    DrainAudioEncoder();
  }

  std::int64_t VideoDurationAudioFrames() const noexcept {
    return av_rescale_q(next_video_pts_.load(), video_encoder_->time_base(),
                        AVRational{1, 48'000});
  }

  bool AudioInputsReady(std::int64_t interval_end_pts) {
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      if (input->aligned_audio == nullptr ||
          input->audio_end_of_stream.load()) {
        continue;
      }
      std::lock_guard<std::mutex> audio_lock(input->audio_mutex);
      const std::optional<std::int64_t> available =
          input->aligned_audio->accepted_until_pts();
      if (!available.has_value() || *available < interval_end_pts) {
        return false;
      }
    }
    return true;
  }

  void ProcessAudioInterval(std::int64_t start_pts) {
    const std::int64_t end_pts =
        start_pts +
        static_cast<std::int64_t>(internal::kProcessorAudioBlockFrameCount);
    const bool finite_inputs = std::all_of(
        config_.inputs.begin(), config_.inputs.end(), internal::IsFiniteInput);
    {
      std::unique_lock<std::mutex> progress_lock(media_progress_mutex_);
      if (finite_inputs) {
        media_progress_cv_.wait(progress_lock, [this, end_pts] {
          return stop_requested_.load() || failed_.load() ||
                 AudioInputsReady(end_pts);
        });
      } else {
        media_progress_cv_.wait_for(
            progress_lock, AudioFrameInterval(), [this, end_pts] {
              return stop_requested_.load() || failed_.load() ||
                     AudioInputsReady(end_pts);
            });
      }
    }
    if (stop_requested_.load() || failed_.load()) {
      return;
    }

    std::vector<internal::NormalizedAudioFrame> inputs;
    inputs.reserve(inputs_.size());
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      if (input->aligned_audio == nullptr) {
        internal::NormalizedAudioFrame silence;
        silence.frame_count = internal::kProcessorAudioBlockFrameCount;
        silence.samples.assign(
            silence.frame_count * internal::kProcessorAudioChannelCount, 0.0F);
        silence.pts = start_pts;
        silence.time_base = {1, 48'000};
        silence.pts_us = internal::ToMicroseconds(start_pts, {1, 48'000});
        inputs.push_back(std::move(silence));
      } else {
        {
          std::lock_guard<std::mutex> audio_lock(input->audio_mutex);
          inputs.push_back(input->aligned_audio->Read(
              start_pts, internal::kProcessorAudioBlockFrameCount));
        }
        input->audio_space_cv.notify_all();
      }
    }

    std::vector<MwAudioFrameView> input_views;
    input_views.reserve(inputs.size());
    for (internal::NormalizedAudioFrame& input : inputs) {
      input_views.push_back(input.view());
    }
    internal::NormalizedAudioFrame output;
    output.frame_count = internal::kProcessorAudioBlockFrameCount;
    output.samples.assign(
        output.frame_count * internal::kProcessorAudioChannelCount, 0.0F);
    MwAudioFrameView output_view = output.view();
    output_view.pts = 0;
    output_view.time_base = {0, 0};
    output_view.pts_us = 0;
    if (!standby_active_.load()) {
      processor_->ProcessAudio(input_views.data(), input_views.size(),
                               &output_view);
    }
    EncodeAudioBlock(std::move(output));
  }

  void EncodeAudioBlock(internal::NormalizedAudioFrame output) {
    while (!audio_encoder_->Send(output)) {
      if (ReceiveEncodedAudio() == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "AAC encoder made no progress while sending");
      }
    }
    ReceiveEncodedAudio();
  }

  std::size_t ReceiveEncodedAudio() {
    std::size_t count = 0;
    while (std::optional<ffmpeg::Packet> packet = audio_encoder_->Receive()) {
      DispatchEncodedPacket(internal::EncodedMediaType::kAudio,
                            std::move(*packet));
      ++count;
    }
    return count;
  }

  void DrainAudioDecoder(std::size_t input_index) {
    internal::AudioAutoDecoder& decoder = *inputs_[input_index]->audio_decoder;
    while (!decoder.SendEndOfStream()) {
      if (ReceiveDecodedAudio(input_index) == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "audio decoder made no progress while draining");
      }
    }
    ReceiveDecodedAudio(input_index);
  }

  void DrainAudioEncoder() {
    while (!audio_encoder_->SendEndOfStream()) {
      if (ReceiveEncodedAudio() == 0) {
        internal::ThrowError(StatusCode::kInternal,
                             "AAC encoder made no progress while draining");
      }
    }
    ReceiveEncodedAudio();
  }

  void DispatchEncodedPacket(internal::EncodedMediaType media_type,
                             ffmpeg::Packet packet) {
    const internal::EncodedPacket encoded(media_type, std::move(packet));
    for (const std::unique_ptr<internal::OutputWorker>& output :
         output_workers_) {
      static_cast<void>(output->Enqueue(encoded));
    }
  }

  void Coordinate() noexcept {
    JoinWorkers();
    StopProcessor();
    if (failed_.load()) {
      Complete(PipelineState::kFailed, FirstError());
    } else if (stop_requested_.load()) {
      Complete(PipelineState::kStopped, Status::Ok());
    } else {
      Complete(PipelineState::kCompleted, Status::Ok());
    }
  }

  void Fail(Status status) noexcept {
    bool expected = false;
    if (failed_.compare_exchange_strong(expected, true)) {
      std::lock_guard<std::mutex> error_lock(error_mutex_);
      first_error_ = std::move(status);
    }
    CancelAll();
  }

  void CancelAll() noexcept {
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      if (input->interrupt != nullptr) {
        input->interrupt->Cancel();
      }
      if (input->video_packets != nullptr) {
        input->video_packets->Cancel();
      }
      if (input->audio_packets != nullptr) {
        input->audio_packets->Cancel();
      }
      if (input->decoded_video_frames != nullptr) {
        input->decoded_video_frames->Cancel();
      }
      input->audio_space_cv.notify_all();
    }
    for (const std::unique_ptr<internal::OutputWorker>& output :
         output_workers_) {
      output->RequestStop();
    }
    if (delayed_packets_ != nullptr) {
      delayed_packets_->Cancel();
    }
    reconnect_cv_.notify_all();
    media_progress_cv_.notify_all();
  }

  void StopProcessor() noexcept {
    if (!processor_started_.exchange(false) || processor_ == nullptr) {
      return;
    }
    try {
      processor_->Stop();
    } catch (...) {
      spdlog::warn("pipeline '{}' Processor on_stop threw an exception",
                   config_.id);
    }
  }

  void Complete(PipelineState state, const Status& status) noexcept {
    SetState(state, status);
    SetTerminalStatus(status);
    if (notification_queue_ != nullptr) {
      notification_queue_->Close();
    }
  }

  void SetTerminalStatus(const Status& status) noexcept {
    {
      std::lock_guard<std::mutex> terminal_lock(terminal_mutex_);
      terminal_status_ = status;
      terminal_ = true;
    }
    terminal_cv_.notify_all();
  }

  Status TerminalStatus() const {
    std::lock_guard<std::mutex> terminal_lock(terminal_mutex_);
    return terminal_status_;
  }

  Status FirstError() const {
    std::lock_guard<std::mutex> error_lock(error_mutex_);
    return first_error_;
  }

  void SetState(PipelineState state, const Status& status) noexcept {
    state_.store(state);
    if (notification_queue_ != nullptr) {
      static_cast<void>(notification_queue_->Push(
          Notification{StateNotification{state, status}}));
    }
  }

  void NotifyInputEndpointEvent(std::size_t input_index, EndpointEventType type,
                                const Status& status) noexcept {
    NotifyEndpointEvent(EndpointKind::kInput, config_.inputs[input_index].id,
                        type, status);
  }

  void NotifyEndpointEvent(EndpointKind kind, const std::string& endpoint_id,
                           EndpointEventType type,
                           const Status& status) noexcept {
    if (notification_queue_ == nullptr) {
      return;
    }
    EndpointEvent event;
    event.kind = kind;
    event.type = type;
    event.endpoint_id = endpoint_id;
    event.status = status;
    static_cast<void>(
        notification_queue_->Push(Notification{std::move(event)}));
  }

  void StartNotificationThread() {
    notification_queue_ =
        std::make_unique<internal::BlockingQueue<Notification>>(16);
    notification_thread_ = std::thread([this] {
      while (std::optional<Notification> notification =
                 notification_queue_->Pop()) {
        try {
          if (std::holds_alternative<StateNotification>(*notification)) {
            if (config_.callbacks.on_state_changed) {
              const StateNotification& state =
                  std::get<StateNotification>(*notification);
              config_.callbacks.on_state_changed(state.state, state.status);
            }
          } else if (config_.callbacks.on_endpoint_event) {
            config_.callbacks.on_endpoint_event(
                std::get<EndpointEvent>(*notification));
          }
        } catch (...) {
          spdlog::warn("pipeline '{}' notification callback threw an exception",
                       config_.id);
        }
      }
    });
  }

  void JoinWorkers() noexcept {
    for (const std::unique_ptr<InputRuntime>& input : inputs_) {
      if (input->demux_thread.joinable()) {
        input->demux_thread.join();
      }
      if (input->delay_thread.joinable()) {
        input->delay_thread.join();
      }
      if (input->video_decode_thread.joinable()) {
        input->video_decode_thread.join();
      }
      if (input->audio_decode_thread.joinable()) {
        input->audio_decode_thread.join();
      }
    }
    if (video_thread_.joinable()) {
      video_thread_.join();
    }
    if (audio_thread_.joinable()) {
      audio_thread_.join();
    }
    for (const std::unique_ptr<internal::OutputWorker>& output :
         output_workers_) {
      output->Wait();
    }
  }

  void JoinCoordinator() noexcept {
    std::lock_guard<std::mutex> join_lock(coordinator_join_mutex_);
    if (coordinator_thread_.joinable() &&
        coordinator_thread_.get_id() != std::this_thread::get_id()) {
      coordinator_thread_.join();
    }
  }

  void JoinNotificationThread() noexcept {
    std::lock_guard<std::mutex> join_lock(notification_join_mutex_);
    if (notification_thread_.joinable() &&
        notification_thread_.get_id() != std::this_thread::get_id()) {
      notification_thread_.join();
    }
  }

  PipelineConfig config_;
  std::atomic<PipelineState> state_{PipelineState::kCreated};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> processor_started_{false};
  std::atomic<bool> standby_active_{false};
  std::atomic<int> remaining_encoder_workers_{0};
  std::atomic<std::int64_t> next_video_pts_{0};
  std::atomic<std::int64_t> next_audio_block_pts_{0};
  std::atomic<bool> video_finished_{false};
  std::atomic<bool> audio_consumption_finished_{false};
  std::mt19937_64 random_engine_{std::random_device{}()};
  std::uniform_real_distribution<double> jitter_distribution_{-1.0, 1.0};
  std::chrono::steady_clock::time_point last_business_video_time_;
  std::chrono::steady_clock::time_point next_standby_video_time_;

  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex terminal_mutex_;
  mutable std::mutex error_mutex_;
  std::mutex coordinator_join_mutex_;
  std::mutex notification_join_mutex_;
  std::mutex reconnect_mutex_;
  std::mutex media_progress_mutex_;
  std::condition_variable terminal_cv_;
  std::condition_variable reconnect_cv_;
  std::condition_variable media_progress_cv_;
  Status terminal_status_;
  Status first_error_;
  bool terminal_ = false;

  std::unique_ptr<internal::BlockingQueue<Notification>> notification_queue_;
  std::thread notification_thread_;
  std::thread video_thread_;
  std::thread audio_thread_;
  std::thread coordinator_thread_;

  // Declared first among FFmpeg resources so the CUDA context is destroyed
  // after every frame, codec, and queue that may reference it.
  std::unique_ptr<internal::CudaDeviceContext> cuda_device_;
  std::vector<std::unique_ptr<InputRuntime>> inputs_;
  std::unique_ptr<internal::GpuNv12FramePool> frame_pool_;
  std::unique_ptr<ffmpeg::Frame> black_frame_;
  std::unique_ptr<internal::StandbyImage> standby_image_;
  std::unique_ptr<internal::Processor> processor_;
  std::unique_ptr<internal::NvEncoder> video_encoder_;
  std::unique_ptr<internal::AacEncoder> audio_encoder_;
  std::vector<std::unique_ptr<internal::OutputWorker>> output_workers_;
  std::unique_ptr<internal::CompressedPacketDelayBuffer> delayed_packets_;
};

Pipeline::Pipeline(PipelineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Pipeline::~Pipeline() = default;

Status Pipeline::Start() { return impl_->Start(); }

Status Pipeline::Stop() { return impl_->Stop(); }

Status Pipeline::Wait() { return impl_->Wait(); }

Status Pipeline::UpdateProcessorConfig(std::string config) {
  return impl_->UpdateProcessorConfig(std::move(config));
}

PipelineState Pipeline::state() const noexcept { return impl_->state(); }

}  // namespace mw::streamer
