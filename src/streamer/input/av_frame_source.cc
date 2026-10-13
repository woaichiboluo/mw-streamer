#include "mw/streamer/input/av_frame_source.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <fmt/format.h>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer {
namespace {

void Check(int code, const char* operation) {
  ffmpeg::FfmpegException::throwIfError(code, operation);
}

struct GraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

struct ParametersDeleter {
  void operator()(AVBufferSrcParameters* parameters) const {
    av_channel_layout_uninit(&parameters->ch_layout);
    // hw_frames_ctx is borrowed here; parameters_set retains its own reference.
    av_free(parameters);
  }
};

AVFilterContext* CreateFilter(AVFilterGraph* graph, const char* name,
                              const char* instance,
                              const char* args = nullptr) {
  const auto* filter = avfilter_get_by_name(name);
  if (!filter) {
    throw ffmpeg::FfmpegException(AVERROR_FILTER_NOT_FOUND, name);
  }
  AVFilterContext* context = nullptr;
  Check(avfilter_graph_create_filter(&context, filter, instance, args, nullptr,
                                     graph),
        "avfilter_graph_create_filter");
  return context;
}

// This graph owns only libavfilter's buffering; the surrounding input drives
// demand and retains the returned frame until its consumer has used it.
class TrackFilter final {
 public:
  TrackFilter(const ffmpeg::StreamInfo& stream, const AvEncoderConfig& config,
              const ffmpeg::Frame& first, std::int64_t start_time_us)
      : video_(stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO),
        graph_(avfilter_graph_alloc()) {
    if (!graph_) throw std::bad_alloc();
    const auto* source_filter =
        avfilter_get_by_name(video_ ? "buffer" : "abuffer");
    if (!source_filter) {
      throw ffmpeg::FfmpegException(AVERROR_FILTER_NOT_FOUND, "buffer source");
    }
    source_ = avfilter_graph_alloc_filter(graph_.get(), source_filter, "in");
    if (!source_) throw std::bad_alloc();
    std::unique_ptr<AVBufferSrcParameters, ParametersDeleter> parameters(
        av_buffersrc_parameters_alloc());
    if (!parameters) throw std::bad_alloc();
    parameters->format = first->format;
    parameters->time_base = stream.time_base;
    if (video_) {
      parameters->width = first->width;
      parameters->height = first->height;
      parameters->sample_aspect_ratio = first->sample_aspect_ratio;
      parameters->frame_rate = stream.frame_rate;
      parameters->hw_frames_ctx = first->hw_frames_ctx;
      parameters->color_space = first->colorspace;
      parameters->color_range = first->color_range;
      parameters->alpha_mode = first->alpha_mode;
    } else {
      parameters->sample_rate = first->sample_rate;
      Check(av_channel_layout_copy(&parameters->ch_layout, &first->ch_layout),
            "复制 AvFrameSource 输入声道布局");
    }
    Check(av_buffersrc_parameters_set(source_, parameters.get()),
          "av_buffersrc_parameters_set");
    Check(avfilter_init_str(source_, nullptr), "avfilter_init_str source");

    const auto* sink_filter =
        avfilter_get_by_name(video_ ? "buffersink" : "abuffersink");
    if (!sink_filter) {
      throw ffmpeg::FfmpegException(AVERROR_FILTER_NOT_FOUND, "buffer sink");
    }
    sink_ = avfilter_graph_alloc_filter(graph_.get(), sink_filter, "out");
    if (!sink_) throw std::bad_alloc();
    constexpr int kArrayFlags = AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE;
    if (video_) {
      // fps must preserve the input layout, including CUDA surfaces. Target
      // canvas dimensions/layout are the later compositor's responsibility.
      const AVPixelFormat format = static_cast<AVPixelFormat>(first->format);
      Check(av_opt_set_array(sink_, "pixel_formats", kArrayFlags, 0, 1,
                             AV_OPT_TYPE_PIXEL_FMT, &format),
            "配置 AvFrameSource 视频格式");
    } else {
      Check(
          av_opt_set_array(sink_, "sample_formats", kArrayFlags, 0, 1,
                           AV_OPT_TYPE_SAMPLE_FMT, &config.audio_sample_format),
          "配置 AvFrameSource 音频格式");
      Check(av_opt_set_array(sink_, "samplerates", kArrayFlags, 0, 1,
                             AV_OPT_TYPE_INT, &config.audio_sample_rate),
            "配置 AvFrameSource 音频采样率");
      Check(
          av_opt_set_array(sink_, "channel_layouts", kArrayFlags, 0, 1,
                           AV_OPT_TYPE_CHLAYOUT, &config.audio_channel_layout),
          "配置 AvFrameSource 输出声道布局");
    }
    Check(avfilter_init_str(sink_, nullptr), "avfilter_init_str sink");

    if (video_) {
      auto args = fmt::format("fps={}/{}", config.fps.num, config.fps.den);
      if (start_time_us != AV_NOPTS_VALUE) {
        args += fmt::format(":start_time={:.6f}",
                            static_cast<double>(start_time_us) / AV_TIME_BASE);
      }
      auto* fps = CreateFilter(graph_.get(), "fps", "fps", args.c_str());
      Check(avfilter_link(source_, 0, fps, 0), "avfilter_link fps input");
      Check(avfilter_link(fps, 0, sink_, 0), "avfilter_link fps output");
    } else {
      auto args = fmt::format("async=1000:min_comp=0.01:osr={}",
                              config.audio_sample_rate);
      if (start_time_us != AV_NOPTS_VALUE) {
        // first_pts is expressed in INPUT samples, even when rates differ.
        args +=
            fmt::format(":first_pts={}",
                        av_rescale_q(start_time_us, AVRational{1, AV_TIME_BASE},
                                     AVRational{1, first->sample_rate}));
      }
      auto* resample =
          CreateFilter(graph_.get(), "aresample", "resample", args.c_str());
      auto* samples =
          CreateFilter(graph_.get(), "asetnsamples", "samples", "n=1024:p=0");
      Check(avfilter_link(source_, 0, resample, 0),
            "avfilter_link resample input");
      Check(avfilter_link(resample, 0, samples, 0),
            "avfilter_link resample output");
      Check(avfilter_link(samples, 0, sink_, 0), "avfilter_link audio output");
    }
    Check(avfilter_graph_config(graph_.get(), nullptr),
          "avfilter_graph_config");
  }

  void PushFrame(const ffmpeg::Frame& frame) {
    Check(av_buffersrc_write_frame(source_, frame.get()),
          "av_buffersrc_write_frame");
  }

  void PushEof(std::int64_t end_pts) {
    Check(av_buffersrc_close(source_, end_pts, 0), "av_buffersrc_close");
  }

  int Read(ffmpeg::Frame& frame, int samples) {
    // FFmpeg forbids mixing get_frame and get_samples on an audio sink.
    const int result =
        video_ ? av_buffersink_get_frame(sink_, frame.get())
               : av_buffersink_get_samples(sink_, frame.get(),
                                           samples == 0 ? 1024 : samples);
    if (result >= 0) frame->time_base = time_base();
    return result;
  }

  AVRational time_base() const { return av_buffersink_get_time_base(sink_); }
  bool wants_frame() const {
    return av_buffersrc_get_nb_failed_requests(source_) != 0;
  }

 private:
  const bool video_;
  std::unique_ptr<AVFilterGraph, GraphDeleter> graph_;
  AVFilterContext* source_ = nullptr;
  AVFilterContext* sink_ = nullptr;
};

}  // namespace

class AvFrameSource::Impl final {
 public:
  Impl(AvDecoder* video, AvDecoder* audio, AvEncoderConfig config,
       std::int64_t start_time_us, OnFailed on_failed)
      : config_(std::move(config)),
        initial_start_us_(start_time_us),
        on_failed_(std::move(on_failed)),
        video_(video, AVMEDIA_TYPE_VIDEO),
        audio_(audio, AVMEDIA_TYPE_AUDIO) {
    if ((!video && !audio) || config_.fps.num <= 0 || config_.fps.den <= 0) {
      throw std::invalid_argument("AvFrameSource 需要轨道和有效输出帧率");
    }
    capacity_ = std::max(
        std::size_t{1},
        static_cast<std::size_t>(config_.fps.num / config_.fps.den) / 4);
    if (audio) {
      if (config_.audio_sample_rate <= 0 ||
          !av_get_sample_fmt_name(config_.audio_sample_format) ||
          !av_channel_layout_check(&config_.audio_channel_layout)) {
        throw std::invalid_argument("AvFrameSource 音频输出配置无效");
      }
      const auto numerator =
          static_cast<std::int64_t>(config_.audio_sample_rate) *
          config_.fps.den;
      if (numerator < config_.fps.num ||
          av_rescale_rnd(numerator, 1, config_.fps.num, AV_ROUND_UP) >
              std::numeric_limits<int>::max()) {
        throw std::invalid_argument("AvFrameSource 每周期音频数量超出范围");
      }
      Check(av_channel_layout_copy(&audio_parameters_.get()->ch_layout,
                                   &config_.audio_channel_layout),
            "复制 AvFrameSource 输出声道布局");
      config_.audio_channel_layout = audio_parameters_.get()->ch_layout;
    }
  }

  ~Impl() { Stop(); }
  void Start() { worker_ = std::thread(&Impl::Run, this); }

  AvFrameSourceReadResult TryReadFrame() {
    AvFrameSourceReadResult result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || frames_.empty()) return result;
      const auto& front = frames_.front();
      if (!read_generation_ || *read_generation_ != front.generation) {
        prebuffering_ = true;
      }
      if (front.status == AvFrameSourceReadStatus::kFrame && prebuffering_) {
        std::size_t count = 0;
        bool boundary =
            producing_generation_ && front.generation < *producing_generation_;
        for (const auto& item : frames_) {
          if (item.generation != front.generation ||
              item.status == AvFrameSourceReadStatus::kEof) {
            boundary = true;
            break;
          }
          ++count;
        }
        if (!boundary && !error_ &&
            count < std::min(std::size_t{4}, capacity_)) {
          return result;
        }
      }
      result = std::move(frames_.front());
      frames_.pop_front();
      read_generation_ = result.generation;
      prebuffering_ = result.status == AvFrameSourceReadStatus::kEof;
      if (result.status == AvFrameSourceReadStatus::kFrame) --buffered_frames_;
    }
    changed_.notify_all();
    return result;
  }

  std::size_t frame_capacity() const { return capacity_; }
  std::size_t buffered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffered_frames_;
  }
  std::exception_ptr error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
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
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    buffered_frames_ = 0;
  }

 private:
  struct Track {
    Track(AvDecoder* value, AVMediaType type) : decoder(value) {
      if (!decoder) return;
      stream = decoder->stream_info();
      if (stream->codec_parameters.get()->codec_type != type) {
        throw std::invalid_argument("AvFrameSource Decoder 轨道类型不匹配");
      }
    }
    AvDecoder* decoder = nullptr;
    std::optional<ffmpeg::StreamInfo> stream;
    std::optional<AvDecoderReadResult> pending;
    std::unique_ptr<TrackFilter> filter;
    std::optional<ffmpeg::Frame> output;
    std::optional<ffmpeg::Frame> last;
    bool input_eof = false;
    bool output_eof = false;
  };

  bool Stopping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
  }

  bool Fetch(Track& track) {
    if (!track.decoder || track.pending) return false;
    if (const auto failure = track.decoder->error()) {
      std::rethrow_exception(failure);
    }
    if (!track.input_eof &&
        (track.output || (track.filter && !track.filter->wants_frame()))) {
      return false;
    }
    auto input = track.decoder->TryReadFrame();
    if (input.status == AvDecoderReadStatus::kNull) return false;
    track.pending = std::move(input);
    return true;
  }

  std::optional<std::uint64_t> NextGeneration() const {
    std::optional<std::uint64_t> next;
    for (const auto* track : {&video_, &audio_}) {
      if (track->pending &&
          (!generation_ || track->pending->generation > *generation_)) {
        const auto candidate = track->pending->generation;
        if (!next || candidate < *next) next = candidate;
      }
    }
    return next;
  }

  void ResetGeneration(std::uint64_t generation) {
    const bool initial = !generation_;
    // Keep audio cadence phase across a normally drained generation, even
    // though its frame number resets. A discontinuity starts fresh.
    if (initial || !generation_finished_) cadence_index_ = 0;
    generation_ = generation;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      producing_generation_ = generation;
    }
    changed_.notify_all();
    start_us_ = initial ? initial_start_us_ : AV_NOPTS_VALUE;
    origin_ready_ = start_us_ != AV_NOPTS_VALUE;
    generation_finished_ = false;
    frame_index_ = 0;
    last_end_us_ = AV_NOPTS_VALUE;
    for (auto* track : {&video_, &audio_}) {
      track->filter.reset();
      track->output.reset();
      track->last.reset();
      track->input_eof = !track->decoder;
      track->output_eof = !track->decoder;
      if (track->pending && track->pending->generation < generation)
        track->pending.reset();
    }
  }

  bool FindOrigin() {
    if (origin_ready_) return true;
    for (const auto* track : {&video_, &audio_}) {
      if (track->decoder &&
          (!track->pending || track->pending->generation != *generation_)) {
        return false;
      }
    }
    for (const auto* track : {&video_, &audio_}) {
      if (!track->pending || !track->pending->frame ||
          track->pending->frame->get()->pts == AV_NOPTS_VALUE)
        continue;
      const auto pts_us =
          av_rescale_q(track->pending->frame->get()->pts,
                       track->stream->time_base, AVRational{1, AV_TIME_BASE});
      if (start_us_ == AV_NOPTS_VALUE || pts_us < start_us_) start_us_ = pts_us;
    }
    origin_ready_ = true;
    return true;
  }

  bool Pump(Track& track) {
    if (!track.pending || track.pending->generation > *generation_)
      return false;
    if (track.pending->generation < *generation_) {
      track.pending.reset();
      return true;
    }
    auto input = std::move(*track.pending);
    track.pending.reset();
    if (input.status == AvDecoderReadStatus::kEof) {
      if (track.filter) track.filter->PushEof(input.end_pts);
      track.input_eof = true;
      if (!track.filter) track.output_eof = true;
    } else {
      if (!track.filter) {
        track.filter = std::make_unique<TrackFilter>(*track.stream, config_,
                                                     *input.frame, start_us_);
      }
      track.filter->PushFrame(*input.frame);
    }
    return true;
  }

  bool Pull(Track& track, int samples) {
    if (!track.filter || track.output || track.output_eof) return false;
    ffmpeg::Frame frame;
    const int result = track.filter->Read(frame, samples);
    if (result == AVERROR(EAGAIN)) return false;
    if (result == AVERROR_EOF) {
      track.output_eof = true;
    } else {
      Check(result, "读取 AvFrameSource 滤镜输出");
      track.output = std::move(frame);
    }
    return true;
  }

  int AudioSamples() const {
    if (!audio_.decoder) return 0;
    // Distribute fractional sample counts over five output periods.
    constexpr std::array<int, 5> k2997{1602, 1601, 1602, 1601, 1602};
    constexpr std::array<int, 5> k5994{801, 801, 801, 800, 801};
    const auto phase = static_cast<std::size_t>(cadence_index_ % 5);
    if (config_.audio_sample_rate == 48000) {
      if (av_cmp_q(config_.fps, AVRational{30000, 1001}) == 0)
        return k2997[phase];
      if (av_cmp_q(config_.fps, AVRational{60000, 1001}) == 0)
        return k5994[phase];
    }
    const auto tick = static_cast<std::int64_t>(cadence_index_);
    const AVRational sample_time_base{1, config_.audio_sample_rate};
    const auto begin =
        av_rescale_q(tick, av_inv_q(config_.fps), sample_time_base);
    const auto end =
        av_rescale_q(tick + 1, av_inv_q(config_.fps), sample_time_base);
    return static_cast<int>(end - begin);
  }

  bool Emit(AvFrameSourceReadResult result) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock,
                  [this] { return stopping_ || frames_.size() < capacity_; });
    if (stopping_) return false;
    const bool frame = result.status == AvFrameSourceReadStatus::kFrame;
    frames_.push_back(std::move(result));
    if (frame) ++buffered_frames_;
    return true;
  }

  bool Bundle() {
    if ((!video_.output && !video_.output_eof) ||
        (!audio_.output && !audio_.output_eof))
      return false;
    const bool new_video = video_.output.has_value();
    const bool new_audio = audio_.output.has_value();
    if (!new_video && !new_audio) return false;
    if (new_video) video_.last = std::move(video_.output);
    if (new_audio) audio_.last = std::move(audio_.output);
    video_.output.reset();
    audio_.output.reset();
    AvFrameSourceReadResult result;
    result.status = AvFrameSourceReadStatus::kFrame;
    result.video = video_.last;
    result.audio = audio_.last;
    result.generation = *generation_;
    result.frame_index = frame_index_;
    if (new_video) {
      const auto& video = *video_.last;
      result.pts_us = video->pts == AV_NOPTS_VALUE
                          ? AV_NOPTS_VALUE
                          : av_rescale_q(video->pts, video->time_base,
                                         AVRational{1, AV_TIME_BASE});
      result.duration_us =
          av_rescale_q(1, av_inv_q(config_.fps), AVRational{1, AV_TIME_BASE});
    }
    if (new_audio) {
      const auto& audio = *audio_.last;
      result.pts_us = audio->pts == AV_NOPTS_VALUE
                          ? AV_NOPTS_VALUE
                          : av_rescale_q(audio->pts, audio->time_base,
                                         AVRational{1, AV_TIME_BASE});
      result.duration_us =
          av_rescale_q(audio->nb_samples, AVRational{1, audio->sample_rate},
                       AVRational{1, AV_TIME_BASE});
    }
    last_end_us_ =
        result.pts_us == AV_NOPTS_VALUE || result.duration_us < 0 ||
                result.pts_us > std::numeric_limits<std::int64_t>::max() -
                                    result.duration_us
            ? AV_NOPTS_VALUE
            : result.pts_us + result.duration_us;
    if (!Emit(std::move(result))) return false;
    ++frame_index_;
    ++cadence_index_;
    return true;
  }

  void Run() noexcept {
    try {
      while (!Stopping()) {
        bool progress = Fetch(video_);
        progress |= Fetch(audio_);
        if (!generation_ || generation_finished_) {
          if (const auto next = NextGeneration()) {
            ResetGeneration(*next);
            progress = true;
          }
        }
        if (generation_ && !generation_finished_) {
          // A track that advances without its old EOF signals a discontinuity.
          // Rebuild both graphs together; never bundle different generations.
          std::optional<std::uint64_t> discontinuity;
          for (const auto* track : {&video_, &audio_}) {
            if (track->pending && !track->input_eof &&
                track->pending->generation > *generation_) {
              if (!discontinuity || track->pending->generation > *discontinuity)
                discontinuity = track->pending->generation;
            }
          }
          if (discontinuity) {
            ResetGeneration(*discontinuity);
            progress = true;
          }
          // Discard an old decoder cache prefix while the other track catches
          // up.
          for (auto* track : {&video_, &audio_}) {
            if (track->pending && track->pending->generation < *generation_) {
              track->pending.reset();
              progress = true;
            }
          }
          if (FindOrigin()) {
            progress |= Pump(video_);
            progress |= Pump(audio_);
            progress |= Pull(video_, 0);
            progress |= Pull(audio_, AudioSamples());
            if (video_.output_eof && audio_.output_eof) {
              if (!Emit({AvFrameSourceReadStatus::kEof,
                         {},
                         {},
                         *generation_,
                         frame_index_,
                         last_end_us_,
                         0}))
                return;
              generation_finished_ = true;
              progress = true;
            } else {
              progress |= Bundle();
            }
          }
        }
        if (!progress) {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait_for(lock, std::chrono::milliseconds(5),
                            [this] { return stopping_; });
        }
      }
    } catch (...) {
      const auto failure = std::current_exception();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = failure;
      }
      changed_.notify_all();
      MW_LOG_ERROR("streamer", "AvFrameSource 处理失败");
      if (on_failed_) {
        try {
          on_failed_(failure);
        } catch (const std::exception& observer_error) {
          MW_LOG_ERROR("streamer", "AvFrameSource 错误通知异常: {}",
                       observer_error.what());
        } catch (...) {
          MW_LOG_ERROR("streamer", "AvFrameSource 错误通知发生未知异常");
        }
      }
    }
  }

  AvEncoderConfig config_;
  const std::int64_t initial_start_us_;
  const OnFailed on_failed_;
  Track video_;
  Track audio_;
  ffmpeg::CodecParameters audio_parameters_;
  std::size_t capacity_ = 1;
  std::optional<std::uint64_t> generation_;
  std::int64_t start_us_ = AV_NOPTS_VALUE;
  bool origin_ready_ = false;
  bool generation_finished_ = false;
  std::uint64_t frame_index_ = 0;
  std::uint64_t cadence_index_ = 0;
  std::int64_t last_end_us_ = AV_NOPTS_VALUE;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  std::exception_ptr error_;
  std::deque<AvFrameSourceReadResult> frames_;
  std::size_t buffered_frames_ = 0;
  // Published by the worker, so a discontinued short cache can be consumed
  // while the next generation is still waiting for decodable video.
  std::optional<std::uint64_t> producing_generation_;
  std::optional<std::uint64_t> read_generation_;
  bool prebuffering_ = true;
  std::thread worker_;
};

AvFrameSource::AvFrameSource(AvDecoder* video, AvDecoder* audio,
                             AvEncoderConfig config, std::int64_t start_time_us,
                             OnFailed on_failed)
    : impl_(std::make_unique<Impl>(video, audio, std::move(config),
                                   start_time_us, std::move(on_failed))) {
  impl_->Start();
}
AvFrameSource::~AvFrameSource() = default;
AvFrameSourceReadResult AvFrameSource::TryReadFrame() {
  return impl_->TryReadFrame();
}
std::size_t AvFrameSource::frame_capacity() const {
  return impl_->frame_capacity();
}
std::size_t AvFrameSource::buffered_frames() const {
  return impl_->buffered_frames();
}
std::exception_ptr AvFrameSource::error() const { return impl_->error(); }
void AvFrameSource::RequestStop() noexcept { impl_->RequestStop(); }
void AvFrameSource::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
