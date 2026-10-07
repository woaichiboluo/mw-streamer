#include "mw/streamer/scheduler/scheduler.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <utility>

extern "C" {
#include <libswresample/swresample.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/input/ffmpeg_input_timing.h"
#include "mw/streamer/platform/platform.h"

namespace mw::streamer {
namespace {

constexpr AVRational kNanoseconds{1, 1000000000};
constexpr std::int64_t kTimestampJump = 2000000000;
constexpr std::size_t kMaxVideoFrames = 30;
constexpr std::int64_t kMaxAudioSamples = 1000 * 1024;

std::int64_t FramePts(const ffmpeg::Frame& frame) {
  return av_rescale_q(frame->pts, frame->time_base, kNanoseconds);
}

std::int64_t SamplesNs(std::int64_t samples, int rate) {
  return internal::AudioSamplesToNs(samples, rate);
}

std::int64_t SystemNs(std::chrono::steady_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             time.time_since_epoch())
      .count();
}

bool SameAudioFormat(const AVFrame& left, const AVFrame& right) {
  return left.format == right.format && left.sample_rate == right.sample_rate &&
         av_channel_layout_compare(&left.ch_layout, &right.ch_layout) == 0;
}
}  // namespace

Scheduler::Scheduler(SchedulerConfig config) noexcept : config_(config) {}

Scheduler::~Scheduler() { Stop(); }

void Scheduler::SetOnVideo(OnFrame callback) noexcept {
  on_video_ = std::move(callback);
}

void Scheduler::SetOnAudio(OnFrame callback) noexcept {
  on_audio_ = std::move(callback);
}

void Scheduler::SetOnEnded(OnEnded callback) noexcept {
  on_ended_ = std::move(callback);
}

bool Scheduler::Start(const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
  if (config_.video_frame_rate.num <= 0 || config_.video_frame_rate.den <= 0 ||
      config_.audio_sample_rate <= 0 || config_.audio_block_samples <= 0) {
    MW_LOG_ERROR("streamer", "调度配置无效");
    return false;
  }
  bool video = false;
  bool audio = false;
  for (const auto& stream : streams) {
    video |= stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO;
    audio |= stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return false;
    Clear();
    initialized_ = true;
    stopping_ = false;
    draining_ = false;
    ended_notified_ = false;
    video_done_ = !video;
    audio_done_ = !audio;
    performance_enabled_ =
        mw::log::ShouldLog("perf.scheduler", mw::log::LogLevel::kInfo);
    performance_video_ = video;
    performance_audio_ = audio;
    video_performance_ = {};
    audio_performance_ = {};
    if (performance_enabled_) {
      const auto now = Clock::now();
      video_performance_.window.Reset(now);
      audio_performance_.window.Reset(now);
    }
  }
  internal::SystemTimeBase();
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    started_workers_ = 0;
    stop_requested_.store(false);
  }
  try {
    if (video) video_thread_ = std::thread(&Scheduler::RunVideo, this);
    if (audio) audio_thread_ = std::thread(&Scheduler::RunAudio, this);
    // Establish both worker clocks before the input can submit its first frame.
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wake_.wait(lock, [this, video, audio] {
      return started_workers_ ==
                 static_cast<int>(video) + static_cast<int>(audio) ||
             stop_requested_.load();
    });
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "无法启动调度线程: {}", error.what());
    Stop();
    return false;
  }
  if (stop_requested_.load()) {
    Stop();
    return false;
  }
  MW_LOG_INFO("streamer",
              "Scheduler启动: video={}, fps={}/{}, audio={}, "
              "sample_rate={}, block_samples={}",
              video, config_.video_frame_rate.num, config_.video_frame_rate.den,
              audio, config_.audio_sample_rate, config_.audio_block_samples);
  return true;
}

std::int64_t Scheduler::OutputTime(Clock::time_point now) const noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             now - internal::SystemTimeBase())
      .count();
}

bool Scheduler::SubmitVideo(const ffmpeg::Frame& frame) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || stopping_ || draining_ || video_done_) return false;
    MW_LOG_TRACE("streamer", "Scheduler video input: pts={}, now={}",
                 FramePts(frame), SystemNs(Clock::now()));
    if (video_frames_.size() >= kMaxVideoFrames) {
      // Clear the video cache and reset selection when it reaches this limit.
      MW_LOG_WARNING("streamer", "Scheduler视频缓存达到30帧，清空并重新同步");
      if (performance_enabled_) {
        video_performance_.capacity_cleared += video_frames_.size();
        ++video_performance_.capacity_rejected;
      }
      video_frames_.clear();
      video_timing_set_ = false;
      return false;
    }
    video_frames_.push_back(frame.Ref());
    if (performance_enabled_) ++video_performance_.window.counters().packets;
  } catch (const std::exception& error) {
    if (performance_enabled_) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++video_performance_.window.counters().errors;
    }
    MW_LOG_ERROR("streamer", "Scheduler缓存视频失败: {}", error.what());
    return false;
  }
  Wake();
  return true;
}

ffmpeg::Frame Scheduler::ResampleAudio(const ffmpeg::Frame& frame) {
  const auto rate = config_.audio_sample_rate;
  const auto* source = frame.get();
  const auto pts = FramePts(frame);
  resample_offset_ = 0;
  if (source->format == AV_SAMPLE_FMT_FLTP && source->sample_rate == rate &&
      !resampler_) {
    return frame.Ref();
  }
  if (!resampler_ || resampler_format_ != source->format ||
      resampler_rate_ != source->sample_rate ||
      av_channel_layout_compare(&resampler_layout_, &source->ch_layout) != 0) {
    swr_free(&resampler_);
    av_channel_layout_uninit(&resampler_layout_);
    ffmpeg::FfmpegException::throwIfError(
        av_channel_layout_copy(&resampler_layout_, &source->ch_layout),
        "复制音频重采样声道布局");
    ffmpeg::FfmpegException::throwIfError(
        swr_alloc_set_opts2(&resampler_, &resampler_layout_, AV_SAMPLE_FMT_FLTP,
                            rate, &resampler_layout_,
                            static_cast<AVSampleFormat>(source->format),
                            source->sample_rate, 0, nullptr),
        "创建音频重采样器");
    ffmpeg::FfmpegException::throwIfError(swr_init(resampler_),
                                          "初始化音频重采样器");
    resampler_format_ = static_cast<AVSampleFormat>(source->format);
    resampler_rate_ = source->sample_rate;
  }
  const auto delay = swr_get_delay(resampler_, source->sample_rate);
  resample_offset_ = swr_get_delay(resampler_, 1000000000);
  ffmpeg::Frame result;
  result.CopyPropertiesFrom(frame);
  result->format = AV_SAMPLE_FMT_FLTP;
  result->sample_rate = rate;
  ffmpeg::FfmpegException::throwIfError(
      av_channel_layout_copy(&result->ch_layout, &source->ch_layout),
      "复制重采样输出声道布局");
  result->nb_samples = static_cast<int>(av_rescale_rnd(
      delay + source->nb_samples, rate, source->sample_rate, AV_ROUND_UP));
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(result.get(), 0),
                                        "分配重采样音频帧");
  const int samples =
      swr_convert(resampler_, result->extended_data, result->nb_samples,
                  const_cast<const std::uint8_t**>(source->extended_data),
                  source->nb_samples);
  ffmpeg::FfmpegException::throwIfError(samples, "重采样音频帧");
  result->nb_samples = samples;
  result->time_base = kNanoseconds;
  // Preserve the source media timestamp for SourceAudioTiming. Subtract the
  // resampling delay only when the PCM is placed in the system FIFO.
  result->pts = pts;
  result->duration = SamplesNs(samples, rate);
  resampler_next_pts_ = result->pts + result->duration;
  return result;
}

bool Scheduler::QueueAudio(ffmpeg::Frame frame, bool append_tail) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return false;
  const auto now = SystemNs(Clock::now());
  const auto rate = config_.audio_sample_rate;
  const auto mapping = audio_timing_.Map(FramePts(frame), frame->nb_samples,
                                         rate, now, resample_offset_);
  MW_LOG_TRACE("streamer",
               "Scheduler audio input: pts={}, samples={}, now={}, mapped={}, "
               "append={}, reset={}, head={}, queued={}",
               FramePts(frame), frame->nb_samples, now, mapping.timestamp_ns,
               mapping.append, mapping.reset_buffer, audio_timestamp_,
               audio_queued_samples_);
  if (mapping.reset_buffer) {
    MW_LOG_DEBUG("streamer", "Scheduler音频时间戳跳变，重置PCM缓存");
    ResetAudio(now);
  }
  auto placement = audio_queued_samples_;
  if ((!mapping.append && !append_tail) || !audio_timestamp_) {
    if (!audio_timestamp_ || mapping.timestamp_ns < audio_timestamp_) {
      ResetAudio(mapping.timestamp_ns);
      // Reset the predicted system timestamp to the new FIFO origin after
      // replacing the buffer.
      audio_timing_.ResetBufferTimestamp(mapping.timestamp_ns);
    }
    placement = internal::AudioNsToSamples(
        mapping.timestamp_ns - audio_timestamp_, rate);
  }
  const auto end = placement + frame->nb_samples;
  if (end > kMaxAudioSamples) {
    if (performance_enabled_) {
      ++audio_performance_.capacity_rejected;
      audio_performance_.capacity_rejected_samples += frame->nb_samples;
    }
    MW_LOG_WARNING("streamer", "Scheduler音频缓存超过样本上限，丢弃本次输入");
    return false;
  }
  // Replace the previous tail at the mapped position. Gaps between segments
  // are delivered as silence.
  const auto position = audio_sample_cursor_ + placement;
  while (!audio_frames_.empty() &&
         audio_frames_.back().start_sample >= position) {
    if (performance_enabled_) {
      audio_performance_.dropped_source_samples += audio_frames_.back().samples;
    }
    audio_frames_.pop_back();
  }
  if (!audio_frames_.empty()) {
    auto& tail = audio_frames_.back();
    const auto retained = static_cast<int>(
        std::min<std::int64_t>(tail.samples, position - tail.start_sample));
    if (performance_enabled_) {
      audio_performance_.dropped_source_samples += tail.samples - retained;
    }
    tail.samples = retained;
  }
  audio_output_template_ = frame.Ref();
  const int samples = frame->nb_samples;
  audio_frames_.push_back({std::move(frame), position, 0, samples});
  audio_queued_samples_ = end;
  audio_last_size_ = 0;
  Wake();
  return true;
}

bool Scheduler::SubmitAudio(const ffmpeg::Frame& frame) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || stopping_ || draining_ || audio_done_) return false;
    ++audio_calls_;
  }
  bool accepted = false;
  try {
    auto normalized = ResampleAudio(frame);
    if (normalized->nb_samples > 0) {
      accepted = QueueAudio(std::move(normalized));
    } else {
      accepted = true;
    }
  } catch (const std::exception& error) {
    if (performance_enabled_) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++audio_performance_.window.counters().errors;
    }
    MW_LOG_ERROR("streamer", "Scheduler音频处理失败: {}", error.what());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --audio_calls_;
    if (performance_enabled_ && accepted) {
      ++audio_performance_.window.counters().packets;
    }
  }
  idle_.notify_all();
  Wake();
  return accepted;
}

void Scheduler::FlushResampler() {
  if (!resampler_) return;
  resample_offset_ = 0;
  const auto rate = config_.audio_sample_rate;
  for (;;) {
    const int delay = static_cast<int>(swr_get_delay(resampler_, rate));
    if (delay <= 0) return;
    ffmpeg::Frame frame;
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = rate;
    frame->nb_samples = delay;
    ffmpeg::FfmpegException::throwIfError(
        av_channel_layout_copy(&frame->ch_layout, &resampler_layout_),
        "复制重采样尾帧声道布局");
    ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                          "分配重采样尾帧");
    const int samples =
        swr_convert(resampler_, frame->extended_data, delay, nullptr, 0);
    ffmpeg::FfmpegException::throwIfError(samples, "排空音频重采样器");
    if (samples == 0) return;
    frame->nb_samples = samples;
    frame->time_base = kNanoseconds;
    frame->pts = resampler_next_pts_;
    frame->duration = SamplesNs(samples, rate);
    resampler_next_pts_ += frame->duration;
    // This is the continuation still held inside SWR, rather than a new
    // timestamped source packet. Append its samples physically so the floor
    // ns-to-sample conversion cannot overwrite the preceding final sample.
    QueueAudio(std::move(frame), true);
  }
}

void Scheduler::Drain() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || stopping_ || draining_) return;
    draining_ = true;
    ++audio_calls_;
  }
  try {
    FlushResampler();
  } catch (const std::exception& error) {
    if (performance_enabled_) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++audio_performance_.window.counters().errors;
    }
    MW_LOG_ERROR("streamer", "Scheduler排空音频失败: {}", error.what());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --audio_calls_;
  }
  idle_.notify_all();
  Wake();
  NotifyEnded();
  MW_LOG_DEBUG("streamer", "Scheduler开始排空");
}

bool Scheduler::TickVideo(Clock::time_point now) {
  std::optional<ffmpeg::Frame> selected;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    MW_LOG_TRACE("streamer",
                 "Scheduler video tick: now={}, previous={}, media={}, "
                 "set={}, queued={}, first={}",
                 SystemNs(now), SystemNs(video_system_), video_media_,
                 video_timing_set_, video_frames_.size(),
                 video_frames_.empty() ? AV_NOPTS_VALUE
                                       : FramePts(video_frames_.front()));
    if (!video_timing_set_ && video_frames_.empty()) {
      video_system_ = now;
      return !draining_;
    }
    if (!video_frames_.empty()) {
      auto pts = static_cast<std::uint64_t>(FramePts(video_frames_.front()));
      const auto difference =
          pts > video_media_ ? pts - video_media_ : video_media_ - pts;
      bool ready = !video_timing_set_ || !video_media_;
      if (ready || difference > kTimestampJump) {
        video_media_ = pts;
        video_timing_set_ = true;
        ready = true;
      } else {
        auto frame_offset = pts - video_media_;
        video_media_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now -
                                                                 video_system_)
                .count());
        ready = video_media_ > pts;
        if (ready) {
          while (video_frames_.size() > 1) {
            const auto next =
                static_cast<std::uint64_t>(FramePts(video_frames_[1]));
            // An unsigned delta also identifies backward discontinuities.
            if (next - pts > kTimestampJump) {
              video_media_ = next - frame_offset;
            }
            frame_offset = next - video_media_;
            if (video_media_ <= next || video_media_ - next < 2000000) break;
            if (performance_enabled_) ++video_performance_.late_old;
            video_frames_.pop_front();
            pts = next;
          }
        }
      }
      if (ready) {
        selected.emplace(std::move(video_frames_.front()));
        video_frames_.pop_front();
        if (performance_enabled_) ++video_performance_.selected_new;
        const auto duration =
            av_rescale_q(selected->get()->duration, selected->get()->time_base,
                         kNanoseconds);
        video_end_system_ =
            now +
            std::chrono::nanoseconds(std::max<std::int64_t>(
                duration, av_rescale_q(1, av_inv_q(config_.video_frame_rate),
                                       kNanoseconds)));
      }
    }
    // Advance the system clock every tick, but preserve the media position
    // when the cache is empty. Advancing media here would catch video up across
    // an HLS stall while buffered audio still contains older media.
    video_system_ = now;
    if (!selected && draining_ && video_frames_.empty() &&
        (!video_output_ || now >= video_end_system_)) {
      return false;
    }
  }
  if (selected) {
    MW_LOG_TRACE("streamer", "Scheduler video selected: now={}, pts={}",
                 SystemNs(now), FramePts(*selected));
    auto scheduled = selected->Ref();
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    // Update audio's source-to-output mapping from the chosen video frame.
    audio_timing_.SetVideoTiming(FramePts(scheduled), SystemNs(now));
    MW_LOG_TRACE("streamer", "Scheduler video mapping: pts={}, tick={}",
                 FramePts(scheduled), SystemNs(now));
    video_output_ = std::move(scheduled);
  }
  std::optional<ffmpeg::Frame> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    if (video_output_) result.emplace(video_output_->Ref());
  }
  if (result && !stop_requested_.load()) {
    (*result)->pts = OutputTime(now);
    (*result)->time_base = kNanoseconds;
    (*result)->duration =
        av_rescale_q(1, av_inv_q(config_.video_frame_rate), kNanoseconds);
    (*result)->pkt_dts = AV_NOPTS_VALUE;
    DeliverVideo(*result);
    if (performance_enabled_ && !selected) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++video_performance_.repeated;
    }
  }
  return true;
}

void Scheduler::ResetAudio(std::int64_t timestamp) noexcept {
  if (performance_enabled_ && initialized_ && !stopping_) {
    for (const auto& entry : audio_frames_) {
      audio_performance_.dropped_source_samples += entry.samples;
    }
  }
  audio_frames_.clear();
  audio_timestamp_ = timestamp;
  audio_sample_cursor_ = 0;
  audio_queued_samples_ = 0;
  audio_last_size_ = 0;
}

void Scheduler::PopAudio(std::int64_t samples, bool discarded) noexcept {
  samples = std::min(samples, audio_queued_samples_);
  audio_sample_cursor_ += samples;
  audio_queued_samples_ -= samples;
  while (!audio_frames_.empty()) {
    auto& entry = audio_frames_.front();
    const auto skipped = std::min<std::int64_t>(
        entry.samples,
        std::max<std::int64_t>(0, audio_sample_cursor_ - entry.start_sample));
    entry.start_sample += skipped;
    entry.offset += static_cast<int>(skipped);
    entry.samples -= static_cast<int>(skipped);
    if (performance_enabled_) {
      if (discarded) {
        audio_performance_.dropped_source_samples += skipped;
        audio_performance_.late_source_samples += skipped;
      } else {
        audio_performance_.consumed_source_samples += skipped;
      }
    }
    if (entry.samples) break;
    audio_frames_.pop_front();
  }
}

bool Scheduler::DiscardStoppedAudio() noexcept {
  if (!audio_queued_samples_) return false;
  if (audio_last_size_ == audio_queued_samples_) {
    if (audio_pending_stop_) {
      ResetAudio(0);
      audio_pending_stop_ = false;
      return true;
    }
    audio_pending_stop_ = true;
  }
  audio_last_size_ = audio_queued_samples_;
  return false;
}

bool Scheduler::TickAudio(Clock::time_point start, Clock::time_point end) {
  std::vector<ffmpeg::Frame> results;
  std::vector<std::int64_t> source_samples;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    const bool finite_tail = draining_ && audio_calls_ == 0;
    if (!audio_queued_samples_ && finite_tail) return false;
    const auto rate = config_.audio_sample_rate;
    const auto block = config_.audio_block_samples;
    audio_output_timing_.Push(SystemNs(start), SystemNs(end));
    auto window = audio_output_timing_.window();
    // Normal output requires a complete block. Drain also permits the last
    // partial block once no more input can arrive.
    bool pending =
        !audio_timestamp_ || (!finite_tail && audio_queued_samples_ < block);
    if (audio_output_timing_.maxed() && audio_timestamp_ &&
        audio_timestamp_ < window.start) {
      const auto count = internal::AudioNsToSamples(
                             window.start - audio_timestamp_ - 1, rate) +
                         1;
      const auto discarded = std::min(count, audio_queued_samples_);
      PopAudio(discarded, true);
      audio_last_size_ = 0;
      audio_timestamp_ += SamplesNs(discarded, rate);
      if (audio_timestamp_ == window.start - 1) audio_timestamp_ = window.start;
      if (audio_timestamp_ < window.start) {
        audio_timestamp_ = 0;
        audio_timing_.ClearTiming();
        pending = true;
      } else {
        pending = !finite_tail && audio_queued_samples_ < block;
      }
      MW_LOG_TRACE("streamer", "Scheduler音频缓冲达上限，追赶丢弃{}个样本",
                   discarded);
    }
    if (!pending && audio_timestamp_ < window.start) {
      const auto previous = audio_output_timing_.total_buffering_ticks();
      audio_output_timing_.BufferTo(audio_timestamp_, rate, block);
      window = audio_output_timing_.window();
      const auto added =
          audio_output_timing_.total_buffering_ticks() - previous;
      if (added) {
        MW_LOG_DEBUG("streamer", "Scheduler音频增加缓冲{}块，累计{}块", added,
                     audio_output_timing_.total_buffering_ticks());
      }
    }
    int offset = 0;
    if (audio_timestamp_ > window.start) {
      offset = static_cast<int>(std::min<std::int64_t>(
          block,
          internal::AudioNsToSamples(audio_timestamp_ - window.start, rate)));
    }
    const bool inside = audio_timestamp_ >= window.start - 1 &&
                        audio_timestamp_ < window.end && offset < block;
    int needed = block - offset;
    if (!finite_tail && inside && audio_queued_samples_ < needed)
      pending = true;
    const bool mixing = !audio_output_timing_.waiting_ticks() && !pending &&
                        inside && audio_timestamp_ >= window.start;
    if (performance_enabled_ && pending) {
      ++audio_performance_.underload_ticks;
    }
    MW_LOG_TRACE("streamer",
                 "Scheduler audio tick: start={}, end={}, head={}, queued={}, "
                 "pending={}, mixing={}, waiting={}, buffering={}",
                 window.start, window.end, audio_timestamp_,
                 audio_queued_samples_, pending, mixing,
                 audio_output_timing_.waiting_ticks(),
                 audio_output_timing_.total_buffering_ticks());
    if (audio_output_template_) {
      if (mixing && finite_tail) {
        needed = static_cast<int>(
            std::min<std::int64_t>(needed, audio_queued_samples_));
      }
      const int samples = mixing ? offset + needed : block;
      const auto& prototype = audio_frames_.empty()
                                  ? *audio_output_template_
                                  : audio_frames_.front().frame;
      // Keep input channel layouts separate within the same output window.
      // Splitting the window must not advance the FIFO head by more time than
      // the samples actually consumed.
      std::vector<std::pair<int, const ffmpeg::Frame*>> slices{{0, &prototype}};
      if (mixing) {
        for (const auto& entry : audio_frames_) {
          const auto position = entry.start_sample - audio_sample_cursor_;
          if (position >= needed) break;
          if (!SameAudioFormat(*entry.frame.get(),
                               *slices.back().second->get())) {
            slices.emplace_back(offset + static_cast<int>(position),
                                &entry.frame);
          }
        }
      }
      for (std::size_t i = 0; i < slices.size(); ++i) {
        const auto begin = slices[i].first;
        const auto finish =
            i + 1 == slices.size() ? samples : slices[i + 1].first;
        if (begin == finish) continue;
        const auto& source = *slices[i].second;
        ffmpeg::Frame result;
        std::int64_t copied_samples = 0;
        result.CopyPropertiesFrom(source);
        result->format = source->format;
        result->sample_rate = source->sample_rate;
        ffmpeg::FfmpegException::throwIfError(
            av_channel_layout_copy(&result->ch_layout, &source->ch_layout),
            "复制音频输出声道布局");
        result->nb_samples = finish - begin;
        ffmpeg::FfmpegException::throwIfError(
            av_frame_get_buffer(result.get(), 0), "分配音频输出帧");
        const auto format = static_cast<AVSampleFormat>(result->format);
        const auto channels = result->ch_layout.nb_channels;
        ffmpeg::FfmpegException::throwIfError(
            av_samples_set_silence(result->extended_data, 0, result->nb_samples,
                                   channels, format),
            "填充音频静音");
        if (mixing) {
          for (const auto& entry : audio_frames_) {
            const auto position =
                offset + entry.start_sample - audio_sample_cursor_;
            if (position >= finish) break;
            const auto copy_start = std::max<std::int64_t>(position, begin);
            const auto copy_end =
                std::min<std::int64_t>(position + entry.samples, finish);
            if (copy_end <= copy_start) continue;
            ffmpeg::FfmpegException::throwIfError(
                av_samples_copy(
                    result->extended_data, entry.frame->extended_data,
                    static_cast<int>(copy_start - begin),
                    entry.offset + static_cast<int>(copy_start - position),
                    static_cast<int>(copy_end - copy_start), channels, format),
                "复制音频输出样本");
            if (performance_enabled_) copied_samples += copy_end - copy_start;
          }
        }
        result->pts = window.start - SystemNs(internal::SystemTimeBase()) +
                      SamplesNs(begin, rate);
        result->time_base = kNanoseconds;
        result->duration = SamplesNs(result->nb_samples, rate);
        result->pkt_dts = AV_NOPTS_VALUE;
        results.push_back(std::move(result));
        if (performance_enabled_) source_samples.push_back(copied_samples);
      }
    }
    // Consume against the chosen historical window even during waiting.
    // Insufficient data advances the timestamp without popping samples;
    // DiscardStoppedAudio clears residual data that remains unchanged.
    if (audio_timestamp_ && audio_timestamp_ < window.end) {
      if (audio_timestamp_ < window.start - 1) {
        if (pending && audio_queued_samples_ < block && !finite_tail) {
          DiscardStoppedAudio();
        }
      } else if (inside) {
        if (audio_queued_samples_ < needed) {
          if (!DiscardStoppedAudio()) audio_timestamp_ = window.end;
        } else {
          PopAudio(needed);
          audio_last_size_ = 0;
          audio_pending_stop_ = false;
          audio_timestamp_ = window.end;
        }
      }
    }
    if (!audio_output_timing_.Finish()) results.clear();
  }
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (stop_requested_.load()) break;
    const auto& result = results[index];
    DeliverAudio(result);
    if (performance_enabled_) {
      std::lock_guard<std::mutex> lock(mutex_);
      audio_performance_.delivered_source_samples += source_samples[index];
      if (source_samples[index]) ++audio_performance_.selected_new;
      audio_performance_.zero_fill_samples +=
          result->nb_samples - source_samples[index];
    }
  }
  return true;
}

void Scheduler::FinishTrack(bool video) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (video)
      video_done_ = true;
    else
      audio_done_ = true;
  }
  NotifyEnded();
}

void Scheduler::NotifyEnded() noexcept {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_ && draining_ && !stopping_ && video_done_ && audio_done_ &&
        audio_calls_ == 0 && !ended_notified_) {
      ended_notified_ = true;
      ++callback_calls_;
      notify = true;
    }
  }
  if (notify) {
    if (!stop_requested_.load()) {
      if (performance_enabled_) {
        std::lock_guard<std::mutex> lock(mutex_);
        LogPerformanceLocked(true, true);
        LogPerformanceLocked(false, true);
      }
      MW_LOG_INFO("streamer", "Scheduler排空完成");
      if (on_ended_) on_ended_();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --callback_calls_;
    }
    idle_.notify_all();
  }
}

void Scheduler::Clear() noexcept {
  video_frames_.clear();
  video_output_.reset();
  audio_frames_.clear();
  audio_output_template_.reset();
  video_timing_set_ = false;
  video_end_system_ = {};
  audio_timing_.Reset();
  audio_output_timing_.Reset();
  ResetAudio(0);
  audio_pending_stop_ = false;
  swr_free(&resampler_);
  av_channel_layout_uninit(&resampler_layout_);
  resampler_format_ = AV_SAMPLE_FMT_NONE;
  resampler_rate_ = 0;
  resampler_next_pts_ = 0;
  resample_offset_ = 0;
}

void Scheduler::Stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;
    stopping_ = true;
  }
  RequestStop();
  Join();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock,
               [this] { return audio_calls_ == 0 && callback_calls_ == 0; });
    if (performance_enabled_) {
      LogPerformanceLocked(true, true);
      LogPerformanceLocked(false, true);
    }
    Clear();
    initialized_ = false;
  }
  MW_LOG_INFO("streamer", "Scheduler停止完成");
}

void Scheduler::RequestStop() noexcept {
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    stop_requested_.store(true);
  }
  Wake();
}

void Scheduler::Join() noexcept {
  if (video_thread_.joinable()) video_thread_.join();
  if (audio_thread_.joinable()) audio_thread_.join();
}

void Scheduler::Wake() noexcept { wake_.notify_all(); }

bool Scheduler::WaitUntil(Clock::time_point deadline, bool precise) noexcept {
  return internal::WaitUntil(wake_, wait_mutex_, stop_requested_, deadline,
                             precise);
}

void Scheduler::WorkerStarted() noexcept {
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    ++started_workers_;
  }
  Wake();
}

void Scheduler::RunVideo() noexcept {
  MW_LOG_DEBUG("streamer", "视频输出线程启动");
  const auto started = Clock::now();
  // Run the initial empty tick before exposing a ready scheduler. Otherwise a
  // delayed worker can render newly submitted media against a tick from before
  // the audio worker's window origin and incorrectly discard its first PCM.
  try {
    const auto tick_started =
        performance_enabled_ ? Clock::now() : Clock::time_point{};
    TickVideo(started);
    if (performance_enabled_) RecordTick(true, tick_started, started, 0);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "调度视频线程初始化失败: {}", error.what());
    RequestStop();
    WorkerStarted();
    FinishTrack(true);
    return;
  }
  WorkerStarted();
  std::int64_t tick = 1;
  const auto period = av_inv_q(config_.video_frame_rate);
  const auto interval =
      av_rescale_q_rnd(1, period, kNanoseconds, AV_ROUND_DOWN);
  while (!stop_requested_.load()) {
    const auto deadline = started + std::chrono::nanoseconds(tick * interval);
    if (!WaitUntil(deadline, true)) break;
    const auto tick_started =
        performance_enabled_ ? Clock::now() : Clock::time_point{};
    try {
      const bool running = TickVideo(deadline);
      if (performance_enabled_)
        RecordTick(true, tick_started, deadline, interval);
      if (!running) break;
    } catch (const std::exception& error) {
      if (performance_enabled_) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++video_performance_.window.counters().errors;
        }
        RecordTick(true, tick_started, deadline, interval);
      }
      MW_LOG_ERROR("streamer", "视频输出失败: {}", error.what());
      RequestStop();
      break;
    }
    ++tick;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - started)
                             .count();
    // Skip missed output ticks instead of replaying old video output.
    const auto next = elapsed / interval;
    if (next > tick) {
      if (performance_enabled_) {
        std::lock_guard<std::mutex> lock(mutex_);
        video_performance_.skipped_ticks += next - tick;
      }
      tick = next;
    }
  }
  FinishTrack(true);
  MW_LOG_DEBUG("streamer", "视频输出线程退出");
}

void Scheduler::RunAudio() noexcept {
  MW_LOG_DEBUG("streamer", "音频输出线程启动");
  const internal::ScopedAudioScheduling audio_scheduling;
  const auto started = Clock::now();
  WorkerStarted();
  std::int64_t samples = config_.audio_block_samples;
  auto start = started;
  while (!stop_requested_.load()) {
    const auto deadline =
        started + std::chrono::nanoseconds(internal::AudioSamplesToNs(
                      samples, config_.audio_sample_rate));
    if (!WaitUntil(deadline)) break;
    const auto tick_started =
        performance_enabled_ ? Clock::now() : Clock::time_point{};
    try {
      const bool running = TickAudio(start, deadline);
      if (performance_enabled_) {
        RecordTick(
            false, tick_started, deadline,
            SamplesNs(config_.audio_block_samples, config_.audio_sample_rate));
      }
      if (!running) break;
    } catch (const std::exception& error) {
      if (performance_enabled_) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++audio_performance_.window.counters().errors;
        }
        RecordTick(
            false, tick_started, deadline,
            SamplesNs(config_.audio_block_samples, config_.audio_sample_rate));
      }
      MW_LOG_ERROR("streamer", "音频输出失败: {}", error.what());
      RequestStop();
      break;
    }
    samples += config_.audio_block_samples;
    start = deadline;
  }
  FinishTrack(false);
  MW_LOG_DEBUG("streamer", "音频输出线程退出");
}

void Scheduler::DeliverVideo(const ffmpeg::Frame& frame) noexcept {
  const auto started =
      performance_enabled_ && on_video_ ? Clock::now() : Clock::time_point{};
  if (on_video_) on_video_(frame);
  if (performance_enabled_) {
    const auto elapsed =
        on_video_ ? SystemNs(Clock::now()) - SystemNs(started) : 0;
    std::lock_guard<std::mutex> lock(mutex_);
    ++video_performance_.window.counters().frames;
    if (on_video_) {
      ++video_performance_.external_callback_calls;
      video_performance_.external_callback_ns += elapsed;
      video_performance_.max_external_callback_ns =
          std::max(video_performance_.max_external_callback_ns, elapsed);
    }
  }
}

void Scheduler::DeliverAudio(const ffmpeg::Frame& frame) noexcept {
  const auto started =
      performance_enabled_ && on_audio_ ? Clock::now() : Clock::time_point{};
  if (on_audio_) on_audio_(frame);
  if (performance_enabled_) {
    const auto elapsed =
        on_audio_ ? SystemNs(Clock::now()) - SystemNs(started) : 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& counters = audio_performance_.window.counters();
    ++counters.frames;
    counters.samples += frame->nb_samples;
    if (on_audio_) {
      ++audio_performance_.external_callback_calls;
      audio_performance_.external_callback_ns += elapsed;
      audio_performance_.max_external_callback_ns =
          std::max(audio_performance_.max_external_callback_ns, elapsed);
    }
  }
}

void Scheduler::RecordTick(bool video, Clock::time_point started,
                           Clock::time_point deadline,
                           std::int64_t interval_ns) noexcept {
  const auto finished = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  auto& performance = video ? video_performance_ : audio_performance_;
  performance.window.AddWork(SystemNs(finished) - SystemNs(started));
  // Normal wakeup jitter is not counted as an entire missed output tick.
  if (interval_ns > 0 &&
      SystemNs(started) - SystemNs(deadline) >= interval_ns) {
    ++performance.late_ticks;
  }
  LogPerformanceLocked(video);
}

void Scheduler::LogPerformanceLocked(bool video, bool final) noexcept {
  if (!performance_enabled_ ||
      (video ? !performance_video_ : !performance_audio_)) {
    return;
  }
  auto& performance = video ? video_performance_ : audio_performance_;
  internal::PerformanceReport report;
  if (!performance.window.Sample(report, final, Clock::now())) return;
  const auto& total = report.total;
  const auto current_fps = internal::PerformanceReport::Rate(
      static_cast<double>(report.interval.frames), report.interval_seconds);
  const auto source_fps = internal::PerformanceReport::Rate(
      static_cast<double>(report.interval.packets), report.interval_seconds);
  const auto callback_mean_ms = performance.external_callback_calls
                                    ? performance.external_callback_ns / 1e6 /
                                          performance.external_callback_calls
                                    : 0;
  const auto tick_mean_ms =
      total.work_calls ? total.work_ns / 1e6 / total.work_calls : 0;
  const auto& overview = final ? total : report.interval;
  const auto overview_seconds =
      final ? report.elapsed_seconds : report.interval_seconds;
  if (video) {
    MW_LOG_INFO("perf.scheduler",
                "Scheduler[{}] track=video report={} fps={:.1f}/{:.1f} "
                "queue_frames={} repeated_total={} dropped_total={} "
                "callback_mean_ms={:.3f}",
                static_cast<const void*>(this), final ? "summary" : "interval",
                internal::PerformanceReport::Rate(
                    static_cast<double>(overview.frames), overview_seconds),
                static_cast<double>(config_.video_frame_rate.num) /
                    config_.video_frame_rate.den,
                video_frames_.size(), performance.repeated,
                performance.late_old + performance.capacity_cleared +
                    performance.capacity_rejected,
                callback_mean_ms);
    MW_LOG_TRACE(
        "perf.scheduler",
        "Scheduler[{}] track=video final={} counts=since_start "
        "elapsed_s={:.3f} window_s={:.3f} "
        "target_fps={:.3f} source_fps={:.3f} current_fps={:.3f} "
        "accepted_new={} deliver_frames={} selected_new={} repeated={} "
        "dropped_late_old={} dropped_capacity_clear={} "
        "capacity_rejected_new={} "
        "late_ticks={} skipped_ticks={} late_tick_threshold_ms={:.3f} "
        "queue_frames={} "
        "external_callback_calls={} external_callback_mean_ms={:.3f} "
        "external_callback_max_ms={:.3f} "
        "tick_calls={} tick_include_callback_mean_ms={:.3f} "
        "tick_include_callback_max_ms={:.3f} errors={}",
        static_cast<const void*>(this), final, report.elapsed_seconds,
        report.interval_seconds,
        static_cast<double>(config_.video_frame_rate.num) /
            config_.video_frame_rate.den,
        source_fps, current_fps, total.packets, total.frames,
        performance.selected_new, performance.repeated, performance.late_old,
        performance.capacity_cleared, performance.capacity_rejected,
        performance.late_ticks, performance.skipped_ticks,
        av_rescale_q_rnd(1, av_inv_q(config_.video_frame_rate), kNanoseconds,
                         AV_ROUND_DOWN) /
            1e6,
        video_frames_.size(), performance.external_callback_calls,
        callback_mean_ms, performance.max_external_callback_ns / 1e6,
        total.work_calls, tick_mean_ms, total.max_work_ns / 1e6, total.errors);
  } else {
    MW_LOG_INFO(
        "perf.scheduler",
        "Scheduler[{}] track=audio report={} samples_per_second={:.0f}/{} "
        "queue_ms={:.1f} silence_ms_total={:.1f} dropped_ms_total={:.1f} "
        "callback_mean_ms={:.3f}",
        static_cast<const void*>(this), final ? "summary" : "interval",
        internal::PerformanceReport::Rate(static_cast<double>(overview.samples),
                                          overview_seconds),
        config_.audio_sample_rate,
        static_cast<double>(audio_queued_samples_) * 1000 /
            config_.audio_sample_rate,
        static_cast<double>(performance.zero_fill_samples) * 1000 /
            config_.audio_sample_rate,
        static_cast<double>(performance.dropped_source_samples) * 1000 /
            config_.audio_sample_rate,
        callback_mean_ms);
    MW_LOG_TRACE(
        "perf.scheduler",
        "Scheduler[{}] track=audio final={} counts=since_start "
        "elapsed_s={:.3f} window_s={:.3f} "
        "target_tick_fps={:.3f} sample_rate={} block_samples={} "
        "source_fps={:.3f} current_fps={:.3f} "
        "accepted_new={} deliver_frames={} selected_new={} "
        "selected_unit=source_payload_frames repeated=0 "
        "delivered_samples={} consumed_source_samples={} "
        "delivered_source_samples={} "
        "zero_fill_samples={} underload_ticks={} "
        "dropped_queued_source_samples={} "
        "dropped_late_source_samples={} capacity_rejected_new={} "
        "capacity_rejected_samples={} "
        "late_ticks={} late_tick_threshold_ms={:.3f} queue_samples={} "
        "queue_ms={:.3f} "
        "external_callback_calls={} external_callback_mean_ms={:.3f} "
        "external_callback_max_ms={:.3f} "
        "tick_calls={} tick_include_callback_mean_ms={:.3f} "
        "tick_include_callback_max_ms={:.3f} errors={}",
        static_cast<const void*>(this), final, report.elapsed_seconds,
        report.interval_seconds,
        static_cast<double>(config_.audio_sample_rate) /
            config_.audio_block_samples,
        config_.audio_sample_rate, config_.audio_block_samples, source_fps,
        current_fps, total.packets, total.frames, performance.selected_new,
        total.samples, performance.consumed_source_samples,
        performance.delivered_source_samples, performance.zero_fill_samples,
        performance.underload_ticks, performance.dropped_source_samples,
        performance.late_source_samples, performance.capacity_rejected,
        performance.capacity_rejected_samples, performance.late_ticks,
        SamplesNs(config_.audio_block_samples, config_.audio_sample_rate) / 1e6,
        audio_queued_samples_,
        static_cast<double>(audio_queued_samples_) * 1000 /
            config_.audio_sample_rate,
        performance.external_callback_calls, callback_mean_ms,
        performance.max_external_callback_ns / 1e6, total.work_calls,
        tick_mean_ms, total.max_work_ns / 1e6, total.errors);
  }
}

}  // namespace mw::streamer
