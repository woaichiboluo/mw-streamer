#include "mw/streamer/input/ffmpeg_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/decoder.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/input/ffmpeg_input_timing.h"

namespace mw::streamer {
namespace internal {

std::chrono::steady_clock::time_point SystemTimeBase() noexcept {
  static const auto epoch = std::chrono::steady_clock::now();
  return epoch;
}

}  // namespace internal
namespace {

using Clock = std::chrono::steady_clock;

const char* ModeName(InputMode mode) {
  switch (mode) {
    case InputMode::kLive:
      return "live";
    case InputMode::kRemux:
      return "remux";
    case InputMode::kBatch:
      return "batch";
  }
  return "unknown";
}

const char* StateName(InputState state) noexcept {
  switch (state) {
    case InputState::kIdle:
      return "kIdle";
    case InputState::kConnecting:
      return "kConnecting";
    case InputState::kConnected:
      return "kConnected";
    case InputState::kWaitingRetry:
      return "kWaitingRetry";
    case InputState::kEnded:
      return "kEnded";
    case InputState::kFailed:
      return "kFailed";
    case InputState::kStopped:
      return "kStopped";
  }
  return "unknown";
}

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

template <typename Callback, typename... Args>
void InvokeCallback(const Callback& callback, Args&&... args) noexcept {
  if (!callback) {
    return;
  }
  try {
    callback(std::forward<Args>(args)...);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "FFmpeg Input回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "FFmpeg Input回调抛出了未知异常");
  }
}

}  // namespace

// One pending frame per track lets Input schedule interleaved audio/video.
// Packet queues and timestamp synthesis belong to this delivery layer.
struct FfmpegInput::Track {
  Track(std::unique_ptr<ffmpeg::Decoder> decoder,
        const ffmpeg::StreamInfo& stream, AVRational frame_rate,
        bool wait_for_keyframe, std::int64_t last_duration_ns)
      : decoder_(std::move(decoder)),
        timing_(stream.time_base, frame_rate,
                stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO,
                last_duration_ns),
        stream_index_(stream.stream_index),
        wait_for_keyframe_(wait_for_keyframe &&
                           stream.codec_parameters.get()->codec_type ==
                               AVMEDIA_TYPE_VIDEO) {}

  void Push(ffmpeg::Packet packet) {
    queued_bytes_ += static_cast<std::size_t>(packet->size);
    packets_.push_back(std::move(packet));
  }

  void Prepare(bool input_eof, FfmpegInput& input) {
    if (ready_ || ended_) {
      return;
    }
    for (;;) {
      const auto result = decoder_->ReceiveFrame(frame_);
      if (result == ffmpeg::DecodeResult::kFrame) {
        timing_.Update(*frame_.get());
        ready_ = true;
        return;
      }
      if (result == ffmpeg::DecodeResult::kEnd) {
        MW_LOG_TRACE("streamer", "Input轨道解码结束: stream={}", stream_index_);
        ended_ = true;
        return;
      }
      if (!packets_.empty()) {
        // Receive returned kNeedInput, so Send cannot also return EAGAIN.
        input.SubmitPacket(packets_.front(), decoder_.get());
        queued_bytes_ -= static_cast<std::size_t>(packets_.front()->size);
        packets_.pop_front();
      } else if (input_eof && !drain_sent_) {
        MW_LOG_TRACE("streamer", "Input轨道开始排空解码器: stream={}",
                     stream_index_);
        if (!decoder_->Drain()) {
          throw ffmpeg::FfmpegException(AVERROR_BUG, "解码器拒绝EOF输入");
        }
        drain_sent_ = true;
      } else {
        return;
      }
    }
  }

  bool ShouldDeliver() {
    if (wait_for_keyframe_) {
      if (!(frame_->flags & AV_FRAME_FLAG_KEY)) {
        return false;
      }
      wait_for_keyframe_ = false;
      MW_LOG_DEBUG("streamer", "Input轨道收到首个关键帧: stream={}, pts_ns={}",
                   stream_index_, timing_.pts_ns());
    }
    return true;
  }

  const ffmpeg::Frame& OutputFrame(const internal::PlaybackClock& playback,
                                   InputMode mode) {
    if (mode == InputMode::kLive) {
      frame_->pts = playback.Timestamp(timing_.pts_ns());
      frame_->duration = playback.ScaleTime(timing_.duration_ns());
      if (frame_->nb_samples > 0) {
        const auto rate = av_rescale_q(frame_->sample_rate, playback.speed(),
                                       AVRational{1, 1});
        if (rate <= 0 || rate > std::numeric_limits<int>::max()) {
          throw std::invalid_argument("倍速音频采样率超出有效范围");
        }
        frame_->sample_rate = static_cast<int>(rate);
      }
    } else {
      frame_->pts = timing_.pts_ns();
      frame_->duration = timing_.duration_ns();
    }
    frame_->time_base = {1, 1000000000};
    frame_->pkt_dts = AV_NOPTS_VALUE;
    return frame_;
  }

  void Consume() noexcept {
    ready_ = false;
    frame_.Unref();
  }

  void Flush() noexcept {
    MW_LOG_TRACE("streamer",
                 "Input轨道清空解码器和Packet缓存: stream={}, bytes={}",
                 stream_index_, queued_bytes_);
    decoder_->Flush();
    frame_.Unref();
    packets_.clear();
    timing_.Flush();
    queued_bytes_ = 0;
    ready_ = false;
    ended_ = false;
    drain_sent_ = false;
  }

  bool ready() const noexcept { return ready_; }
  bool ended() const noexcept { return ended_; }
  int stream_index() const noexcept { return stream_index_; }
  std::int64_t pts_ns() const noexcept { return timing_.pts_ns(); }
  std::int64_t next_pts_ns() const noexcept { return timing_.next_pts_ns(); }
  std::int64_t duration_ns() const noexcept { return timing_.duration_ns(); }
  std::size_t queued_bytes() const noexcept { return queued_bytes_; }

 private:
  std::unique_ptr<ffmpeg::Decoder> decoder_;
  ffmpeg::Frame frame_;
  std::deque<ffmpeg::Packet> packets_;
  internal::FrameTiming timing_;
  int stream_index_;
  bool wait_for_keyframe_;
  bool ready_ = false;
  bool ended_ = false;
  bool drain_sent_ = false;
  std::size_t queued_bytes_ = 0;
};

FfmpegInput::FfmpegInput(FfmpegInputConfig config)
    : config_(std::move(config)) {
  if ((config_.mode != InputMode::kLive && config_.mode != InputMode::kRemux &&
       config_.mode != InputMode::kBatch) ||
      config_.max_retries < -1 || config_.retry_interval.count() <= 0 ||
      config_.open_timeout.count() <= 0 || config_.read_timeout.count() <= 0 ||
      config_.max_packet_buffer_bytes == 0 ||
      !std::isfinite(config_.playback_speed) || config_.playback_speed < 0.5 ||
      config_.playback_speed > 8.0) {
    MW_LOG_ERROR(
        "streamer",
        "Input配置无效: mode={}, max_retries={}, retry_ms={}, "
        "open_timeout_ms={}, "
        "read_timeout_ms={}, packet_buffer_bytes={}, playback_speed={}",
        static_cast<int>(config_.mode), config_.max_retries,
        config_.retry_interval.count(), config_.open_timeout.count(),
        config_.read_timeout.count(), config_.max_packet_buffer_bytes,
        config_.playback_speed);
    throw std::invalid_argument("FFmpeg Input配置无效");
  }
}

FfmpegInput::FfmpegInput(const ffmpeg::HwDeviceContext& video_device,
                         FfmpegInputConfig config)
    : FfmpegInput(std::move(config)) {
  video_device_.emplace(video_device);
}

FfmpegInput::~FfmpegInput() { StopWorkers(false); }

void FfmpegInput::SetOnReady(OnReady callback) {
  on_ready_ = std::move(callback);
}

void FfmpegInput::SetOnPacket(OnPacket callback) {
  on_packet_ = std::move(callback);
}

void FfmpegInput::SetOnFrame(OnFrame callback) {
  on_frame_ = std::move(callback);
}

void FfmpegInput::SetOnStateChanged(OnStateChanged callback) {
  on_state_changed_ = std::move(callback);
}

void FfmpegInput::Start(std::string_view url) {
  if (url.empty() || std::all_of(url.begin(), url.end(), [](unsigned char ch) {
        return std::isspace(ch);
      })) {
    MW_LOG_ERROR("streamer", "Input[{}]启动失败: URL为空",
                 static_cast<const void*>(this));
    throw std::invalid_argument("Input URL不能为空");
  }
  std::lock_guard<std::mutex> lock(control_mutex_);
  const auto current = state();
  if (current == InputState::kConnecting || current == InputState::kConnected ||
      current == InputState::kWaitingRetry) {
    MW_LOG_ERROR("streamer", "Input[{}]重复启动: state={}",
                 static_cast<const void*>(this), StateName(current));
    throw std::logic_error("Input已经启动");
  }
  JoinWorkers();
  performance_.Start(this);
  ready_streams_.clear();
  generation_ = 0;
  url_ = url;
  const char* protocol = avio_find_protocol_name(url_.c_str());
  network_source_ = protocol && std::strcmp(protocol, "file") != 0;
  {
    std::lock_guard<std::mutex> wait_lock(wait_mutex_);
    seek_position_.reset();
    stop_requested_.store(false);
  }
  retries_ = 0;
  state_.store(InputState::kConnecting);
  MW_LOG_INFO("streamer",
              "Input[{}]开始启动: protocol={}, mode={}, playback_speed={}, "
              "video_device={}, loop={}, "
              "auto_reconnect={}, max_retries={}",
              static_cast<const void*>(this), protocol ? protocol : "unknown",
              ModeName(config_.mode), config_.playback_speed,
              video_device_ && video_device_->get() ? "cuda" : "cpu",
              config_.loop, config_.auto_reconnect, config_.max_retries);
  try {
    std::lock_guard<std::mutex> thread_lock(thread_mutex_);
    media_thread_ = std::thread(&FfmpegInput::RunMedia, this);
  } catch (...) {
    state_.store(current);
    ReportPerformance(true);
    MW_LOG_ERROR("streamer", "Input[{}]启动失败: 无法创建媒体线程",
                 static_cast<const void*>(this));
    throw;
  }
}

void FfmpegInput::Stop() { StopWorkers(true); }

void FfmpegInput::Seek(std::chrono::milliseconds position) {
  if (position.count() < 0) {
    MW_LOG_ERROR("streamer", "Input[{}]Seek位置无效: position_ms={}",
                 static_cast<const void*>(this), position.count());
    throw std::invalid_argument("Seek位置不能为负数");
  }
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    const auto current = state();
    if (stop_requested_.load() || (current != InputState::kConnecting &&
                                   current != InputState::kConnected)) {
      MW_LOG_DEBUG(
          "streamer", "Input[{}]忽略Seek请求: state={}, position_ms={}",
          static_cast<const void*>(this), StateName(current), position.count());
      return;
    }
    if (network_source_.load()) {
      MW_LOG_ERROR("streamer", "Input[{}]Seek失败: 网络输入不支持Seek",
                   static_cast<const void*>(this));
      throw std::invalid_argument("Seek仅支持本地文件");
    }
    seek_position_ = position;
    MW_LOG_DEBUG("streamer", "Input[{}]提交Seek请求: position_ms={}",
                 static_cast<const void*>(this), position.count());
  }
  wake_.notify_all();
}

void FfmpegInput::StopWorkers(bool notify) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  const auto current = state();
  const bool already_stopped = current == InputState::kStopped;
  MW_LOG_DEBUG("streamer", "Input[{}]请求停止: state={}, notify={}",
               static_cast<const void*>(this), StateName(current), notify);
  {
    std::lock_guard<std::mutex> wait_lock(wait_mutex_);
    stop_requested_.store(true);
    seek_position_.reset();
  }
  wake_.notify_all();
  JoinWorkers();
  state_.store(InputState::kStopped);
  ReportPerformance(true);
  if (!already_stopped && (notify || current != InputState::kIdle)) {
    MW_LOG_INFO("streamer", "Input[{}]停止完成: generation={}",
                static_cast<const void*>(this), generation_);
  }
  if (notify && !already_stopped) {
    InvokeCallback(on_state_changed_, InputState::kStopped, 0,
                   std::string_view{});
  }
}

void FfmpegInput::JoinWorkers() {
  std::thread media;
  std::thread reconnect;
  {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    media = std::move(media_thread_);
    reconnect = std::move(reconnect_thread_);
  }
  // Reconnect may already own the previous media worker's thread handle.
  // Joining it also waits for that worker. Never join under thread_mutex_.
  if (reconnect.joinable()) {
    reconnect.join();
  }
  if (media.joinable()) {
    media.join();
  }
}

InputState FfmpegInput::state() const noexcept { return state_.load(); }

void FfmpegInput::NotifyState(InputState state, int error,
                              std::string_view message) {
  if (!stop_requested_.load()) {
    state_.store(state);
    MW_LOG_DEBUG("streamer", "Input[{}]状态通知: {}, error={}",
                 static_cast<const void*>(this), StateName(state), error);
    if (state == InputState::kConnected) {
      MW_LOG_INFO("streamer", "Input[{}]已连接: generation={}, tracks={}",
                  static_cast<const void*>(this), generation_, streams_.size());
    } else if (state == InputState::kEnded) {
      MW_LOG_INFO("streamer", "Input[{}]读取结束: generation={}",
                  static_cast<const void*>(this), generation_);
    } else if (state == InputState::kWaitingRetry) {
      MW_LOG_WARNING(
          "streamer",
          "Input[{}]等待重连: retry={}, max_retries={}, delay_ms={}, "
          "error={}, reason={}",
          static_cast<const void*>(this), retries_, config_.max_retries,
          config_.retry_interval.count(), error, message);
    }
    InvokeCallback(on_state_changed_, state, error, message);
  }
}

int FfmpegInput::InterruptCallback(void* opaque) {
  const auto* input = static_cast<const FfmpegInput*>(opaque);
  const auto deadline = input->io_deadline_ns_.load();
  return input->stop_requested_.load() ||
         (deadline != 0 && NowNs() >= deadline);
}

void FfmpegInput::SetIoDeadline(std::chrono::milliseconds timeout) {
  io_deadline_ns_.store(
      NowNs() +
      std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
}

void FfmpegInput::OpenAttempt() {
  performance_.SegmentReset();
  MW_LOG_DEBUG(
      "streamer", "Input[{}]开始打开输入: retry={}, open_timeout_ms={}",
      static_cast<const void*>(this), retries_, config_.open_timeout.count());
  format_ = avformat_alloc_context();
  if (!format_) {
    throw std::bad_alloc();
  }
  // Installed before open and probing, so Stop can interrupt both phases.
  format_->interrupt_callback = {&FfmpegInput::InterruptCallback, this};
  SetIoDeadline(config_.open_timeout);
  AVDictionary* options = nullptr;
  const char* protocol = avio_find_protocol_name(url_.c_str());
  // Servers can filter HEVC out unless the RTMP connect advertises hvc1.
  int result = 0;
  if (protocol && std::strncmp(protocol, "rtmp", 4) == 0) {
    result = av_dict_set(&options, "rtmp_enhanced_codecs", "hvc1", 0);
  }
  if (result >= 0) {
    result = avformat_open_input(&format_, url_.c_str(), nullptr, &options);
  }
  av_dict_free(&options);
  ffmpeg::FfmpegException::throwIfError(result, "avformat_open_input");
  MW_LOG_DEBUG("streamer", "Input[{}]输入已打开，开始探测轨道",
               static_cast<const void*>(this));
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(format_, nullptr), "avformat_find_stream_info");
  io_deadline_ns_.store(0);
  MW_LOG_DEBUG("streamer", "Input[{}]轨道探测完成: streams={}",
               static_cast<const void*>(this), format_->nb_streams);

  // Select one video and one audio stream from the input.
  int video_index = -1;
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    const int related = type == AVMEDIA_TYPE_AUDIO ? video_index : -1;
    const int index =
        av_find_best_stream(format_, type, -1, related, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) {
      continue;
    }
    ffmpeg::FfmpegException::throwIfError(index, "av_find_best_stream");
    if (type == AVMEDIA_TYPE_VIDEO) {
      video_index = index;
    }
    auto* stream = format_->streams[index];
    ffmpeg::StreamInfo info{stream->index,
                            ffmpeg::CodecParameters(*stream->codecpar),
                            stream->time_base};
    info.Validate();
    MW_LOG_DEBUG("streamer",
                 "Input[{}]选择轨道: stream={}, type={}, codec={}, format={}, "
                 "time_base={}/{}, size={}x{}, fps={}/{}, sample_rate={}, "
                 "channels={}",
                 static_cast<const void*>(this), info.stream_index,
                 av_get_media_type_string(type),
                 avcodec_get_name(stream->codecpar->codec_id),
                 stream->codecpar->format, info.time_base.num,
                 info.time_base.den, stream->codecpar->width,
                 stream->codecpar->height, stream->codecpar->framerate.num,
                 stream->codecpar->framerate.den, stream->codecpar->sample_rate,
                 stream->codecpar->ch_layout.nb_channels);
    streams_.push_back(std::move(info));
  }
  if (!ready_streams_.empty() && ready_streams_ != streams_) {
    throw std::runtime_error("Input轨道配置已改变，停止读取");
  }
  if (streams_.empty()) {
    throw ffmpeg::FfmpegException(AVERROR_STREAM_NOT_FOUND,
                                  "Input没有音视频轨道");
  }
  // Compare the full selected stream set before opening any new decoder.
  if (config_.mode != InputMode::kRemux) {
    for (const auto& info : streams_) {
      auto* stream = format_->streams[info.stream_index];
      const auto type = info.codec_parameters.get()->codec_type;
      const auto& decoder_name = type == AVMEDIA_TYPE_VIDEO
                                     ? config_.video_decoder_name
                                     : config_.audio_decoder_name;
      MW_LOG_DEBUG("streamer",
                   "Input[{}]创建解码器: stream={}, type={}, name={}",
                   static_cast<const void*>(this), info.stream_index,
                   av_get_media_type_string(type),
                   decoder_name.empty() ? "auto" : decoder_name.c_str());
      std::unique_ptr<ffmpeg::Decoder> decoder;
      if (type == AVMEDIA_TYPE_VIDEO) {
        if (video_device_) {
          decoder = std::make_unique<ffmpeg::VideoDecoder>(
              info, *video_device_, config_.video_decoder_name);
        } else {
          decoder = std::make_unique<ffmpeg::VideoDecoder>(
              info, config_.video_decoder_name);
        }
      } else {
        decoder = std::make_unique<ffmpeg::AudioDecoder>(
            info, config_.audio_decoder_name);
      }
      tracks_.push_back(std::make_unique<Track>(
          std::move(decoder), info,
          av_guess_frame_rate(format_, stream, nullptr), network_source_,
          type == AVMEDIA_TYPE_VIDEO ? video_last_duration_ns_ : 0));
    }
  }
  // Let demuxers (notably HLS) stop fetching unselected variants/tracks.
  for (unsigned int index = 0; index < format_->nb_streams; ++index) {
    const bool selected =
        std::any_of(streams_.begin(), streams_.end(), [&](const auto& stream) {
          return stream.stream_index == static_cast<int>(index);
        });
    format_->streams[index]->discard =
        selected ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }
}

void FfmpegInput::ReportPerformance(bool final) {
  if (!performance_.Sample(final)) return;
  std::size_t buffered = 0;
  for (const auto& track : tracks_) buffered += track->queued_bytes();
  performance_.Report(generation_, state(), buffered);
}

void FfmpegInput::WaitForPlayback(std::unique_lock<std::mutex>& lock,
                                  Clock::time_point deadline) {
  performance_.BeginWait();
  wake_.wait_until(lock, deadline, [this] {
    return stop_requested_.load() || seek_position_.has_value();
  });
  performance_.EndWait();
  ReportPerformance();
}

bool FfmpegInput::ReadPacket(ffmpeg::Packet& packet) {
  while (!stop_requested_.load()) {
    SetIoDeadline(config_.read_timeout);
    performance_.BeginRead();
    const int result = av_read_frame(format_, packet.get());
    io_deadline_ns_.store(0);
    performance_.Read(*packet.get(), result, stop_requested_.load(), streams_);
    ReportPerformance();
    if (result == AVERROR_EOF) {
      MW_LOG_DEBUG("streamer", "Input[{}]读到EOF: generation={}",
                   static_cast<const void*>(this), generation_);
      input_eof_ = true;
      return false;
    }
    ffmpeg::FfmpegException::throwIfError(result, "av_read_frame");
    const bool selected =
        std::any_of(streams_.begin(), streams_.end(), [&](const auto& stream) {
          return stream.stream_index == packet->stream_index;
        });
    if (packet->size > 0 && selected) {
      return !stop_requested_.load();
    }
    packet.Unref();
  }
  return false;
}

void FfmpegInput::SubmitPacket(const ffmpeg::Packet& packet,
                               ffmpeg::Decoder* decoder) {
  InvokeCallback(on_packet_, generation_, packet);
  if (decoder && !decoder->SendPacket(packet)) {
    throw ffmpeg::FfmpegException(AVERROR_BUG, "解码器同时拒绝输入和输出");
  }
}

void FfmpegInput::PlayPackets(bool initialized) {
  bool received = false;
  while (!stop_requested_.load()) {
    ApplySeek();
    ffmpeg::Packet packet;
    if (!ReadPacket(packet)) {
      break;
    }
    SubmitPacket(packet, nullptr);
    received = true;
    retries_ = 0;
    if (state() != InputState::kConnected) {
      NotifyState(InputState::kConnected);
    }
  }
  if (!stop_requested_.load() && !received && (config_.loop || initialized)) {
    throw ffmpeg::FfmpegException(AVERROR_EOF, "恢复输入未读取到音视频Packet");
  }
}

bool FfmpegInput::PrepareFrames() {
  while (!stop_requested_.load()) {
    bool ready = true;
    bool has_frame = false;
    for (const auto& decoder : tracks_) {
      if (stop_requested_.load()) {
        return false;
      }
      decoder->Prepare(input_eof_, *this);
      ready = ready && (decoder->ready() || decoder->ended());
      has_frame = has_frame || decoder->ready();
    }
    if (ready) {
      return has_frame;
    }
    if (input_eof_) {
      throw ffmpeg::FfmpegException(AVERROR_INVALIDDATA, "decoder EOF drain");
    }
    ffmpeg::Packet packet;
    if (!ReadPacket(packet)) {
      continue;
    }
    for (const auto& decoder : tracks_) {
      if (decoder->stream_index() != packet->stream_index) {
        continue;
      }
      std::size_t buffered = 0;
      for (const auto& track : tracks_) {
        buffered += track->queued_bytes();
      }
      if (static_cast<std::size_t>(packet->size) >
          config_.max_packet_buffer_bytes - buffered) {
        MW_LOG_DEBUG("streamer",
                     "Input[{}]Packet缓存超限: stream={}, buffered_bytes={}, "
                     "packet_bytes={}, limit_bytes={}",
                     static_cast<const void*>(this), packet->stream_index,
                     buffered, packet->size, config_.max_packet_buffer_bytes);
        throw std::length_error("Input轨道Packet缓存超过字节上限");
      }
      performance_.Queued(buffered + static_cast<std::size_t>(packet->size));
      decoder->Push(std::move(packet));
      break;
    }
  }
  return false;
}

bool FfmpegInput::HasSeekRequest() {
  std::lock_guard<std::mutex> lock(wait_mutex_);
  return seek_position_.has_value();
}

bool FfmpegInput::ApplySeek() {
  std::optional<std::chrono::milliseconds> position;
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    if (stop_requested_.load()) {
      return false;
    }
    position = std::exchange(seek_position_, std::nullopt);
  }
  if (!position) {
    return false;
  }
  performance_.Seek();
  // Tracks are selected video-first; audio-only input uses its audio stream.
  const int index = streams_.front().stream_index;
  MW_LOG_DEBUG("streamer", "Input[{}]执行Seek: stream={}, position_ms={}",
               static_cast<const void*>(this), index, position->count());
  auto timestamp =
      av_rescale_q(position->count(), AVRational{1, 1000}, AV_TIME_BASE_Q);
  if (format_->start_time != AV_NOPTS_VALUE) {
    timestamp += format_->start_time;
  }
  timestamp = av_rescale_q(timestamp, AV_TIME_BASE_Q,
                           format_->streams[index]->time_base);
  SetIoDeadline(config_.read_timeout);
  const int result =
      av_seek_frame(format_, index, timestamp, AVSEEK_FLAG_BACKWARD);
  io_deadline_ns_.store(0);
  if (result >= 0) {
    ++generation_;
    MW_LOG_INFO("streamer", "Input[{}]Seek完成: position_ms={}, generation={}",
                static_cast<const void*>(this), position->count(), generation_);
  }
  if (result < 0 && !stop_requested_.load()) {
    // A failed seek still flushes decoding and resumes reading.
    MW_LOG_WARNING("streamer",
                   "Input[{}]Seek失败，清空解码器后继续读取: position_ms={}, "
                   "error={}, reason={}",
                   static_cast<const void*>(this), position->count(), result,
                   ffmpeg::AvErrorStr(result));
  }
  for (const auto& track : tracks_) {
    track->Flush();
  }
  input_eof_ = false;
  return true;
}

void FfmpegInput::PlayAttempt(internal::PlaybackClock& playback,
                              bool first_attempt) {
  const bool initialized = !ready_streams_.empty();
  if (initialized) {
    ++generation_;
    MW_LOG_DEBUG("streamer", "Input[{}]恢复交付: generation={}, mode={}",
                 static_cast<const void*>(this), generation_,
                 ModeName(config_.mode));
  } else {
    ready_streams_ = streams_;
    MW_LOG_INFO("streamer", "Input[{}]轨道就绪: tracks={}, mode={}",
                static_cast<const void*>(this), ready_streams_.size(),
                ModeName(config_.mode));
    InvokeCallback(on_ready_, ready_streams_);
  }
  if (config_.mode == InputMode::kRemux) {
    PlayPackets(initialized);
    return;
  }
  const bool prepared = PrepareFrames();
  if (stop_requested_.load()) {
    return;
  }
  if (!prepared) {
    if (config_.loop || initialized) {
      throw ffmpeg::FfmpegException(AVERROR_EOF, "恢复输入未解码出音视频帧");
    }
    return;
  }
  const auto next_pts = [this] {
    auto pts = std::numeric_limits<std::int64_t>::max();
    for (const auto& track : tracks_) {
      if (track->ready()) {
        pts = std::min(pts, track->pts_ns());
      }
    }
    return pts;
  };
  if (first_attempt) {
    // Establish the output timestamp anchor after preparing the first frames.
    // User initialization and initial decoding must not add timestamp lateness.
    playback = internal::PlaybackClock(
        next_pts(), Clock::now(), internal::SystemTimeBase(),
        av_d2q(config_.playback_speed, std::numeric_limits<int>::max()));
  } else {
    playback.BeginLoop(next_pts());
  }
  retries_ = 0;
  if (state() != InputState::kConnected) {
    NotifyState(InputState::kConnected);
  }
  if (first_attempt) {
    playback.StartDelivery(Clock::now());
  }
  for (;;) {
    if (stop_requested_.load()) {
      return;
    }
    bool has_frames = true;
    if (ApplySeek()) {
      has_frames = PrepareFrames();
      if (stop_requested_.load()) {
        return;
      }
      if (has_frames) {
        playback.Seek(next_pts());
      }
    }
    if (has_frames) {
      {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        WaitForPlayback(lock, playback.deadline());
        if (stop_requested_.load()) {
          return;
        }
        if (seek_position_) {
          continue;
        }
      }
      // Deliver both tracks if due together before reading more input. A read
      // needed for one track must not postpone the other's already-ready frame.
      for (const auto& decoder : tracks_) {
        if (stop_requested_.load()) {
          return;
        }
        if (HasSeekRequest()) {
          break;
        }
        if (decoder->ready() && playback.CanDeliver(decoder->pts_ns())) {
          // Non-key video still advances the shared media clock.
          if (decoder->ShouldDeliver()) {
            InvokeCallback(on_frame_, decoder->stream_index(),
                           decoder->OutputFrame(playback, config_.mode));
          }
          decoder->Consume();
        }
      }
      if (HasSeekRequest()) {
        continue;
      }
      has_frames = PrepareFrames();
    }
    if (!has_frames) {
      std::int64_t end_pts = 0;
      for (const auto& track : tracks_) {
        end_pts = std::max(end_pts, track->next_pts_ns());
      }
      std::unique_lock<std::mutex> lock(wait_mutex_);
      if (config_.loop && !stop_requested_.load()) {
        // Advance the old deadline by max(next_pts) - old clock PTS.
        WaitForPlayback(lock, playback.LoopDeadline(end_pts));
      }
      if (!stop_requested_.load()) {
        if (seek_position_) {
          continue;
        }
        // Commit EOF under the same lock as Seek submission. Later requests
        // observe ended input, or apply to the next committed loop cycle.
        if (config_.loop) {
          playback.EndLoop(end_pts);
          for (const auto& track : tracks_) {
            if (format_->streams[track->stream_index()]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO) {
              video_last_duration_ns_ = track->duration_ns();
            }
          }
        }
        if (!config_.loop) {
          state_.store(InputState::kEnded);
        }
      }
      return;
    }
    playback.Advance(next_pts());
  }
}

void FfmpegInput::ClearAttempt() noexcept {
  MW_LOG_TRACE("streamer", "Input[{}]释放本轮输入与解码器资源",
               static_cast<const void*>(this));
  tracks_.clear();
  streams_.clear();
  avformat_close_input(&format_);
  input_eof_ = false;
  io_deadline_ns_.store(0);
}

void FfmpegInput::RunMedia() {
  MW_LOG_DEBUG("streamer", "Input[{}]媒体线程开始运行",
               static_cast<const void*>(this));
  if (stop_requested_.load()) {
    return;
  }
  int error_code = 0;
  std::string message;
  bool retryable = true;
  bool ended = false;
  try {
    video_last_duration_ns_ = 0;
    internal::PlaybackClock playback(0, {}, internal::SystemTimeBase());
    bool first_attempt = true;
    NotifyState(InputState::kConnecting);
    while (!stop_requested_.load()) {
      OpenAttempt();
      PlayAttempt(playback, first_attempt);
      if (stop_requested_.load()) {
        break;
      }
      if (!config_.loop) {
        ended = true;
        break;
      }
      MW_LOG_DEBUG("streamer", "Input[{}]EOF循环，重新打开输入: generation={}",
                   static_cast<const void*>(this), generation_);
      performance_.Loop();
      ClearAttempt();
      first_attempt = false;
    }
  } catch (const ffmpeg::FfmpegException& error) {
    error_code = error.error_code();
    message = error.what();
  } catch (const std::exception& error) {
    error_code = AVERROR_UNKNOWN;
    message = error.what();
    retryable = false;
  } catch (...) {
    error_code = AVERROR_UNKNOWN;
    message = "Input发生未知异常";
    retryable = false;
  }
  // Release the old connection on its media worker before handing off retry.
  ClearAttempt();
  if (stop_requested_.load()) {
    MW_LOG_DEBUG("streamer", "Input[{}]媒体线程响应停止并退出",
                 static_cast<const void*>(this));
    return;
  }
  if (ended) {
    NotifyState(InputState::kEnded);
    ReportPerformance(true);
    return;
  }
  const bool can_reconnect =
      network_source_ || (config_.loop && !ready_streams_.empty());
  if (error_code == AVERROR_BUG || error_code == AVERROR_BUG2) {
    MW_LOG_CRITICAL("streamer", "Input[{}]内部状态错误: error={}, reason={}",
                    static_cast<const void*>(this), error_code, message);
  } else {
    MW_LOG_ERROR("streamer", "Input[{}]运行失败: error={}, reason={}",
                 static_cast<const void*>(this), error_code, message);
  }
  if (!retryable || !can_reconnect || !config_.auto_reconnect ||
      (config_.max_retries >= 0 && retries_ >= config_.max_retries)) {
    MW_LOG_DEBUG("streamer",
                 "Input[{}]停止重试: retryable={}, can_reconnect={}, "
                 "auto_reconnect={}, retries={}, max_retries={}",
                 static_cast<const void*>(this), retryable, can_reconnect,
                 config_.auto_reconnect, retries_, config_.max_retries);
    NotifyState(InputState::kFailed, error_code, message);
    ReportPerformance(true);
    return;
  }
  ++retries_;
  try {
    ScheduleReconnect(error_code, std::move(message));
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "Input[{}]无法创建重连线程: {}",
                 static_cast<const void*>(this), error.what());
    NotifyState(InputState::kFailed, AVERROR_UNKNOWN, error.what());
    ReportPerformance(true);
  }
}

void FfmpegInput::ScheduleReconnect(int error, std::string message) {
  std::thread previous;
  {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    previous = std::move(reconnect_thread_);
  }
  // The previous reconnect worker created this media worker and then exits;
  // it never waits for this worker, so joining it cannot create a cycle.
  if (previous.joinable()) {
    previous.join();
  }
  std::lock_guard<std::mutex> lock(thread_mutex_);
  if (!stop_requested_.load()) {
    reconnect_thread_ = std::thread(&FfmpegInput::RunReconnect, this, error,
                                    std::move(message));
  }
}

void FfmpegInput::RunReconnect(int error, std::string message) {
  MW_LOG_DEBUG("streamer", "Input[{}]重连线程开始运行",
               static_cast<const void*>(this));
  std::thread previous_media;
  {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    previous_media = std::move(media_thread_);
  }
  if (previous_media.joinable()) {
    previous_media.join();
  }
  if (stop_requested_.load()) {
    return;
  }
  NotifyState(InputState::kWaitingRetry, error, message);
  {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    if (wake_.wait_for(lock, config_.retry_interval,
                       [this] { return stop_requested_.load(); })) {
      MW_LOG_DEBUG("streamer", "Input[{}]取消重连等待",
                   static_cast<const void*>(this));
      return;
    }
  }
  try {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (!stop_requested_.load()) {
      MW_LOG_INFO("streamer", "Input[{}]开始重连: retry={}",
                  static_cast<const void*>(this), retries_);
      performance_.Reconnect();
      media_thread_ = std::thread(&FfmpegInput::RunMedia, this);
    }
  } catch (const std::exception& exception) {
    MW_LOG_ERROR("streamer", "Input[{}]重连无法创建媒体线程: {}",
                 static_cast<const void*>(this), exception.what());
    NotifyState(InputState::kFailed, AVERROR_UNKNOWN, exception.what());
    ReportPerformance(true);
  }
}

}  // namespace mw::streamer
