#include "mw/streamer/recorder/recorder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/av_dictionary.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

using ffmpeg::FfmpegException;

void Validate(const std::vector<ffmpeg::StreamInfo>& streams,
              const RecorderConfig& config) {
  if (config.path.empty() || config.path.find('\0') != std::string::npos ||
      config.path.find("://") != std::string::npos ||
      std::filesystem::u8path(config.path).extension() != ".mp4" ||
      config.max_queue_bytes == 0) {
    throw std::invalid_argument("Recorder 需要本地 .mp4 路径和非零队列上限");
  }
  if (streams.empty() || streams.size() > 2)
    throw std::invalid_argument("Recorder 需要一至两条音视频轨道");
  bool video = false;
  bool audio = false;
  for (std::size_t i = 0; i < streams.size(); ++i) {
    streams[i].Validate();
    const auto* codec = streams[i].codec_parameters.get();
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO && !video &&
        (codec->codec_id == AV_CODEC_ID_H264 ||
         codec->codec_id == AV_CODEC_ID_HEVC)) {
      video = true;
    } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO && !audio &&
               codec->codec_id == AV_CODEC_ID_AAC) {
      audio = true;
    } else {
      throw std::invalid_argument(
          "Recorder 仅支持单路 H264/H265 视频和 AAC 音频");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (streams[i].stream_index == streams[j].stream_index)
        throw std::invalid_argument("Recorder 轨道索引重复");
    }
  }
}

std::size_t PacketBytes(const AVPacket& packet) {
  std::size_t bytes = sizeof(AVPacket) + static_cast<std::size_t>(packet.size);
  for (int i = 0; i < packet.side_data_elems; ++i)
    bytes += packet.side_data[i].size;
  return bytes;
}

struct FormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context->pb) avio_closep(&context->pb);
    avformat_free_context(context);
  }
};

struct BitstreamFilterDeleter {
  void operator()(AVBSFContext* context) const noexcept {
    av_bsf_free(&context);
  }
};

// Only the recorder worker touches the file context and its IO buffers.
class FileWriter final {
 public:
  FileWriter(const std::vector<ffmpeg::StreamInfo>& streams,
             const std::string& path)
      : streams_(streams) {
    AVFormatContext* raw = nullptr;
    const int result =
        avformat_alloc_output_context2(&raw, nullptr, "mp4", path.c_str());
    context_.reset(raw);
    FfmpegException::throwIfError(result, "创建录像 MP4 上下文");
    if (!context_) throw std::bad_alloc();
    for (const auto& info : streams_) {
      auto* stream = avformat_new_stream(context_.get(), nullptr);
      if (!stream) throw std::bad_alloc();
      FfmpegException::throwIfError(
          avcodec_parameters_copy(stream->codecpar,
                                  info.codec_parameters.get()),
          "复制录像轨道参数");
      stream->codecpar->codec_tag = 0;
      stream->time_base = info.time_base;
      stream->avg_frame_rate = info.frame_rate;
      stream->sample_aspect_ratio = info.sample_aspect_ratio;
    }
    // Force the local file protocol: even a colon in a filesystem name must
    // never select another FFmpeg protocol.
    const auto file_url = "file:" + path;
    FfmpegException::throwIfError(
        avio_open(&context_->pb, file_url.c_str(), AVIO_FLAG_WRITE),
        "打开录像文件");
    ffmpeg::AvDictionary options;
    // Delay moov until the first fragment so edit lists can represent initial
    // audio padding and video reorder delay.
    options.Set("movflags", "frag_keyframe+delay_moov+default_base_moof");
    // Also fragment audio-only recordings and long GOPs to avoid retaining an
    // entire recording in the muxer. This does not change frame timestamps.
    options.Set("frag_duration", "1000000");
    FfmpegException::throwIfError(
        avformat_write_header(context_.get(), options.address()),
        "写入录像文件头");
    CheckIo();
  }

  void Write(ffmpeg::Packet packet) {
    const auto found =
        std::find_if(streams_.begin(), streams_.end(), [&](const auto& info) {
          return info.stream_index == packet->stream_index;
        });
    const auto index = static_cast<std::size_t>(found - streams_.begin());
    const auto& info = streams_[index];
    // Match the MP4 muxer's detection: only ADTS needs aac_adtstoasc. Raw AAC
    // can be copied even when its input track has no AudioSpecificConfig.
    if (info.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC &&
        packet->size > 2 && (AV_RB16(packet->data) & 0xfff0) == 0xfff0) {
      if (!audio_filter_) {
        const auto* filter = av_bsf_get_by_name("aac_adtstoasc");
        if (!filter)
          throw FfmpegException(AVERROR_BSF_NOT_FOUND, "查找录像 AAC 过滤器");
        AVBSFContext* raw_filter = nullptr;
        FfmpegException::throwIfError(av_bsf_alloc(filter, &raw_filter),
                                      "创建录像 AAC 过滤器");
        audio_filter_.reset(raw_filter);
        audio_index_ = index;
        FfmpegException::throwIfError(
            avcodec_parameters_copy(audio_filter_->par_in,
                                    info.codec_parameters.get()),
            "复制录像 AAC 过滤器参数");
        audio_filter_->time_base_in = info.time_base;
        FfmpegException::throwIfError(av_bsf_init(audio_filter_.get()),
                                      "初始化录像 AAC 过滤器");
      }
      WriteAudio(packet.get());
    } else {
      WritePacket(std::move(packet), index);
    }
  }

  void Finish() {
    if (audio_filter_) WriteAudio(nullptr);
    FfmpegException::throwIfError(av_write_trailer(context_.get()),
                                  "写入录像文件尾");
    avio_flush(context_->pb);
    CheckIo();
    FfmpegException::throwIfError(avio_closep(&context_->pb), "关闭录像文件");
  }

 private:
  void WriteAudio(AVPacket* packet) {
    // Also handle HLS/TS packets that retain ADTS despite having global ASC.
    FfmpegException::throwIfError(
        av_bsf_send_packet(audio_filter_.get(), packet), "提交录像 AAC 编码包");
    for (;;) {
      ffmpeg::Packet filtered;
      const int result =
          av_bsf_receive_packet(audio_filter_.get(), filtered.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      FfmpegException::throwIfError(result, "读取录像 AAC 编码包");
      WritePacket(std::move(filtered), audio_index_);
    }
  }

  void WritePacket(ffmpeg::Packet packet, std::size_t index) {
    const auto& info = streams_[index];
    const auto input_base = info.time_base;
    if (!origin_) {
      origin_ = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      origin_base_ = input_base;
      // Padding is decoded but not presented. Keep it before presentation
      // zero, including for audio-only recording, so MP4 can retain the trim.
      const auto* codec = info.codec_parameters.get();
      std::size_t size = 0;
      const auto* skip = av_packet_get_side_data(
          packet.get(), AV_PKT_DATA_SKIP_SAMPLES, &size);
      if (skip && size >= 4 && codec->sample_rate > 0) {
        *origin_ +=
            av_rescale_q(AV_RL32(skip), {1, codec->sample_rate}, input_base);
      }
    }
    const auto offset = av_rescale_q(*origin_, origin_base_, input_base);
    if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset;
    av_packet_rescale_ts(packet.get(), input_base,
                         context_->streams[index]->time_base);
    packet->stream_index = static_cast<int>(index);
    packet->pos = -1;
    FfmpegException::throwIfError(
        av_interleaved_write_frame(context_.get(), packet.get()),
        "写入录像编码包");
    CheckIo();
  }

  void CheckIo() const {
    FfmpegException::throwIfError(context_->pb->error, "录像文件 IO");
  }

  const std::vector<ffmpeg::StreamInfo>& streams_;
  std::unique_ptr<AVFormatContext, FormatDeleter> context_;
  std::unique_ptr<AVBSFContext, BitstreamFilterDeleter> audio_filter_;
  std::size_t audio_index_ = 0;
  std::optional<std::int64_t> origin_;
  AVRational origin_base_{0, 1};
};

}  // namespace

class Recorder::Impl final {
 public:
  void SetOnError(OnError callback) {
    auto stored =
        callback ? std::make_shared<OnError>(std::move(callback)) : nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(stored);
  }

  bool Start(const std::vector<ffmpeg::StreamInfo>& streams,
             RecorderConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) return false;
    Validate(streams, config);
    streams_ = streams;
    config_ = std::move(config);
    status_ = {};
    status_.state = RecorderState::kRunning;
    last_warning_ = {};
    MW_LOG_DEBUG(
        "streamer",
        "Recorder 启动配置: instance={} path={} tracks={} max_queue_bytes={}",
        static_cast<const void*>(this), config_.path, streams_.size(),
        config_.max_queue_bytes);
    if (!worker_.Start([this] { Run(); })) {
      status_.state = RecorderState::kFailed;
      status_.error = "Recorder 无法创建写入线程";
      return false;
    }
    return true;
  }

  bool TrySubmitPacket(const ffmpeg::Packet& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (status_.state != RecorderState::kRunning) return false;
    const auto* raw = packet.get();
    if (!raw || raw->size <= 0 || !raw->data ||
        (raw->dts == AV_NOPTS_VALUE && raw->pts == AV_NOPTS_VALUE) ||
        std::none_of(streams_.begin(), streams_.end(), [&](const auto& info) {
          return info.stream_index == raw->stream_index;
        })) {
      throw std::invalid_argument("Recorder 编码包、时间戳或轨道索引无效");
    }
    const auto bytes = PacketBytes(*raw);
    if (bytes > config_.max_queue_bytes - status_.queue_bytes) {
      const auto dropped = ++status_.dropped_packets;
      const auto now = std::chrono::steady_clock::now();
      const bool warn =
          dropped == 1 || now - last_warning_ >= std::chrono::seconds(5);
      if (warn) last_warning_ = now;
      lock.unlock();
      if (warn)
        MW_LOG_WARNING("streamer",
                       "Recorder 队列已满，丢弃当前包: instance={} dropped={}",
                       static_cast<const void*>(this), dropped);
      return false;
    }
    queue_.push_back(packet.Ref());
    status_.queue_bytes += bytes;
    MW_LOG_TRACE("streamer",
                 "Recorder 投递包: instance={} stream={} pts={} dts={} "
                 "time_base={}/{} size={} queue={} queue_bytes={}",
                 static_cast<const void*>(this), raw->stream_index, raw->pts,
                 raw->dts, raw->time_base.num, raw->time_base.den, raw->size,
                 queue_.size(), status_.queue_bytes);
    lock.unlock();
    ready_.notify_one();
    return true;
  }

  RecorderStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  void Drain() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state == RecorderState::kRunning) {
        status_.state = RecorderState::kStopping;
        MW_LOG_DEBUG("streamer",
                     "Recorder 开始排空: instance={} queue={} queue_bytes={}",
                     static_cast<const void*>(this), queue_.size(),
                     status_.queue_bytes);
      }
    }
    ready_.notify_one();
  }

  void Stop() noexcept {
    Drain();
    worker_.Join();
  }

 private:
  void Run() noexcept {
    try {
      FileWriter writer(streams_, config_.path);
      MW_LOG_INFO("streamer", "Recorder 开始录像: instance={} path={}",
                  static_cast<const void*>(this), config_.path);
      for (;;) {
        std::optional<ffmpeg::Packet> packet;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          ready_.wait(lock, [this] {
            return !queue_.empty() || status_.state != RecorderState::kRunning;
          });
          if (queue_.empty()) break;
          status_.queue_bytes -= PacketBytes(*queue_.front().get());
          packet.emplace(std::move(queue_.front()));
          queue_.pop_front();
          MW_LOG_TRACE("streamer",
                       "Recorder 开始写包: instance={} stream={} pts={} dts={} "
                       "time_base={}/{} size={} queue={} queue_bytes={}",
                       static_cast<const void*>(this), (*packet)->stream_index,
                       (*packet)->pts, (*packet)->dts, (*packet)->time_base.num,
                       (*packet)->time_base.den, (*packet)->size, queue_.size(),
                       status_.queue_bytes);
        }
        writer.Write(std::move(*packet));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++status_.written_packets;
          MW_LOG_TRACE("streamer",
                       "Recorder 完成写包: instance={} written={} queue={} "
                       "queue_bytes={}",
                       static_cast<const void*>(this), status_.written_packets,
                       queue_.size(), status_.queue_bytes);
        }
      }
      writer.Finish();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = RecorderState::kStopped;
        MW_LOG_DEBUG("streamer",
                     "Recorder 排空完成: instance={} written={} dropped={} "
                     "queue_bytes={}",
                     static_cast<const void*>(this), status_.written_packets,
                     status_.dropped_packets, status_.queue_bytes);
      }
      MW_LOG_INFO("streamer", "Recorder 结束录像: instance={} path={}",
                  static_cast<const void*>(this), config_.path);
    } catch (const std::exception& error) {
      Fail(std::current_exception(), error.what());
    } catch (...) {
      Fail(std::current_exception(), "Recorder 未知写入错误");
    }
  }

  void Fail(std::exception_ptr exception, const char* error) noexcept {
    std::deque<ffmpeg::Packet> discarded;
    std::shared_ptr<OnError> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = RecorderState::kFailed;
      status_.error = error;
      status_.queue_bytes = 0;
      queue_.swap(discarded);
      MW_LOG_DEBUG(
          "streamer",
          "Recorder 失败清理: instance={} discarded={} written={} dropped={}",
          static_cast<const void*>(this), discarded.size(),
          status_.written_packets, status_.dropped_packets);
      callback = on_error_;
    }
    discarded.clear();
    MW_LOG_ERROR("streamer", "Recorder 录像失败: instance={} path={} error={}",
                 static_cast<const void*>(this), config_.path, error);
    if (callback) {
      try {
        (*callback)(std::move(exception));
      } catch (const std::exception& callback_error) {
        MW_LOG_WARNING("streamer", "Recorder OnError 回调异常: {}",
                       callback_error.what());
      } catch (...) {
        MW_LOG_WARNING("streamer", "Recorder OnError 回调发生未知异常");
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<ffmpeg::Packet> queue_;
  std::vector<ffmpeg::StreamInfo> streams_;
  RecorderConfig config_;
  RecorderStatus status_;
  std::shared_ptr<OnError> on_error_;
  std::chrono::steady_clock::time_point last_warning_{};
  internal::Thread worker_;
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { Stop(); }

void Recorder::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}

bool Recorder::Start(const std::vector<ffmpeg::StreamInfo>& streams,
                     RecorderConfig config) {
  return impl_->Start(streams, std::move(config));
}

bool Recorder::TrySubmitPacket(const ffmpeg::Packet& packet) {
  return impl_->TrySubmitPacket(packet);
}

RecorderStatus Recorder::status() const { return impl_->status(); }

void Recorder::Drain() noexcept { impl_->Drain(); }

void Recorder::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
