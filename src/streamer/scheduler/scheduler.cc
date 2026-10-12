#include "mw/streamer/scheduler/scheduler.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/performance/performance.h"
#include "mw/streamer/platform/platform.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;
constexpr AVRational kNanoseconds{1, 1000000000};
constexpr int kChannelAudioRate = 48000;

void Check(int result, const char* operation) {
  ffmpeg::FfmpegException::throwIfError(result, operation);
}

struct SourceTracks {
  int video = -1;
  int audio = -1;
};

SourceTracks DescribeSource(const DecodedInputSource& source) {
  SourceTracks tracks;
  for (std::size_t index = 0; index < source.tracks.size(); ++index) {
    const auto& track = source.tracks[index];
    track.stream.Validate();
    if (!track.try_receive || !track.finished || !track.end_pts)
      throw std::invalid_argument("Scheduler 解码源回调不完整");
    const auto type = track.stream.codec_parameters.get()->codec_type;
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
      throw std::invalid_argument("Scheduler 解码源只能包含音视频轨道");
    auto& position = type == AVMEDIA_TYPE_VIDEO ? tracks.video : tracks.audio;
    if (position >= 0)
      throw std::invalid_argument("Scheduler 解码源每种类型最多包含一条轨道");
    position = static_cast<int>(index);
  }
  if (tracks.video < 0 && tracks.audio < 0)
    throw std::invalid_argument("Scheduler 解码源没有音视频轨道");
  return tracks;
}

int Cadence(std::int64_t tick, const SchedulerConfig& config,
            bool producer = false) {
  // Original progressive format vectors in core/video_format.cpp. Producer
  // pre-rotates right once, then once per bundle. A fresh channel increments
  // frame_counter before stage indexes the unrotated vector.
  constexpr std::array<int, 5> p5994{801, 800, 801, 801, 801};
  constexpr std::array<int, 5> p2997{1602, 1601, 1602, 1601, 1602};
  const std::array<int, 5>* reference = nullptr;
  if (av_cmp_q(config.video_frame_rate, AVRational{60000, 1001}) == 0)
    reference = &p5994;
  else if (av_cmp_q(config.video_frame_rate, AVRational{30000, 1001}) == 0)
    reference = &p2997;
  if (reference) {
    const auto phase = static_cast<std::size_t>(tick % 5);
    return (*reference)[producer ? 4 - phase : (phase + 1) % 5];
  }
  const auto numerator = static_cast<std::int64_t>(kChannelAudioRate) *
                         config.video_frame_rate.den;
  const auto denominator = config.video_frame_rate.num;
  if (denominator <= 0 || numerator % denominator != 0 ||
      numerator / denominator <= 0 ||
      numerator / denominator > std::numeric_limits<int>::max())
    throw std::invalid_argument("Scheduler 帧率没有对应的 48kHz 音频 cadence");
  return static_cast<int>(numerator / denominator);
}

// One persistent executor per filter. The producer joins
// both submitted operations before touching either graph or pending frame.
class FilterExecutor final {
 public:
  FilterExecutor() : worker_([this] { Run(); }) {}
  ~FilterExecutor() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_one();
    worker_.join();
  }

  template <typename Work>
  std::future<bool> Submit(Work&& work) {
    std::packaged_task<bool()> task(std::forward<Work>(work));
    auto future = task.get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || task_)
        throw std::logic_error("Scheduler 滤镜任务状态错误");
      task_.emplace(std::move(task));
    }
    changed_.notify_one();
    return future;
  }

 private:
  void Run() noexcept {
    while (true) {
      std::optional<std::packaged_task<bool()>> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return stopping_ || task_; });
        if (!task_) return;
        task = std::move(task_);
        task_.reset();
      }
      // packaged_task transfers filter exceptions to its future.
      (*task)();
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::optional<std::packaged_task<bool()>> task_;
  std::thread worker_;
};

class SourceFilter final {
 public:
  SourceFilter(InputFrameSource source, ffmpeg::Frame first, bool video,
               std::int64_t start_us, const SchedulerConfig& config)
      : source_(std::move(source)), video_(video) {
    graph_.reset(avfilter_graph_alloc());
    if (!graph_) throw std::bad_alloc();
    source_context_ = avfilter_graph_alloc_filter(
        graph_.get(), avfilter_get_by_name(video ? "buffer" : "abuffer"), "in");
    if (!source_context_) throw std::bad_alloc();
    auto* parameters = av_buffersrc_parameters_alloc();
    if (!parameters) throw std::bad_alloc();
    const std::unique_ptr<AVBufferSrcParameters,
                          void (*)(AVBufferSrcParameters*)>
        source_parameters(parameters, [](AVBufferSrcParameters* value) {
          av_channel_layout_uninit(&value->ch_layout);
          av_free(value);
        });
    parameters->format = first->format;
    parameters->time_base = source_.stream.time_base;
    if (video) {
      parameters->width = first->width;
      parameters->height = first->height;
      parameters->sample_aspect_ratio = first->sample_aspect_ratio;
      if (parameters->sample_aspect_ratio.num <= 0 ||
          parameters->sample_aspect_ratio.den <= 0)
        parameters->sample_aspect_ratio = AVRational{1, 1};
      parameters->frame_rate = source_.stream.codec_parameters.get()->framerate;
      parameters->hw_frames_ctx = first->hw_frames_ctx;
      parameters->color_space = first->colorspace;
      parameters->color_range = first->color_range;
      parameters->alpha_mode = first->alpha_mode;
    } else {
      parameters->sample_rate = first->sample_rate;
      Check(av_channel_layout_copy(&parameters->ch_layout, &first->ch_layout),
            "设置 Scheduler 音频源布局");
    }
    Check(av_buffersrc_parameters_set(source_context_, parameters),
          "设置 Scheduler 滤镜源参数");
    Check(avfilter_init_str(source_context_, nullptr),
          "初始化 Scheduler 滤镜源");
    sink_ = avfilter_graph_alloc_filter(
        graph_.get(),
        avfilter_get_by_name(video ? "buffersink" : "abuffersink"), "out");
    if (!sink_) throw std::bad_alloc();
    if (!video) {
      const AVSampleFormat formats[]{AV_SAMPLE_FMT_S32};
      const int rates[]{kChannelAudioRate};
      Check(av_opt_set_array(sink_, "sample_formats",
                             AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0,
                             1, AV_OPT_TYPE_SAMPLE_FMT, formats),
            "设置 Scheduler 音频输出格式");
      Check(av_opt_set_array(sink_, "samplerates",
                             AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0,
                             1, AV_OPT_TYPE_INT, rates),
            "设置 Scheduler 音频输出采样率");
      Check(av_opt_set_int(sink_, "all_channel_counts", 1,
                           AV_OPT_SEARCH_CHILDREN),
            "设置 Scheduler 音频输出声道");
    }
    Check(avfilter_init_str(sink_, nullptr), "初始化 Scheduler 滤镜输出");
    const auto specification =
        video ? fmt::format(
                    "{}=mode=send_field:parity=auto:deint=interlaced,"
                    "fps=fps={}/{}:start_time={:.6f}",
                    first->format == AV_PIX_FMT_CUDA ? "bwdif_cuda" : "bwdif",
                    config.video_frame_rate.num, config.video_frame_rate.den,
                    static_cast<double>(start_us) / AV_TIME_BASE)
              : fmt::format(
                    "aresample=async=1000:first_pts={}:min_comp=0.01:osr={},"
                    "asetnsamples=n=1024:p=0",
                    av_rescale_q(start_us, AVRational{1, AV_TIME_BASE},
                                 AVRational{1, first->sample_rate}),
                    kChannelAudioRate);
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
      avfilter_inout_free(&inputs);
      avfilter_inout_free(&outputs);
      throw std::bad_alloc();
    }
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_;
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source_context_;
    const int parsed = avfilter_graph_parse_ptr(
        graph_.get(), specification.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    Check(parsed, "解析 Scheduler 滤镜");
    Check(avfilter_graph_config(graph_.get(), nullptr), "配置 Scheduler 滤镜");
    Check(av_buffersrc_write_frame(source_context_, first.get()),
          "提交 Scheduler 首帧");
    MW_LOG_TRACE("streamer",
                 "Scheduler source first: stream={}, video={}, pts={}, "
                 "duration={}, time_base={}/{}",
                 source_.stream.stream_index, video_, first->pts,
                 first->duration, source_.stream.time_base.num,
                 source_.stream.time_base.den);
  }

  bool Pump(bool& accepted) {
    accepted = false;
    if (closed_) return false;
    if (source_.finished()) {
      Check(av_buffersrc_close(source_context_, source_.end_pts(), 0),
            "关闭 Scheduler 轨道输入");
      closed_ = true;
      return true;
    }
    if (!av_buffersrc_get_nb_failed_requests(source_context_)) return false;
    ffmpeg::Frame frame;
    if (!source_.try_receive(frame)) return false;
    Check(av_buffersrc_write_frame(source_context_, frame.get()),
          "提交 Scheduler 源帧");
    MW_LOG_TRACE("streamer",
                 "Scheduler source frame: stream={}, video={}, pts={}, "
                 "duration={}, time_base={}/{}",
                 source_.stream.stream_index, video_, frame->pts,
                 frame->duration, source_.stream.time_base.num,
                 source_.stream.time_base.den);
    accepted = true;
    return true;
  }

  bool Pull(std::optional<ffmpeg::Frame>& output, int samples) {
    if (ended_ || output) return false;
    ffmpeg::Frame frame;
    const int result =
        video_ ? av_buffersink_get_frame(sink_, frame.get())
               : av_buffersink_get_samples(sink_, frame.get(), samples);
    if (result == AVERROR(EAGAIN)) return false;
    if (result == AVERROR_EOF) {
      ended_ = true;
      return true;
    }
    Check(result, "读取 Scheduler 滤镜帧");
    frame->time_base = av_buffersink_get_time_base(sink_);
    MW_LOG_TRACE("streamer",
                 "Scheduler filter frame: video={}, pts={}, source_pts={}, "
                 "samples={}, time_base={}/{}",
                 video_, frame->pts, frame->best_effort_timestamp,
                 frame->nb_samples, frame->time_base.num, frame->time_base.den);
    output.emplace(std::move(frame));
    return true;
  }

  bool ended() const noexcept { return ended_; }

  ffmpeg::StreamInfo OutputInfo(const SchedulerConfig& config) const {
    auto result = source_.stream;
    auto* parameters = result.codec_parameters.get();
    parameters->format = av_buffersink_get_format(sink_);
    if (video_) {
      parameters->width = av_buffersink_get_w(sink_);
      parameters->height = av_buffersink_get_h(sink_);
      parameters->framerate = config.video_frame_rate;
      parameters->field_order = AV_FIELD_PROGRESSIVE;
      parameters->sample_aspect_ratio =
          av_buffersink_get_sample_aspect_ratio(sink_);
      parameters->color_space = av_buffersink_get_colorspace(sink_);
      parameters->color_range = av_buffersink_get_color_range(sink_);
      result.time_base = av_inv_q(config.video_frame_rate);
      if (parameters->format == AV_PIX_FMT_CUDA) {
        const auto* context = av_buffersink_get_hw_frames_ctx(sink_);
        if (context)
          parameters->format =
              reinterpret_cast<const AVHWFramesContext*>(context->data)
                  ->sw_format;
      }
    } else {
      parameters->sample_rate = kChannelAudioRate;
      av_channel_layout_uninit(&parameters->ch_layout);
      Check(av_buffersink_get_ch_layout(sink_, &parameters->ch_layout),
            "读取 Scheduler 输出声道布局");
      result.time_base = AVRational{1, kChannelAudioRate};
    }
    return result;
  }

 private:
  struct GraphDeleter {
    void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
  };
  InputFrameSource source_;
  bool video_;
  bool closed_ = false;
  bool ended_ = false;
  std::unique_ptr<AVFilterGraph, GraphDeleter> graph_;
  AVFilterContext* source_context_ = nullptr;
  AVFilterContext* sink_ = nullptr;
};

ffmpeg::Frame AudioBlock(const ffmpeg::Frame& prototype,
                         const std::optional<ffmpeg::Frame>& source,
                         int samples, int max_cadence, bool variable_cadence,
                         std::optional<std::vector<std::int32_t>>& carry) {
  if (!variable_cadence && source && (*source)->nb_samples == samples)
    return source->Ref();
  ffmpeg::Frame result;
  result->format = AV_SAMPLE_FMT_S32;
  result->sample_rate = prototype->sample_rate;
  result->nb_samples = samples;
  Check(av_channel_layout_copy(&result->ch_layout, &prototype->ch_layout),
        "设置 Scheduler 音频块布局");
  Check(av_frame_get_buffer(result.get(), 0), "分配 Scheduler 音频块");
  Check(
      av_samples_set_silence(result->extended_data, 0, samples,
                             result->ch_layout.nb_channels, AV_SAMPLE_FMT_S32),
      "填充 Scheduler 静音");
  if (!source) {
    carry.reset();
    return result;
  }
  const auto channels = static_cast<std::size_t>(result->ch_layout.nb_channels);
  const auto source_size =
      static_cast<std::size_t>((*source)->nb_samples) * channels;
  const auto destination_size = static_cast<std::size_t>(samples) * channels;
  const auto* input = reinterpret_cast<const std::int32_t*>((*source)->data[0]);
  auto* output = reinterpret_cast<std::int32_t*>(result->data[0]);
  // Fractional sample cadence retains a small remainder and inserts one sample
  // of silence on initial/resumed use.
  if (variable_cadence && !carry) carry.emplace(channels, 0);
  const auto previous_size = variable_cadence ? carry->size() : 0;
  for (std::size_t n = 0; n < destination_size; ++n) {
    if (n < previous_size)
      output[n] = (*carry)[n];
    else if (n < previous_size + source_size)
      output[n] = input[n - previous_size];
    else
      output[n] = input[source_size - channels + n % channels];
  }
  if (variable_cadence) {
    if (previous_size + source_size > destination_size) {
      auto remaining = previous_size + source_size - destination_size;
      const auto maximum = 2 * static_cast<std::size_t>(max_cadence) * channels;
      if (remaining > maximum || remaining > source_size) {
        MW_LOG_WARNING(
            "streamer",
            "Scheduler 音频缓存溢出: remaining={}, maximum={}, source={}",
            remaining, maximum, source_size);
        remaining = std::min(maximum, source_size);
      }
      const auto offset = destination_size > previous_size
                              ? destination_size - previous_size
                              : 0;
      carry.emplace(input + offset, input + offset + remaining);
    } else {
      carry.emplace();
    }
  }
  return result;
}

}  // namespace

class Scheduler::Impl {
 public:
  explicit Impl(SchedulerConfig config) : config_(std::move(config)) {}
  ~Impl() { Stop(); }

  bool Start(const DecodedInputSource& source) noexcept {
    try {
      if (config_.video_frame_rate.num <= 0 ||
          config_.video_frame_rate.den <= 0 || Cadence(0, config_) <= 0)
        return false;
      const auto tracks = DescribeSource(source);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return false;
        sources_.push_back(source);
        started_ = true;
        stopping_.store(false);
        draining_ = producer_done_ = ready_notified_ = false;
        frame_flush_ = true;
        source_flush_requested_ = false;
        capacity_ =
            std::max(1, static_cast<int>(av_q2d(config_.video_frame_rate)) / 4);
        performance_.Start(this, config_.video_frame_rate, kChannelAudioRate,
                           Cadence(0, config_), tracks.video >= 0,
                           tracks.audio >= 0);
      }
      producer_ = std::thread(&Impl::Produce, this);
      consumer_ = std::thread(&Impl::Consume, this);
      return true;
    } catch (const std::exception& error) {
      MW_LOG_ERROR("streamer", "Scheduler 启动失败: {}", error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "Scheduler 启动发生未知错误");
    }
    Stop();
    return false;
  }

  bool QueueSource(const DecodedInputSource& source) noexcept {
    try {
      DescribeSource(source);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopping_.load() || draining_) return false;
        if (source.flush) {
          sources_.clear();
          bundles_.clear();
          frame_flush_ = true;
          source_flush_requested_ = true;
        }
        sources_.push_back(source);
      }
      changed_.notify_all();
      return true;
    } catch (const std::exception& error) {
      Fail(AVERROR_EXTERNAL, error.what());
    } catch (...) {
      Fail(AVERROR_UNKNOWN, "Scheduler 追加解码源失败");
    }
    return false;
  }

  void Drain() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      draining_ = true;
    }
    changed_.notify_all();
  }

  void RequestStop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_.store(true);
    }
    changed_.notify_all();
  }

  void Stop() noexcept {
    RequestStop();
    if (producer_.joinable()) producer_.join();
    if (consumer_.joinable()) consumer_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    performance_.Finish(bundles_.size(), 0);
    bundles_.clear();
    sources_.clear();
    started_ = false;
  }

  OnReady on_ready_;
  OnFrame on_frame_;
  OnEnded on_ended_;
  OnError on_error_;

 private:
  void Fail(int code, std::string_view message) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_.exchange(true)) return;
      performance_.Error(true);
    }
    changed_.notify_all();
    MW_LOG_ERROR("streamer", "Scheduler 运行失败: code={}, reason={}", code,
                 message);
    if (on_error_) {
      try {
        on_error_(code, message);
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer", "Scheduler 错误回调异常: {}", error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "Scheduler 错误回调发生未知异常");
      }
    }
  }

  std::unique_ptr<SourceFilter> MakeFilter(const DecodedInputSource& source,
                                           int index, bool video) {
    if (index < 0) return {};
    const auto& track = source.tracks[static_cast<std::size_t>(index)];
    ffmpeg::Frame frame;
    while (!stopping_.load() && !SourceFlushRequested()) {
      if (track.try_receive(frame)) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          performance_.Accept(video);
        }
        return std::make_unique<SourceFilter>(track, std::move(frame), video,
                                              source.start_time_us, config_);
      }
      if (track.finished()) return {};
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait_for(lock, std::chrono::milliseconds(1),
                        [this] { return stopping_.load(); });
    }
    return {};
  }

  bool SourceFlushRequested() {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_flush_requested_;
  }

  void Produce() noexcept {
    try {
      FilterExecutor video_executor;
      FilterExecutor audio_executor;
      std::int64_t produced_ticks = 0;
      while (!stopping_.load()) {
        DecodedInputSource source;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait(lock, [this] {
            return stopping_.load() || !sources_.empty() || draining_;
          });
          if (stopping_.load()) break;
          if (sources_.empty()) break;
          source = std::move(sources_.front());
          sources_.pop_front();
          source_flush_requested_ = false;
        }
        const auto tracks = DescribeSource(source);
        auto video = MakeFilter(source, tracks.video, true);
        auto audio = MakeFilter(source, tracks.audio, false);
        if (stopping_.load()) break;
        if (SourceFlushRequested()) continue;
        if (!ready_notified_ && (video || audio)) {
          std::vector<ffmpeg::StreamInfo> streams;
          if (video) streams.push_back(video->OutputInfo(config_));
          if (audio) streams.push_back(audio->OutputInfo(config_));
          if (on_ready_) on_ready_(streams);
          ready_notified_ = true;
        }
        MediaFrame pending;
        std::optional<ffmpeg::Frame> last_audio;
        std::int64_t segment_frames = 0;
        const auto origin_us = source.media_origin_us;
        const auto end_us =
            source.duration_us == AV_NOPTS_VALUE
                ? AV_NOPTS_VALUE
                : source.start_time_us - origin_us + source.duration_us;
        std::int64_t segment_end_us = 0;
        while (!stopping_.load() && !SourceFlushRequested()) {
          // Check the prior frame's media end before producing the next frame.
          // Both endpoints use the container origin, including
          // after seeking; subtracting the seek position before rounding can
          // change the final channel tick.
          if (end_us != AV_NOPTS_VALUE &&
              av_rescale_q(segment_end_us, AVRational{1, AV_TIME_BASE},
                           av_inv_q(config_.video_frame_rate)) >=
                  av_rescale_q(end_us, AVRational{1, AV_TIME_BASE},
                               av_inv_q(config_.video_frame_rate)))
            break;
          bool progress = false;
          const int samples = Cadence(produced_ticks, config_, true);
          std::array<std::future<bool>, 2> futures;
          std::exception_ptr failure;
          try {
            if (video && !pending.video)
              futures[0] = video_executor.Submit(
                  [&] { return video->Pull(pending.video, samples); });
            if (audio && !pending.audio)
              futures[1] = audio_executor.Submit(
                  [&] { return audio->Pull(pending.audio, samples); });
          } catch (...) {
            failure = std::current_exception();
          }
          for (auto& future : futures) {
            if (!future.valid()) continue;
            try {
              progress = future.get() || progress;
            } catch (...) {
              if (!failure) failure = std::current_exception();
            }
          }
          if (failure) std::rethrow_exception(failure);
          if (stopping_.load() || SourceFlushRequested()) break;
          const bool vdone = !video || video->ended();
          const bool adone = !audio || audio->ended();
          if (vdone && adone && !pending.video && !pending.audio) {
            break;
          } else if ((pending.video || vdone) && (pending.audio || adone)) {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] {
              return stopping_.load() || source_flush_requested_ ||
                     bundles_.size() < static_cast<std::size_t>(capacity_);
            });
            if (stopping_.load() || source_flush_requested_) break;
            if (pending.video && (*pending.video)->pts != AV_NOPTS_VALUE)
              segment_end_us =
                  av_rescale_q((*pending.video)->pts,
                               (*pending.video)->time_base,
                               AVRational{1, AV_TIME_BASE}) -
                  origin_us +
                  av_rescale_q(1, av_inv_q(config_.video_frame_rate),
                               AVRational{1, AV_TIME_BASE});
            if (pending.audio && (*pending.audio)->pts != AV_NOPTS_VALUE)
              segment_end_us =
                  av_rescale_q((*pending.audio)->pts,
                               (*pending.audio)->time_base,
                               AVRational{1, AV_TIME_BASE}) -
                  origin_us +
                  av_rescale_q((*pending.audio)->nb_samples,
                               AVRational{1, (*pending.audio)->sample_rate},
                               AVRational{1, AV_TIME_BASE});
            // Retain the last audio block when the audio filter reaches EOF.
            // A later video frame carries that same tail block again.
            // Consumer underflow has no bundle and remains a silent still.
            if (pending.audio)
              last_audio = pending.audio->Ref();
            else if (adone && last_audio)
              pending.audio = last_audio->Ref();
            bundles_.push_back(std::move(pending));
            pending = MediaFrame{};
            ++produced_ticks;
            ++segment_frames;
            lock.unlock();
            changed_.notify_all();
            continue;
          }
          for (const bool is_video : {true, false}) {
            auto& filter = is_video ? video : audio;
            if (!filter) continue;
            bool accepted = false;
            progress = filter->Pump(accepted) || progress;
            if (accepted) {
              std::lock_guard<std::mutex> lock(mutex_);
              performance_.Accept(is_video);
            }
          }
          if (!progress) {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds(1),
                              [this] { return stopping_.load(); });
          }
        }
        bool completed = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          completed = !stopping_.load() && !source_flush_requested_;
          if (completed && source.loop && segment_frames > 2)
            frame_flush_ = true;
        }
        if (completed && source.complete) source.complete(segment_frames);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        producer_done_ = true;
      }
      changed_.notify_all();
    } catch (const ffmpeg::FfmpegException& error) {
      Fail(error.error_code(), error.what());
    } catch (const std::exception& error) {
      Fail(AVERROR_EXTERNAL, error.what());
    } catch (...) {
      Fail(AVERROR_UNKNOWN, "Scheduler 滤镜生产发生未知错误");
    }
  }

  void Consume() noexcept {
    try {
      internal::ScopedAudioScheduling scheduling;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] {
          // The first queued frame can be previewed without waiting for the
          // initial four-frame buffer threshold.
          return stopping_.load() || !bundles_.empty() || producer_done_;
        });
      }
      const auto output_period = std::chrono::microseconds(
          static_cast<int>(1e6 / av_q2d(config_.video_frame_rate)));
      std::optional<ffmpeg::Frame> last_video;
      std::optional<ffmpeg::Frame> audio_prototype;
      std::optional<std::vector<std::int32_t>> audio_carry;
      const bool variable_cadence =
          (static_cast<std::int64_t>(kChannelAudioRate) *
           config_.video_frame_rate.den) %
              config_.video_frame_rate.num !=
          0;
      int max_cadence = 0;
      for (int phase = 0; phase < 5; ++phase)
        max_cadence = std::max(max_cadence, Cadence(phase, config_));
      // Deliver the first media frame immediately, then wait one output period
      // before the next delivery.
      auto next_output_time = Clock::now() + output_period;
      std::int64_t tick = 0;
      std::int64_t audio_cursor = 0;
      while (!stopping_.load()) {
        const auto deadline = next_output_time;
        MediaFrame bundle;
        bool repeated = true;
        bool still = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (bundles_.empty() && producer_done_) break;
          if (!bundles_.empty()) {
            // After a reset with fewer than four queued frames, preview the
            // first picture without consuming it and mute its audio.
            still = frame_flush_ && bundles_.size() < 4;
            if (still) {
              if (bundles_.front().video)
                bundle.video = bundles_.front().video->Ref();
              if (bundles_.front().audio)
                bundle.audio = bundles_.front().audio->Ref();
            } else {
              bundle = std::move(bundles_.front());
              bundles_.pop_front();
            }
            frame_flush_ = false;
          }
        }
        changed_.notify_all();
        const auto work = Clock::now();
        if (bundle.video) {
          repeated =
              last_video && (*last_video)->buf[0] && (*bundle.video)->buf[0] &&
              (*last_video)->buf[0]->buffer == (*bundle.video)->buf[0]->buffer;
          last_video = bundle.video->Ref();
        }
        if (bundle.audio) audio_prototype = bundle.audio->Ref();
        MW_LOG_TRACE(
            "streamer",
            "Scheduler channel bundle: tick={}, video={}, repeated={}, "
            "audio={}, audio_pts={}, samples={}, still={}",
            tick, bundle.video.has_value(), repeated, bundle.audio.has_value(),
            audio_cursor, Cadence(tick, config_), still);
        MediaFrame output;
        if (last_video) {
          auto frame = last_video->Ref();
          frame->pts = tick;
          frame->duration = 1;
          frame->time_base = av_inv_q(config_.video_frame_rate);
          frame->pkt_dts = AV_NOPTS_VALUE;
          output.video = std::move(frame);
        }
        const int samples = Cadence(tick, config_);
        if (audio_prototype) {
          auto frame =
              AudioBlock(*audio_prototype, still ? std::nullopt : bundle.audio,
                         samples, max_cadence, variable_cadence, audio_carry);
          frame->pts = audio_cursor;
          frame->duration = samples;
          frame->time_base = AVRational{1, kChannelAudioRate};
          frame->pkt_dts = AV_NOPTS_VALUE;
          output.audio = std::move(frame);
        }
        // A channel frame is delivered once; both track statistics measure the
        // same callback boundary, including synchronous processing and submit.
        const auto callback_started = performance_.BeginWork();
        if (on_frame_) on_frame_(output);
        const auto callback_ns = performance_.WorkTime(callback_started);
        audio_cursor += samples;
        ++tick;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (bundle.video && !repeated) performance_.SelectVideo();
          if (last_video)
            performance_.DeliverVideo(repeated, !!on_frame_, callback_ns);
          if (audio_prototype) {
            if (still || !bundle.audio) performance_.AudioUnderload();
            performance_.BeginAudioBatch();
            performance_.BeginAudioFrame();
            if (bundle.audio && !still)
              performance_.CopyAudio(
                  std::min(samples, (*bundle.audio)->nb_samples));
            performance_.PrepareAudioFrame();
            performance_.DeliverAudio(0, samples, !!on_frame_, callback_ns);
          }
          performance_.Tick(
              true, work, deadline,
              av_rescale_q(1, av_inv_q(config_.video_frame_rate), kNanoseconds),
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  Clock::now() - work)
                  .count(),
              bundles_.size(), 0);
          if (audio_prototype)
            performance_.Tick(
                false, work, deadline,
                av_rescale_q(1, av_inv_q(config_.video_frame_rate),
                             kNanoseconds),
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - work)
                    .count(),
                bundles_.size(), 0);
        }
        // Match initialized software output: send, wait, then advance by
        // its whole-microsecond period. The platform clock remains monotonic
        // and its wait is interruptible for Stop.
        if (!internal::WaitUntil(changed_, mutex_, stopping_, next_output_time,
                                 true))
          break;
        next_output_time += output_period;
      }
      if (!stopping_.load() && on_ended_) on_ended_();
    } catch (const ffmpeg::FfmpegException& error) {
      Fail(error.error_code(), error.what());
    } catch (const std::exception& error) {
      Fail(AVERROR_EXTERNAL, error.what());
    } catch (...) {
      Fail(AVERROR_UNKNOWN, "Scheduler 节拍消费发生未知错误");
    }
  }

  SchedulerConfig config_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::atomic<bool> stopping_{true};
  bool started_ = false;
  bool draining_ = false;
  bool producer_done_ = false;
  bool ready_notified_ = false;
  bool frame_flush_ = true;
  bool source_flush_requested_ = false;
  int capacity_ = 1;
  std::deque<DecodedInputSource> sources_;
  std::deque<MediaFrame> bundles_;
  std::thread producer_;
  std::thread consumer_;
  internal::SchedulerPerformance performance_;
};

Scheduler::Scheduler(SchedulerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Scheduler::~Scheduler() = default;
void Scheduler::SetOnReady(OnReady callback) noexcept {
  impl_->on_ready_ = std::move(callback);
}
void Scheduler::SetOnFrame(OnFrame callback) noexcept {
  impl_->on_frame_ = std::move(callback);
}
void Scheduler::SetOnEnded(OnEnded callback) noexcept {
  impl_->on_ended_ = std::move(callback);
}
void Scheduler::SetOnError(OnError callback) noexcept {
  impl_->on_error_ = std::move(callback);
}
bool Scheduler::Start(const DecodedInputSource& source) noexcept {
  return impl_->Start(source);
}
bool Scheduler::QueueSource(const DecodedInputSource& source) noexcept {
  return impl_->QueueSource(source);
}
void Scheduler::Drain() noexcept { impl_->Drain(); }
void Scheduler::RequestStop() noexcept { impl_->RequestStop(); }
void Scheduler::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
