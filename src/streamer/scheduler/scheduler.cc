#include "mw/streamer/scheduler/scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/performance/latency.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;
constexpr AVRational kNanoseconds{1, 1000000000};

void Check(int result, const char* operation) {
  ffmpeg::FfmpegException::throwIfError(result, operation);
}

bool HasData(const ffmpeg::Frame& frame) noexcept {
  return frame.get() && frame->data[0];
}

ffmpeg::Frame SelectTrack(const Scheduler::Inputs& inputs, bool video) {
  for (const auto& input : inputs) {
    const auto& frame = video ? input.frame.video : input.frame.audio;
    if (frame && HasData(*frame)) return frame->Ref();
  }
  return ffmpeg::Frame{};
}

// Channel ordering bounds this queue: after submitting the current tick it
// waits for the previous tick before another submission. Tasks own snapshots,
// and the worker finishes all accepted tasks before destruction returns.
class VideoExecutor final {
 public:
  bool Start() noexcept {
    return worker_.Start([this]() noexcept { Run(); });
  }
  ~VideoExecutor() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_one();
    worker_.Join();
  }

  std::future<ffmpeg::Frame> Submit(std::function<ffmpeg::Frame()> work) {
    std::packaged_task<ffmpeg::Frame()> task(std::move(work));
    auto result = task.get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    changed_.notify_one();
    return result;
  }

 private:
  void Run() noexcept {
    while (true) {
      std::packaged_task<ffmpeg::Frame()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      // packaged_task preserves an exception for the channel's future.get().
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::deque<std::packaged_task<ffmpeg::Frame()>> tasks_;
  internal::Thread worker_;
};

struct FifoDeleter {
  void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

std::vector<void*> AudioPlanes(const ffmpeg::Frame& frame) {
  const auto count =
      av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))
          ? frame->ch_layout.nb_channels
          : 1;
  std::vector<void*> planes;
  planes.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index)
    planes.push_back(frame->extended_data[index]);
  return planes;
}

bool Terminal(SchedulerInputState state) noexcept {
  return state == SchedulerInputState::kEof ||
         state == SchedulerInputState::kFailed ||
         state == SchedulerInputState::kStopped;
}

struct InputSlot {
  std::shared_ptr<AvMediaInput> input;
  SchedulerInputFrame latest;
  std::optional<ffmpeg::Frame> last_video;
  std::optional<ffmpeg::Frame> last_audio;
  std::optional<std::uint64_t> eof_generation;
  std::unique_ptr<AVAudioFifo, FifoDeleter> audio;
};

struct PendingTick {
  SchedulerTick tick;
  std::uint64_t audio_sample_index;
  std::future<ffmpeg::Frame> video;
  ffmpeg::Frame audio;
};

}  // namespace

class Scheduler::Impl final {
 public:
  explicit Impl(SchedulerConfig config) : config_(std::move(config)) {
    const auto& output = config_.output;
    const bool video_enabled = output.video_pixel_format != AV_PIX_FMT_NONE;
    const bool audio_enabled = output.audio_sample_format != AV_SAMPLE_FMT_NONE;
    const auto* pixel = av_pix_fmt_desc_get(output.video_pixel_format);
    if ((!video_enabled && !audio_enabled) ||
        (video_enabled &&
         (!pixel || (pixel->flags & AV_PIX_FMT_FLAG_HWACCEL) ||
          output.width <= 0 || output.height <= 0 ||
          av_image_check_size(static_cast<unsigned>(output.width),
                              static_cast<unsigned>(output.height), 0,
                              nullptr) < 0)) ||
        output.fps.num <= 0 || output.fps.den <= 0 ||
        (audio_enabled &&
         (output.audio_sample_rate <= 0 ||
          !av_get_sample_fmt_name(output.audio_sample_format) ||
          !av_channel_layout_check(&output.audio_channel_layout)))) {
      throw std::invalid_argument("Scheduler 输出格式无效");
    }
    if (av_rescale_q(1, av_inv_q(output.fps), kNanoseconds) <= 0) {
      throw std::invalid_argument("Scheduler 输出周期超出范围");
    }
    if (audio_enabled) {
      const auto numerator =
          static_cast<std::int64_t>(output.audio_sample_rate) * output.fps.den;
      const auto maximum =
          av_rescale_rnd(numerator, 1, output.fps.num, AV_ROUND_UP);
      if (numerator < output.fps.num ||
          maximum > std::numeric_limits<int>::max() / 2) {
        throw std::invalid_argument("Scheduler 输出周期或音频样本数量超出范围");
      }
      max_samples_ = static_cast<int>(maximum);
      variable_cadence_ = numerator % output.fps.num != 0;
      Check(av_channel_layout_copy(&audio_format_->ch_layout,
                                   &output.audio_channel_layout),
            "复制 Scheduler 音频布局");
      audio_format_->format = output.audio_sample_format;
      audio_format_->sample_rate = output.audio_sample_rate;
      config_.output.audio_channel_layout = audio_format_->ch_layout;
      default_audio_ = CreateSilence(max_samples_);
      if (variable_cadence_) shorter_audio_ = CreateSilence(max_samples_ - 1);
    } else {
      config_.output.audio_channel_layout = {};
    }
    if (video_enabled) {
      default_video_->format = output.video_pixel_format;
      default_video_->width = output.width;
      default_video_->height = output.height;
      default_video_->time_base = av_inv_q(output.fps);
      Check(av_frame_get_buffer(default_video_.get(), 0),
            "分配 Scheduler 默认画面");
      for (auto* buffer : default_video_->buf) {
        if (buffer) std::memset(buffer->data, 0, buffer->size);
      }
    }
  }

  ~Impl() { Stop(); }

  void CheckConfigurable() const {
    if (worker_.joinable())
      throw std::logic_error("配置或重新启动 Scheduler 前需要 Stop");
  }

  bool Start(std::vector<std::shared_ptr<AvMediaInput>> inputs) noexcept {
    if (worker_.joinable()) {
      MW_LOG_ERROR("streamer", "Scheduler 启动失败: 重新启动前需要 Stop");
      return false;
    }
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      if (!inputs[index] || inputs[index]->mode() != AvMediaInputMode::kLive) {
        MW_LOG_ERROR("streamer", "Scheduler 启动失败: 需要非空 Live 输入");
        return false;
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (inputs[previous] == inputs[index]) {
          MW_LOG_ERROR("streamer", "Scheduler 启动失败: 输入重复");
          return false;
        }
      }
    }
    if (output_ && !output_->Start(config_.output)) {
      MW_LOG_ERROR("streamer", "Scheduler 启动失败: Output 初始化失败");
      return false;
    }
    if (on_video_ && config_.output.video_pixel_format != AV_PIX_FMT_NONE)
      video_latency_.Start("perf.filter.video", this, "filter.video");
    if (on_audio_ && config_.output.audio_sample_format != AV_SAMPLE_FMT_NONE)
      audio_latency_.Start("perf.filter.audio", this, "filter.audio");
    std::unique_ptr<VideoExecutor> video;
    if (config_.output.video_pixel_format != AV_PIX_FMT_NONE) {
      video = std::make_unique<VideoExecutor>();
      if (!video->Start()) return false;
    }
    slots_.clear();
    slots_.reserve(inputs.size());
    for (auto& input : inputs) {
      InputSlot slot;
      slot.input = std::move(input);
      slots_.push_back(std::move(slot));
    }
    video_ = std::move(video);
    last_output_video_.reset();
    stopping_ = false;
    SetStatus(SchedulerState::kRunning);
    if (worker_.Start([this]() noexcept { Run(); })) return true;
    stopping_ = true;
    video_.reset();
    slots_.clear();
    SetStatus(SchedulerState::kStopped);
    return false;
  }

  SchedulerStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  void RequestStop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
  }

  void SetOutput(std::shared_ptr<Output> output) {
    CheckConfigurable();
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = std::move(output);
  }

  void Stop() noexcept {
    RequestStop();
    worker_.Join();
    video_.reset();
    video_latency_.Finish();
    audio_latency_.Finish();
    slots_.clear();
  }

  OnVideo on_video_;
  OnAudio on_audio_;
  OnOutput on_output_;
  std::shared_ptr<Output> output_;

 private:
  void SetStatus(SchedulerState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {state};
  }

  int Samples(std::uint64_t number) const {
    if (config_.output.audio_sample_format == AV_SAMPLE_FMT_NONE) return 0;
    // The fractional phase matches Caspar's progressive channel, which
    // increments frame_counter before indexing its cadence vector.
    const auto tick = static_cast<std::int64_t>(number);
    const auto time_base = av_inv_q(config_.output.fps);
    const AVRational sample_base{1, config_.output.audio_sample_rate};
    const auto begin =
        av_rescale_q_rnd(tick, time_base, sample_base, AV_ROUND_DOWN);
    const auto end =
        av_rescale_q_rnd(tick + 1, time_base, sample_base, AV_ROUND_DOWN);
    return static_cast<int>(end - begin);
  }

  ffmpeg::Frame AllocateAudio(int samples,
                              const ffmpeg::Frame* source = nullptr) const {
    ffmpeg::Frame frame;
    if (source) frame.CopyPropertiesFrom(*source);
    frame->format = audio_format_->format;
    frame->sample_rate = audio_format_->sample_rate;
    frame->nb_samples = samples;
    frame->duration = samples;
    if (!source) frame->time_base = {1, frame->sample_rate};
    Check(av_channel_layout_copy(&frame->ch_layout, &audio_format_->ch_layout),
          "设置 Scheduler 音频布局");
    Check(av_frame_get_buffer(frame.get(), 0), "分配 Scheduler 音频帧");
    return frame;
  }

  ffmpeg::Frame CreateSilence(int samples) const {
    auto frame = AllocateAudio(samples);
    Check(av_samples_set_silence(frame->extended_data, 0, samples,
                                 frame->ch_layout.nb_channels,
                                 config_.output.audio_sample_format),
          "生成 Scheduler 静音");
    return frame;
  }

  ffmpeg::Frame Silence(int samples) const {
    // Samples() produces only the floor/ceiling of the samples-per-tick ratio.
    // Shared buffers stay read-only; each tick receives its own frame header.
    return samples == max_samples_ ? default_audio_.Ref()
                                   : shorter_audio_->Ref();
  }

  ffmpeg::Frame PrepareAudio(InputSlot& slot, const ffmpeg::Frame& source,
                             int samples) const {
    // Input owns format conversion and supplies nonempty audio. Only cadence
    // reconciliation belongs here; a different sample count is not an error.
    if (!variable_cadence_ && source->nb_samples == samples &&
        (!slot.audio || av_audio_fifo_size(slot.audio.get()) == 0))
      return source.Ref();

    if (!slot.audio) {
      slot.audio.reset(av_audio_fifo_alloc(config_.output.audio_sample_format,
                                           audio_format_->ch_layout.nb_channels,
                                           max_samples_ * 2));
      if (!slot.audio) throw std::bad_alloc();
      // Like Caspar's audio mixer, one initial silent sample allows sources
      // with a different fractional cadence phase to retain their samples.
      if (variable_cadence_) {
        auto planes = AudioPlanes(default_audio_);
        Check(av_audio_fifo_write(slot.audio.get(), planes.data(), 1),
              "初始化 Scheduler 音频 cadence");
      }
    }
    const int previous_samples = av_audio_fifo_size(slot.audio.get());
    auto input_planes = AudioPlanes(source);
    Check(av_audio_fifo_write(slot.audio.get(), input_planes.data(),
                              source->nb_samples),
          "保存 Scheduler 音频余量");
    auto output = AllocateAudio(samples, &source);
    auto output_planes = AudioPlanes(output);
    const int received =
        av_audio_fifo_read(slot.audio.get(), output_planes.data(), samples);
    Check(received, "读取 Scheduler 本拍音频");
    for (int index = received; index < samples; ++index) {
      Check(av_samples_copy(output->extended_data, source->extended_data, index,
                            source->nb_samples - 1, 1,
                            audio_format_->ch_layout.nb_channels,
                            config_.output.audio_sample_format),
            "补足 Scheduler 音频尾样本");
    }
    const int remaining = av_audio_fifo_size(slot.audio.get());
    if (remaining > max_samples_ * 2 || remaining > source->nb_samples) {
      const int retained = std::min(max_samples_ * 2, source->nb_samples);
      MW_LOG_WARNING("streamer",
                     "Scheduler 音频余量超限: remaining={} retained={}",
                     remaining, retained);
      av_audio_fifo_reset(slot.audio.get());
      const int offset = std::max(samples - previous_samples, 0);
      const int channels_per_plane =
          av_sample_fmt_is_planar(config_.output.audio_sample_format)
              ? 1
              : audio_format_->ch_layout.nb_channels;
      const auto offset_bytes =
          static_cast<std::size_t>(offset) *
          static_cast<std::size_t>(channels_per_plane) *
          static_cast<std::size_t>(
              av_get_bytes_per_sample(config_.output.audio_sample_format));
      for (auto& plane : input_planes)
        plane = static_cast<std::uint8_t*>(plane) + offset_bytes;
      Check(
          av_audio_fifo_write(slot.audio.get(), input_planes.data(), retained),
          "保留 Scheduler 音频余量");
    }
    return output;
  }

  void Observe(InputSlot& slot, const AvMediaInputStatus& status) {
    if (Terminal(slot.latest.state)) return;
    if (status.state == AvMediaInputState::kFailed) {
      slot.latest.state = SchedulerInputState::kFailed;
      MW_LOG_ERROR("streamer", "Scheduler 输入终止失败: input={}",
                   static_cast<const void*>(slot.input.get()));
    } else if (status.state == AvMediaInputState::kStopped) {
      slot.latest.state = SchedulerInputState::kStopped;
    } else if (status.state == AvMediaInputState::kEnded ||
               (status.state == AvMediaInputState::kDraining &&
                slot.eof_generation == status.generation)) {
      // Reading source EOF acknowledges Input's drain; its controller can
      // still be cleaning up before publishing Ended. No extra tick is needed.
      slot.latest.state = SchedulerInputState::kEof;
    }
  }

  Inputs Collect(int samples) {
    Inputs result;
    result.reserve(slots_.size());
    for (auto& slot : slots_) {
      Observe(slot, slot.input->status());
      std::optional<ffmpeg::Frame> audio;
      bool fresh = false;
      if (!Terminal(slot.latest.state)) {
        AvFrameSourceReadResult read;
        do {
          read = slot.input->TryReadFrame();
          if (read.status == AvFrameSourceReadStatus::kEof) {
            slot.eof_generation = read.generation;
            Observe(slot, slot.input->status());
          }
          // A drained loop segment is a boundary, not producer underflow.
          // Consume a cached next segment in this same channel tick. Only an
          // actual kNull should freeze the picture and mute its audio.
        } while (read.status == AvFrameSourceReadStatus::kEof &&
                 !Terminal(slot.latest.state) && !stopping_);
        if (read.status == AvFrameSourceReadStatus::kFrame) {
          const bool drained_transition =
              slot.eof_generation &&
              *slot.eof_generation >= slot.latest.generation &&
              *slot.eof_generation < read.generation;
          if (slot.latest.generation != read.generation && !drained_transition)
            slot.audio.reset();
          slot.eof_generation.reset();
          slot.latest.generation = read.generation;
          slot.latest.frame_index = read.frame_index;
          slot.latest.pts_us = read.pts_us;
          slot.latest.state = SchedulerInputState::kRunning;
          slot.last_video = std::move(read.video);
          slot.last_audio = std::move(read.audio);
          if (slot.last_audio &&
              config_.output.audio_sample_format != AV_SAMPLE_FMT_NONE)
            audio = PrepareAudio(slot, *slot.last_audio, samples);
          fresh = true;
        } else if (!Terminal(slot.latest.state)) {
          slot.latest.state = slot.last_video || slot.last_audio
                                  ? SchedulerInputState::kUnderflow
                                  : SchedulerInputState::kWaiting;
        }
        Observe(slot, slot.input->status());
      }
      if (!fresh || Terminal(slot.latest.state)) {
        slot.audio.reset();
        audio.reset();
        if (slot.last_audio &&
            config_.output.audio_sample_format != AV_SAMPLE_FMT_NONE)
          audio = Silence(samples);
      }
      slot.latest.frame.video = slot.last_video;
      slot.latest.frame.audio = std::move(audio);
      result.push_back(slot.latest);
    }
    return result;
  }

  bool Deliver(PendingTick tick) {
    auto video = tick.video.valid() ? tick.video.get() : ffmpeg::Frame{};
    if (stopping_) return false;
    MediaFrame frame;
    if (config_.output.video_pixel_format != AV_PIX_FMT_NONE) {
      if (HasData(video)) {
        last_output_video_ = video.Ref();
      } else {
        video = last_output_video_ ? last_output_video_->Ref()
                                   : default_video_.Ref();
        video->pts = static_cast<std::int64_t>(tick.tick.frame_number);
        video->time_base = av_inv_q(config_.output.fps);
        video->duration = 1;
      }
      frame.video = std::move(video);
    }
    if (config_.output.audio_sample_format != AV_SAMPLE_FMT_NONE) {
      frame.audio = HasData(tick.audio) ? std::move(tick.audio)
                                        : Silence(tick.tick.audio_samples);
    }
    bool synchronized = false;
    if (output_) {
      if (!output_->Send({frame, tick.tick.frame_number,
                          tick.audio_sample_index, tick.tick.audio_samples},
                         &stopping_))
        return false;
      synchronized = output_->HasSynchronizationClock();
    }
    if (!stopping_ && on_output_) on_output_(frame);
    return synchronized;
  }

  void Pace(std::optional<Clock::time_point>& origin, std::uint64_t iteration,
            bool synchronized) {
    if (synchronized) {
      origin.reset();
      return;
    }
    const auto offset =
        av_rescale_q(static_cast<std::int64_t>(iteration),
                     av_inv_q(config_.output.fps), kNanoseconds);
    const auto elapsed = std::chrono::nanoseconds(offset);
    if (!origin) origin = Clock::now() - elapsed;
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_until(lock, *origin + elapsed,
                        [this] { return stopping_.load(); });
  }

  PendingTick Produce(const SchedulerTick& tick, std::uint64_t sample_index) {
    auto inputs = std::make_shared<const Inputs>(Collect(tick.audio_samples));
    const bool all_failed =
        std::all_of(inputs->begin(), inputs->end(), [](const auto& input) {
          return input.state == SchedulerInputState::kFailed;
        });
    std::future<ffmpeg::Frame> video;
    if (video_)
      video = video_->Submit([this, inputs, tick, all_failed] {
        if (all_failed) {
          return ffmpeg::Frame{};
        }
        if (!on_video_) return SelectTrack(*inputs, true);
        const auto started = video_latency_.Begin();
        auto frame = on_video_(*inputs, tick);
        video_latency_.End(started);
        return frame;
      });
    auto audio = [&] {
      if (config_.output.audio_sample_format == AV_SAMPLE_FMT_NONE ||
          all_failed)
        return ffmpeg::Frame{};
      if (!on_audio_) return SelectTrack(*inputs, false);
      const auto started = audio_latency_.Begin();
      auto frame = on_audio_(*inputs, tick);
      audio_latency_.End(started);
      return frame;
    }();
    return {tick, sample_index, std::move(video), std::move(audio)};
  }

  void Run() noexcept {
    MW_LOG_INFO("streamer", "Scheduler 频道启动: inputs={} fps={}/{}",
                slots_.size(), config_.output.fps.num, config_.output.fps.den);
    std::deque<PendingTick> pending;
    std::optional<Clock::time_point> origin;
    std::uint64_t number = 0;
    std::uint64_t audio_sample_index = 0;
    while (!stopping_) {
      SchedulerTick tick{number, Samples(number)};
      bool synchronized = false;
      pending.push_back(Produce(tick, audio_sample_index));
      if (pending.size() > 1) {
        auto previous = std::move(pending.front());
        pending.pop_front();
        synchronized = Deliver(std::move(previous));
      }
      audio_sample_index += static_cast<std::uint64_t>(tick.audio_samples);
      Pace(origin, number, synchronized);
      ++number;
    }
    // Accepted tasks finish before Stop permits releasing filter resources.
    video_.reset();
    video_latency_.Finish();
    audio_latency_.Finish();
    SetStatus(SchedulerState::kStopped);
  }

  SchedulerConfig config_;
  ffmpeg::Frame audio_format_;
  ffmpeg::Frame default_video_;
  std::optional<ffmpeg::Frame> last_output_video_;
  ffmpeg::Frame default_audio_;
  std::optional<ffmpeg::Frame> shorter_audio_;
  int max_samples_ = 0;
  bool variable_cadence_ = false;
  std::vector<InputSlot> slots_;
  std::unique_ptr<VideoExecutor> video_;
  internal::StageLatency video_latency_;
  internal::StageLatency audio_latency_;
  internal::Thread worker_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  SchedulerStatus status_;
};

Scheduler::Scheduler(SchedulerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Scheduler::~Scheduler() = default;

void Scheduler::SetOnVideo(OnVideo callback) {
  impl_->CheckConfigurable();
  impl_->on_video_ = std::move(callback);
}
void Scheduler::SetOnAudio(OnAudio callback) {
  impl_->CheckConfigurable();
  impl_->on_audio_ = std::move(callback);
}
void Scheduler::SetOnOutput(OnOutput callback) {
  impl_->CheckConfigurable();
  impl_->on_output_ = std::move(callback);
}
void Scheduler::SetOutput(std::shared_ptr<Output> output) {
  impl_->SetOutput(std::move(output));
}
bool Scheduler::Start(
    std::vector<std::shared_ptr<AvMediaInput>> inputs) noexcept {
  return impl_->Start(std::move(inputs));
}
SchedulerStatus Scheduler::status() const { return impl_->status(); }
void Scheduler::RequestStop() noexcept { impl_->RequestStop(); }
void Scheduler::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
