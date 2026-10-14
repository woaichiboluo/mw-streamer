#include "mw/streamer/scheduler/scheduler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

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
  VideoExecutor() : worker_([this] { Run(); }) {}
  ~VideoExecutor() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_one();
    worker_.join();
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
  std::thread worker_;
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
  std::exception_ptr failure;
};

struct PendingTick {
  std::future<ffmpeg::Frame> video;
  ffmpeg::Frame audio;
};

}  // namespace

class Scheduler::Impl final {
 public:
  explicit Impl(SchedulerConfig config) : config_(std::move(config)) {
    const auto& output = config_.output;
    if (output.fps.num <= 0 || output.fps.den <= 0 ||
        output.audio_sample_rate <= 0 ||
        !av_get_sample_fmt_name(output.audio_sample_format) ||
        !av_channel_layout_check(&output.audio_channel_layout) ||
        (config_.end_policy != SchedulerEndPolicy::kAnyInput &&
         config_.end_policy != SchedulerEndPolicy::kAllInputs)) {
      throw std::invalid_argument("Scheduler 输出配置或终止策略无效");
    }
    const auto numerator =
        static_cast<std::int64_t>(output.audio_sample_rate) * output.fps.den;
    const auto maximum =
        av_rescale_rnd(numerator, 1, output.fps.num, AV_ROUND_UP);
    if (numerator < output.fps.num ||
        maximum > std::numeric_limits<int>::max() / 2 ||
        av_rescale_q(1, av_inv_q(output.fps), kNanoseconds) <= 0) {
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
  }

  ~Impl() { Stop(); }

  void CheckConfigurable() const {
    if (worker_.joinable())
      throw std::logic_error("配置或重新启动 Scheduler 前需要 Stop");
  }

  void Start(std::vector<std::shared_ptr<AvMediaInput>> inputs) {
    CheckConfigurable();
    if (inputs.empty()) throw std::invalid_argument("Scheduler 输入集合为空");
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      if (!inputs[index] || inputs[index]->mode() != AvMediaInputMode::kLive)
        throw std::invalid_argument("Scheduler 需要非空 Live 输入");
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (inputs[previous] == inputs[index])
          throw std::invalid_argument("Scheduler 不能重复消费同一路输入");
      }
      if (inputs[index]->status().state == AvMediaInputState::kStopped)
        throw std::invalid_argument("Scheduler 输入必须先由外部启动");
    }
    slots_.clear();
    slots_.reserve(inputs.size());
    for (auto& input : inputs) {
      InputSlot slot;
      slot.input = std::move(input);
      slots_.push_back(std::move(slot));
    }
    stopping_ = false;
    SetStatus(SchedulerState::kRunning);
    try {
      video_ = std::make_unique<VideoExecutor>();
      worker_ = std::thread([this] { Run(); });
    } catch (...) {
      video_.reset();
      slots_.clear();
      SetStatus(SchedulerState::kStopped);
      throw;
    }
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

  void Stop() noexcept {
    RequestStop();
    if (worker_.joinable()) worker_.join();
    video_.reset();
    slots_.clear();
  }

  OnVideo on_video_;
  OnAudio on_audio_;
  OnOutput on_output_;
  OnEof on_eof_;
  OnEnded on_ended_;
  OnFailed on_failed_;

 private:
  enum class Completion { kNone, kEof, kFailed, kStopped };

  void SetStatus(SchedulerState state, std::exception_ptr error = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {state, std::move(error)};
  }

  int Samples(std::uint64_t number) const {
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

  ffmpeg::Frame Silence(int samples) const {
    auto frame = AllocateAudio(samples);
    Check(av_samples_set_silence(frame->extended_data, 0, samples,
                                 frame->ch_layout.nb_channels,
                                 config_.output.audio_sample_format),
          "生成 Scheduler 静音");
    return frame;
  }

  ffmpeg::Frame PrepareAudio(InputSlot& slot, const ffmpeg::Frame& source,
                             int samples) const {
    if (!HasData(source) || source->nb_samples <= 0 ||
        source->nb_samples > max_samples_ ||
        source->format != audio_format_->format ||
        source->sample_rate != audio_format_->sample_rate ||
        av_channel_layout_compare(&source->ch_layout,
                                  &audio_format_->ch_layout) != 0) {
      throw std::invalid_argument("Scheduler 输入音频与频道目标格式不一致");
    }
    if (!variable_cadence_ && source->nb_samples == samples)
      return source.Ref();

    if (!slot.audio) {
      slot.audio.reset(av_audio_fifo_alloc(config_.output.audio_sample_format,
                                           audio_format_->ch_layout.nb_channels,
                                           max_samples_ * 2));
      if (!slot.audio) throw std::bad_alloc();
      // Like Caspar's audio mixer, one initial silent sample allows sources
      // with a different fractional cadence phase to retain their samples.
      if (variable_cadence_) {
        auto first = Silence(1);
        auto planes = AudioPlanes(first);
        Check(av_audio_fifo_write(slot.audio.get(), planes.data(), 1),
              "初始化 Scheduler 音频 cadence");
      }
    }
    if (av_audio_fifo_size(slot.audio.get()) + source->nb_samples >
        max_samples_ * 2)
      throw std::invalid_argument("Scheduler 输入音频 cadence 与频道不一致");
    auto input_planes = AudioPlanes(source);
    Check(av_audio_fifo_write(slot.audio.get(), input_planes.data(),
                              source->nb_samples),
          "保存 Scheduler 音频余量");
    auto output = AllocateAudio(samples, &source);
    auto output_planes = AudioPlanes(output);
    const int received =
        av_audio_fifo_read(slot.audio.get(), output_planes.data(), samples);
    Check(received, "读取 Scheduler 本拍音频");
    // A real short tail holds its last sample per channel, as Caspar does.
    // Producer underflow follows a separate path and always outputs silence.
    for (int index = received; index < samples; ++index) {
      Check(av_samples_copy(output->extended_data, source->extended_data, index,
                            source->nb_samples - 1, 1,
                            audio_format_->ch_layout.nb_channels,
                            config_.output.audio_sample_format),
            "补足 Scheduler 音频尾样本");
    }
    return output;
  }

  void Observe(InputSlot& slot, const AvMediaInputStatus& status) {
    if (Terminal(slot.latest.state)) return;
    if (status.state == AvMediaInputState::kFailed) {
      slot.latest.state = SchedulerInputState::kFailed;
      slot.failure = status.error;
      if (!slot.failure)
        slot.failure = std::make_exception_ptr(
            std::runtime_error("Scheduler 输入发生不可恢复失败"));
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
          if (slot.last_audio)
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
        if (slot.last_audio) audio = Silence(samples);
      }
      slot.latest.frame.video = slot.last_video;
      slot.latest.frame.audio = std::move(audio);
      result.push_back(slot.latest);
    }
    return result;
  }

  Completion Completed(std::exception_ptr& failure) const {
    bool any = false;
    bool all = true;
    bool stopped = false;
    for (const auto& slot : slots_) {
      const bool terminal = Terminal(slot.latest.state);
      any |= terminal;
      all &= terminal;
      stopped |= slot.latest.state == SchedulerInputState::kStopped;
      if (slot.failure && !failure) failure = slot.failure;
    }
    const bool completed =
        config_.end_policy == SchedulerEndPolicy::kAnyInput ? any : all;
    if (!completed) return Completion::kNone;
    if (failure) return Completion::kFailed;
    return stopped ? Completion::kStopped : Completion::kEof;
  }

  void Deliver(PendingTick tick) {
    auto video = tick.video.get();
    if (stopping_) return;
    MediaFrame frame;
    if (HasData(video)) frame.video = std::move(video);
    if (HasData(tick.audio)) frame.audio = std::move(tick.audio);
    if (on_output_ && (frame.video || frame.audio)) on_output_(frame);
  }

  void Pace(std::optional<Clock::time_point>& origin, std::uint64_t iteration) {
    if (!origin) origin = Clock::now();
    const auto offset =
        av_rescale_q(static_cast<std::int64_t>(iteration),
                     av_inv_q(config_.output.fps), kNanoseconds);
    const auto deadline = *origin + std::chrono::nanoseconds(offset);
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_until(lock, deadline, [this] { return stopping_.load(); });
  }

  void Run() noexcept {
    Completion completion = Completion::kNone;
    std::exception_ptr failure;
    try {
      MW_LOG_INFO("streamer", "Scheduler 频道启动: inputs={} fps={}/{}",
                  slots_.size(), config_.output.fps.num,
                  config_.output.fps.den);
      std::deque<PendingTick> pending;
      std::optional<Clock::time_point> origin;
      std::uint64_t number = 0;
      while (!stopping_) {
        SchedulerTick tick{number, Samples(number)};
        auto inputs =
            std::make_shared<const Inputs>(Collect(tick.audio_samples));
        // Check the whole set before submitting either filter: an any-input
        // EOF must not create an incomplete final composition.
        completion = Completed(failure);
        if (completion != Completion::kNone || stopping_) break;
        auto video = video_->Submit([this, inputs, tick] {
          return on_video_ ? on_video_(*inputs, tick)
                           : SelectTrack(*inputs, true);
        });
        auto audio =
            on_audio_ ? on_audio_(*inputs, tick) : SelectTrack(*inputs, false);
        pending.push_back({std::move(video), std::move(audio)});
        if (pending.size() > 1) {
          auto previous = std::move(pending.front());
          pending.pop_front();
          Deliver(std::move(previous));
        }
        MW_LOG_TRACE("streamer", "Scheduler 拍结束: tick={} audio_samples={}",
                     number, tick.audio_samples);
        Pace(origin, number);
        ++number;
      }
      while (!stopping_ && !pending.empty()) {
        auto previous = std::move(pending.front());
        pending.pop_front();
        Deliver(std::move(previous));
      }
    } catch (...) {
      completion = Completion::kFailed;
      failure = std::current_exception();
    }
    // In cancellation/error paths a submitted video task may still be running.
    // Join it before terminal notifications allow the user to free filters.
    video_.reset();
    if (stopping_ || completion == Completion::kStopped) {
      SetStatus(SchedulerState::kStopped);
    } else if (completion == Completion::kFailed) {
      SetStatus(SchedulerState::kFailed, failure);
      try {
        std::rethrow_exception(failure);
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer", "Scheduler 频道失败: {}", error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "Scheduler 频道发生未知失败");
      }
      if (on_failed_) on_failed_(failure);
    } else {
      SetStatus(SchedulerState::kEnded);
      MW_LOG_INFO("streamer", "Scheduler 频道 EOF，已排空短流水");
      if (on_eof_) on_eof_();
      if (on_ended_) on_ended_();
    }
  }

  SchedulerConfig config_;
  ffmpeg::Frame audio_format_;
  int max_samples_ = 0;
  bool variable_cadence_ = false;
  std::vector<InputSlot> slots_;
  std::unique_ptr<VideoExecutor> video_;
  std::thread worker_;
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
void Scheduler::SetOnEof(OnEof callback) {
  impl_->CheckConfigurable();
  impl_->on_eof_ = std::move(callback);
}
void Scheduler::SetOnEnded(OnEnded callback) {
  impl_->CheckConfigurable();
  impl_->on_ended_ = std::move(callback);
}
void Scheduler::SetOnFailed(OnFailed callback) {
  impl_->CheckConfigurable();
  impl_->on_failed_ = std::move(callback);
}
void Scheduler::Start(std::vector<std::shared_ptr<AvMediaInput>> inputs) {
  impl_->Start(std::move(inputs));
}
SchedulerStatus Scheduler::status() const { return impl_->status(); }
void Scheduler::RequestStop() noexcept { impl_->RequestStop(); }
void Scheduler::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
