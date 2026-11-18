#include "streamer/clip/clip_output.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/recorder/recorder.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kMicroseconds{1, 1000000};

std::int64_t Microseconds(std::int64_t pts, AVRational time_base) {
  if (pts == AV_NOPTS_VALUE || time_base.num <= 0 || time_base.den <= 0)
    throw std::invalid_argument("剪辑时间戳或时间基无效");
  const auto result = av_rescale_q(pts, time_base, kMicroseconds);
  if (result == AV_NOPTS_VALUE ||
      result == std::numeric_limits<std::int64_t>::max())
    throw std::invalid_argument("剪辑时间戳超出范围");
  return result;
}

std::int64_t Add(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
    throw std::invalid_argument("剪辑时间范围溢出");
  return left + right;
}

std::int64_t Duration(std::chrono::milliseconds duration) {
  if (duration.count() < 0 ||
      duration.count() > std::numeric_limits<std::int64_t>::max() / 1000)
    throw std::invalid_argument("剪辑时长无效");
  return duration.count() * 1000;
}

bool RandomAccessNal(AVCodecID codec, const std::uint8_t* data,
                     std::size_t size) {
  if (codec == AV_CODEC_ID_H264) return size >= 1 && (data[0] & 0x1f) == 5;
  if (codec == AV_CODEC_ID_HEVC && size >= 2) {
    const auto type = (data[0] >> 1) & 0x3f;
    return type == 19 || type == 20 || type == 21;
  }
  return false;
}

bool IsRandomAccess(const ffmpeg::Packet& packet, AVCodecID codec,
                    int length_size) {
  if (!(packet->flags & AV_PKT_FLAG_KEY)) return false;
  const auto* data = packet->data;
  const auto size = static_cast<std::size_t>(packet->size);
  if (length_size != 0) {
    std::size_t offset = 0;
    bool valid = true;
    bool random_access = false;
    while (offset < size) {
      if (size - offset < static_cast<std::size_t>(length_size)) {
        valid = false;
        break;
      }
      std::uint32_t length = 0;
      for (int index = 0; index < length_size; ++index)
        length = (length << 8) | data[offset++];
      if (length == 0 || length > size - offset) {
        valid = false;
        break;
      }
      random_access |= RandomAccessNal(codec, data + offset, length);
      offset += length;
    }
    if (valid) return random_access;
  }
  // Only a leading start code identifies Annex B. A length prefix itself can
  // contain 00 00 01, so never infer representation by searching the payload.
  if (size < 4 || data[0] != 0 || data[1] != 0 ||
      !(data[2] == 1 || (data[2] == 0 && data[3] == 1)))
    return false;
  for (std::size_t offset = 0; offset + 3 < size; ++offset) {
    if (data[offset] != 0 || data[offset + 1] != 0) continue;
    std::size_t prefix = 0;
    if (data[offset + 2] == 1)
      prefix = 3;
    else if (data[offset + 2] == 0 && data[offset + 3] == 1)
      prefix = 4;
    if (!prefix) continue;
    if (RandomAccessNal(codec, data + offset + prefix, size - offset - prefix))
      return true;
    offset += prefix - 1;
  }
  return false;
}

std::size_t PacketBytes(const ffmpeg::Packet& packet) {
  auto bytes = sizeof(AVPacket) + static_cast<std::size_t>(packet->size);
  for (int index = 0; index < packet->side_data_elems; ++index)
    bytes += sizeof(AVPacketSideData) + packet->side_data[index].size;
  return bytes;
}

}  // namespace

class ClipOutput::Impl final {
 public:
  explicit Impl(ClipConfig config) : config_(config) {
    cache_duration_us_ = Duration(config.cache_duration);
    if (cache_duration_us_ == 0 || config.max_cache_bytes == 0)
      throw std::invalid_argument("剪辑缓存时长和字节上限必须为正数");
  }

  ~Impl() { Stop(); }

  void Start(const std::vector<ffmpeg::StreamInfo>& streams) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) throw std::logic_error("剪辑模块已经启动");
    video_index_ = audio_index_ = -1;
    nal_length_size_ = 0;
    for (const auto& stream : streams) {
      stream.Validate();
      const auto* codec = stream.codec_parameters.get();
      if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (video_index_ != -1 || (codec->codec_id != AV_CODEC_ID_H264 &&
                                   codec->codec_id != AV_CODEC_ID_HEVC))
          throw std::invalid_argument("剪辑仅支持一条 H264/H265 视频轨道");
        video_index_ = stream.stream_index;
        video_codec_ = codec->codec_id;
        video_time_base_ = stream.time_base;
        if (codec->extradata_size > 0 && codec->extradata[0] == 1) {
          const int offset = video_codec_ == AV_CODEC_ID_H264 ? 4 : 21;
          if (codec->extradata_size > offset)
            nal_length_size_ = (codec->extradata[offset] & 3) + 1;
        }
      } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (audio_index_ != -1 || codec->codec_id != AV_CODEC_ID_AAC)
          throw std::invalid_argument("剪辑仅支持一条 AAC 音频轨道");
        audio_index_ = stream.stream_index;
        audio_time_base_ = stream.time_base;
      } else {
        throw std::invalid_argument("剪辑包含不支持的轨道");
      }
    }
    if (video_index_ == -1) throw std::invalid_argument("事件剪辑需要视频轨道");
    ClearHistory();
    generation_ = 0;
    sequence_ = 0;
    streams_ = streams;
    running_ = true;
    stopping_ = false;
    try {
      retirement_worker_ = std::thread([this] { Retire(); });
    } catch (...) {
      running_ = false;
      throw;
    }
    MW_LOG_INFO("streamer.clip", "剪辑模块启动: cache_ms={} max_cache_bytes={}",
                config_.cache_duration.count(), config_.max_cache_bytes);
  }

  void Stop() noexcept {
    std::shared_ptr<Recorder> unfinished;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && !retirement_worker_.joinable()) return;
      running_ = false;
      stopping_ = true;
      try {
        Abort("直播停止，剪辑未完成");
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer.clip", "剪辑模块停止失败: {}", error.what());
        if (task_) {
          unfinished = std::move(task_->recorder);
          task_.reset();
        }
      } catch (...) {
        MW_LOG_ERROR("streamer.clip", "剪辑模块停止失败: 未知异常");
        if (task_) {
          unfinished = std::move(task_->recorder);
          task_.reset();
        }
      }
      ClearHistory();
    }
    retired_ready_.notify_one();
    if (retirement_worker_.joinable()) retirement_worker_.join();
    // Even allocation failure while queueing retirement must not leave a
    // joinable worker or destroy a Recorder under the module's mutex.
    if (unfinished) unfinished->Stop();
  }

  void SetGeneration(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == generation_) return;
    Abort("输入代次改变，丢弃旧剪辑");
    ClearHistory();
    generation_ = generation;
    MW_LOG_DEBUG("streamer.clip", "剪辑缓存切换代次: generation={}",
                 generation);
  }

  std::uint64_t generation() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
  }

  void MapVideo(std::uint64_t generation, const ffmpeg::Frame& source,
                const ffmpeg::Frame& output) {
    if (!source.get() || !output.get())
      throw std::invalid_argument("剪辑映射帧为空");
    const auto source_us = Microseconds(source->pts, source->time_base);
    const auto output_us = Microseconds(output->pts, output->time_base);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || generation != generation_) return;
    if (!first_output_us_) first_output_us_ = output_us;
    last_source_us_ = source_us;
    mappings_[source_us] = output_us;
    Resolve();
    TrimMappings();
  }

  void MapAudio(std::uint64_t generation, const ffmpeg::Frame& output) {
    if (!output.get()) throw std::invalid_argument("剪辑音频映射帧为空");
    const auto output_us = Microseconds(output->pts, output->time_base);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || generation != generation_) return;
    if (!first_audio_us_) first_audio_us_ = output_us;
  }

  void SubmitPacket(const ffmpeg::Packet& packet) {
    if (!packet.get() || packet->size <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    const bool video = packet->stream_index == video_index_;
    if (!video && packet->stream_index != audio_index_) return;
    const auto base = video ? video_time_base_ : audio_time_base_;
    const auto pts_us = Microseconds(packet->pts, base);
    const auto dts_us = Microseconds(packet->dts, base);
    if (packet->duration < 0) throw std::invalid_argument("剪辑编码包时长无效");
    const auto duration_us =
        av_rescale_q(packet->duration, base, kMicroseconds);
    const auto first = video ? first_output_us_ : first_audio_us_;
    if (!first || pts_us < *first) return;
    Cached packet_ref{
        packet.Ref(),
        pts_us,
        dts_us,
        duration_us,
        PacketBytes(packet),
        ++sequence_,
        video && IsRandomAccess(packet, video_codec_, nal_length_size_)};
    if (video) {
      if (videos_.empty() && !packet_ref.random_access) return;
      latest_video_us_ = std::max(latest_video_us_.value_or(pts_us), pts_us);
      cache_bytes_ += packet_ref.bytes;
      videos_.push_back(std::move(packet_ref));
      if (task_ && task_->recorder) WriteVideo(videos_.back());
    } else {
      latest_audio_us_ = std::max(latest_audio_us_.value_or(pts_us), pts_us);
      cache_bytes_ += packet_ref.bytes;
      audio_.push_back(std::move(packet_ref));
    }
    Resolve();
    if (task_ && task_->recorder) FlushAudio();
    FinishIfReady();
    Trim();
  }

  void SubmitClip(const ClipRequest& request) {
    const auto source_us =
        Microseconds(request.time.pts, request.time.time_base);
    const auto before_us = Duration(request.before);
    const auto after_us = Duration(request.after);
    if (request.event_id.empty() || request.path.empty() ||
        (before_us == 0 && after_us == 0))
      throw std::invalid_argument("剪辑事件 ID、路径或时间范围无效");
    if (before_us > cache_duration_us_)
      throw std::invalid_argument("剪辑前置时长超过缓存配置");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      MW_LOG_ERROR("streamer.clip", "拒绝剪辑: event={} 剪辑模块未运行",
                   request.event_id);
      return;
    }
    if (request.time.generation != generation_) {
      MW_LOG_ERROR("streamer.clip", "拒绝剪辑: event={} 输入代次已改变",
                   request.event_id);
      return;
    }
    if (task_) {
      MW_LOG_ERROR("streamer.clip", "拒绝剪辑: event={} 上一个任务尚未完成",
                   request.event_id);
      return;
    }
    task_.emplace();
    task_->request = request;
    task_->source_us = source_us;
    try {
      Resolve();
      if (task_ && task_->recorder) FlushAudio();
      FinishIfReady();
    } catch (...) {
      Abort("无法创建剪辑任务");
      throw;
    }
  }

  void Failed(const char* operation, const std::exception* error) noexcept {
    MW_LOG_ERROR("streamer.clip", "剪辑模块 {} 失败: {}", operation,
                 error ? error->what() : "未知异常");
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      Abort("剪辑处理失败");
    } catch (...) {
      // The enclosing API must never fail a live encoding callback.
    }
  }

 private:
  struct Cached {
    ffmpeg::Packet packet;
    std::int64_t pts_us;
    std::int64_t dts_us;
    std::int64_t duration_us;
    std::size_t bytes;
    std::uint64_t sequence;
    bool random_access;
  };
  struct Task {
    ClipRequest request;
    std::int64_t source_us = 0;
    std::optional<std::int64_t> event_us;
    std::int64_t start_us = 0;
    std::int64_t end_us = 0;
    std::optional<std::int64_t> cut_us;
    std::optional<std::int64_t> video_coverage_us;
    std::uint64_t last_audio_sequence = 0;
    std::shared_ptr<Recorder> recorder;
  };
  struct Retired {
    std::shared_ptr<Recorder> recorder;
    std::string event_id;
    std::string path;
    bool complete;
  };

  void ClearHistory() {
    videos_.clear();
    audio_.clear();
    mappings_.clear();
    cache_bytes_ = 0;
    first_output_us_.reset();
    first_audio_us_.reset();
    last_source_us_.reset();
    latest_video_us_.reset();
    latest_audio_us_.reset();
  }

  void Abort(const char* reason) {
    if (!task_) return;
    MW_LOG_ERROR("streamer.clip", "剪辑放弃: event={} path={} reason={}",
                 task_->request.event_id, task_->request.path, reason);
    RetireTask(false);
  }

  void RetireTask(bool complete) {
    if (task_->recorder) {
      task_->recorder->Drain();
      retired_.push_back({std::move(task_->recorder), task_->request.event_id,
                          task_->request.path, complete});
      retired_ready_.notify_one();
    }
    task_.reset();
  }

  void Resolve() {
    if (!task_ || task_->recorder) return;
    if (!task_->event_us) {
      const auto mapping = mappings_.find(task_->source_us);
      if (mapping == mappings_.end()) {
        if (last_source_us_ && task_->source_us < *last_source_us_)
          Abort("事件来源时间没有可用的输出映射");
        return;
      }
      task_->event_us = mapping->second;
      task_->start_us = Add(*task_->event_us, -Duration(task_->request.before));
      task_->end_us = Add(*task_->event_us, Duration(task_->request.after));
    }
    if (videos_.empty() || !latest_video_us_ ||
        *latest_video_us_ < task_->start_us)
      return;
    if (videos_.front().pts_us > task_->start_us) {
      Abort("事件前置历史不足或已被淘汰");
      return;
    }
    const Cached* start = nullptr;
    for (const auto& video : videos_) {
      if (video.random_access && video.pts_us <= task_->start_us)
        start = &video;
    }
    if (!start) return;
    task_->start_us = start->pts_us;
    auto recorder = std::make_shared<Recorder>();
    recorder->SetOnError([id = task_->request.event_id,
                          path =
                              task_->request.path](std::exception_ptr error) {
      try {
        if (error) std::rethrow_exception(error);
      } catch (const std::exception& failure) {
        MW_LOG_ERROR("streamer.clip", "剪辑写盘失败: event={} path={} error={}",
                     id, path, failure.what());
      } catch (...) {
        MW_LOG_ERROR("streamer.clip", "剪辑写盘失败: event={} path={}", id,
                     path);
      }
    });
    RecorderConfig config;
    config.path = task_->request.path;
    config.max_queue_bytes = config_.max_cache_bytes;
    if (!recorder->Start(streams_, std::move(config)))
      throw std::runtime_error("无法启动剪辑录像线程");
    task_->recorder = std::move(recorder);
    MW_LOG_INFO("streamer.clip", "剪辑开始: event={} path={}",
                task_->request.event_id, task_->request.path);
    // Video first establishes the shared presentation origin in Recorder.
    // Per-track order is untouched; Recorder/FFmpeg interleave the tracks.
    bool selected = false;
    for (const auto& video : videos_) {
      if (&video == start) selected = true;
      if (selected && task_) WriteVideo(video);
    }
  }

  void WriteVideo(const Cached& video) {
    if (!task_ || !task_->recorder || task_->cut_us) return;
    // CRA random access discards leading pictures with earlier presentation
    // times; retaining them would require references outside this clip.
    if (video.pts_us < task_->start_us) return;
    if (video.random_access && video.pts_us >= task_->end_us &&
        video.pts_us > task_->start_us && task_->video_coverage_us &&
        *task_->video_coverage_us >= task_->end_us) {
      task_->cut_us = video.pts_us;
      return;
    }
    if (!task_->recorder->TrySubmitPacket(video.packet)) {
      Abort("剪辑录像队列拒绝视频包");
      return;
    }
    // Open GOP leading pictures can follow the next CRA in decode order.
    // Do not stop before that CRA unless already submitted pictures cover the
    // requested end; otherwise retain its GOP rather than silently truncate.
    const auto coverage = Add(video.pts_us, video.duration_us);
    task_->video_coverage_us =
        std::max(task_->video_coverage_us.value_or(coverage), coverage);
  }

  void FlushAudio() {
    if (!task_ || !task_->recorder || audio_index_ == -1) return;
    // Before the next random access point determines the final boundary, never
    // release audio beyond the requested end. This also handles audio arriving
    // ahead of video.
    const auto safe_end = task_->cut_us.value_or(
        std::min(task_->end_us, latest_video_us_.value_or(task_->start_us)));
    for (const auto& audio : audio_) {
      if (audio.sequence <= task_->last_audio_sequence) continue;
      if (audio.pts_us >= safe_end) break;
      task_->last_audio_sequence = audio.sequence;
      if (audio.pts_us < task_->start_us) continue;
      if (!task_->recorder->TrySubmitPacket(audio.packet)) {
        Abort("剪辑录像队列拒绝音频包");
        return;
      }
    }
  }

  void FinishIfReady() {
    if (!task_ || !task_->recorder) return;
    const auto status = task_->recorder->status();
    if (status.state == RecorderState::kFailed || status.dropped_packets != 0) {
      Abort("剪辑录像失败");
      return;
    }
    if (!task_->cut_us) return;
    if (audio_index_ != -1 &&
        (!latest_audio_us_ || *latest_audio_us_ < *task_->cut_us))
      return;
    RetireTask(true);
  }

  void TrimMappings() {
    if (mappings_.empty()) return;
    const auto latest =
        std::max_element(mappings_.begin(), mappings_.end(),
                         [](const auto& left, const auto& right) {
                           return left.second < right.second;
                         });
    const auto cutoff = videos_.empty()
                            ? Add(latest->second, -cache_duration_us_)
                            : videos_.front().pts_us;
    for (auto it = mappings_.begin(); it != mappings_.end();) {
      if (it->second < cutoff)
        it = mappings_.erase(it);
      else
        ++it;
    }
  }

  void Trim() {
    while (!videos_.empty()) {
      const auto next =
          std::find_if(std::next(videos_.begin()), videos_.end(),
                       [](const Cached& item) { return item.random_access; });
      if (next == videos_.end()) break;
      if (cache_bytes_ <= config_.max_cache_bytes &&
          Add(next->pts_us, cache_duration_us_) >
              latest_video_us_.value_or(next->pts_us))
        break;
      const auto next_sequence = next->sequence;
      while (!videos_.empty() && videos_.front().sequence < next_sequence) {
        cache_bytes_ -= videos_.front().bytes;
        videos_.pop_front();
      }
    }
    if (!videos_.empty()) {
      while (!audio_.empty() &&
             audio_.front().pts_us < videos_.front().pts_us) {
        cache_bytes_ -= audio_.front().bytes;
        audio_.pop_front();
      }
    }
    if (cache_bytes_ > config_.max_cache_bytes) {
      MW_LOG_WARNING("streamer.clip",
                     "剪辑缓存达到字节上限，等待下一个随机访问点");
      videos_.clear();
      audio_.clear();
      cache_bytes_ = 0;
      Abort("剪辑缓存字节上限不足");
    }
    TrimMappings();
  }

  void Retire() noexcept {
    for (;;) {
      Retired recording;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        retired_ready_.wait(lock,
                            [&] { return stopping_ || !retired_.empty(); });
        if (retired_.empty()) return;
        recording = std::move(retired_.front());
        retired_.pop_front();
      }
      try {
        recording.recorder->Stop();
        const auto status = recording.recorder->status();
        if (status.state == RecorderState::kFailed ||
            status.dropped_packets != 0) {
          MW_LOG_ERROR("streamer.clip",
                       "剪辑保存失败: event={} path={} error={}",
                       recording.event_id, recording.path, status.error);
        } else if (recording.complete) {
          MW_LOG_INFO("streamer.clip", "剪辑完成: event={} path={}",
                      recording.event_id, recording.path);
        }
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer.clip", "剪辑收尾失败: event={} error={}",
                     recording.event_id, error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer.clip", "剪辑收尾失败: event={} 未知异常",
                     recording.event_id);
      }
    }
  }

  ClipConfig config_;
  std::int64_t cache_duration_us_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable retired_ready_;
  std::thread retirement_worker_;
  bool running_ = false;
  bool stopping_ = false;
  std::uint64_t generation_ = 0;
  std::vector<ffmpeg::StreamInfo> streams_;
  int video_index_ = -1;
  int audio_index_ = -1;
  AVCodecID video_codec_ = AV_CODEC_ID_NONE;
  int nal_length_size_ = 0;
  AVRational video_time_base_{0, 1};
  AVRational audio_time_base_{0, 1};
  std::optional<std::int64_t> first_output_us_;
  std::optional<std::int64_t> first_audio_us_;
  std::optional<std::int64_t> last_source_us_;
  std::optional<std::int64_t> latest_video_us_;
  std::optional<std::int64_t> latest_audio_us_;
  std::map<std::int64_t, std::int64_t> mappings_;
  std::deque<Cached> videos_;
  std::deque<Cached> audio_;
  std::size_t cache_bytes_ = 0;
  std::uint64_t sequence_ = 0;
  std::optional<Task> task_;
  std::deque<Retired> retired_;
};

ClipOutput::ClipOutput(ClipConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
ClipOutput::~ClipOutput() = default;
void ClipOutput::Start(const std::vector<ffmpeg::StreamInfo>& streams) {
  impl_->Start(streams);
}
void ClipOutput::Stop() noexcept { impl_->Stop(); }
std::uint64_t ClipOutput::generation() const noexcept {
  return impl_->generation();
}

// Callback failures are local to clipping, never a live pipeline failure.
void ClipOutput::SetGeneration(std::uint64_t generation) noexcept {
  try {
    impl_->SetGeneration(generation);
  } catch (const std::exception& error) {
    impl_->Failed("切换代次", &error);
  } catch (...) {
    impl_->Failed("切换代次", nullptr);
  }
}
void ClipOutput::MapVideo(std::uint64_t generation, const ffmpeg::Frame& source,
                          const ffmpeg::Frame& output) noexcept {
  try {
    impl_->MapVideo(generation, source, output);
  } catch (const std::exception& error) {
    impl_->Failed("时间映射", &error);
  } catch (...) {
    impl_->Failed("时间映射", nullptr);
  }
}
void ClipOutput::MapAudio(std::uint64_t generation,
                          const ffmpeg::Frame& output) noexcept {
  try {
    impl_->MapAudio(generation, output);
  } catch (const std::exception& error) {
    impl_->Failed("音频代次映射", &error);
  } catch (...) {
    impl_->Failed("音频代次映射", nullptr);
  }
}
void ClipOutput::SubmitPacket(const ffmpeg::Packet& packet) noexcept {
  try {
    impl_->SubmitPacket(packet);
  } catch (const std::exception& error) {
    impl_->Failed("缓存编码包", &error);
  } catch (...) {
    impl_->Failed("缓存编码包", nullptr);
  }
}
void ClipOutput::SubmitClip(const ClipRequest& request) noexcept {
  try {
    impl_->SubmitClip(request);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer.clip", "拒绝剪辑: event={} error={}",
                 request.event_id, error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer.clip", "拒绝剪辑: event={} 未知异常",
                 request.event_id);
  }
}

}  // namespace mw::streamer::internal
