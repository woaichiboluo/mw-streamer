#include "mw/streamer/input/ffmpeg_input.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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
#include "mw/streamer/input/ffmpeg_frame_decoder.h"
#include "mw/streamer/input/ffmpeg_packet_reader.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;

const char* FindInputProtocol(const char* url) {
  // RTSP URLs are handled by a demuxer, not a standalone AVIO protocol.
  if (std::strncmp(url, "rtsp://", 7) == 0 && av_find_input_format("rtsp")) {
    return "rtsp";
  }
  if (std::strncmp(url, "rtsps://", 8) == 0 && av_find_input_format("rtsp") &&
      avio_find_protocol_name("tls://")) {
    return "rtsps";
  }
  return avio_find_protocol_name(url);
}

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
    MW_LOG_ERROR("streamer", "FFmpeg Input 回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "FFmpeg Input 回调抛出了未知异常");
  }
}

}  // namespace

// Each decoder owns its raw frame queue. Source media fields are never mapped
// to a playback clock; downstream consumers choose their own delivery cadence.
struct FfmpegInput::Track {
  Track(FfmpegInput& input, std::unique_ptr<ffmpeg::Decoder> decoder,
        const ffmpeg::StreamInfo& stream)
      : stream_index_(stream.stream_index),
        description_(stream),
        decoder_(
            std::move(decoder),
            [&input](std::size_t bytes) {
              if (input.packet_reader_) input.packet_reader_->Release(bytes);
            },
            [&input] { input.wake_.notify_all(); },
            [this] { return parser_repeat_pict_.load(); }) {}

  void Push(ffmpeg::Packet packet) { decoder_.Push(std::move(packet)); }
  void Prepare() {
    if (ready_ || ended_) return;
    if (decoder_.TryReceive(frame_)) {
      ready_ = true;
    } else if (decoder_.finished()) {
      ended_ = true;
    }
  }
  void Finish() noexcept { decoder_.Finish(); }
  bool TryReceive(ffmpeg::Frame& frame) {
    if (!decoder_.TryReceive(frame)) return false;
    ++pulled_frames_;
    return true;
  }
  void CheckEnd() {
    decoder_.ThrowIfFailed();
    if (decoder_.finished() && !ended_) {
      source_end_pts_.store(decoder_.end_pts());
      ended_ = true;
    }
  }
  bool source_finished() const noexcept { return decoder_.finished(); }
  std::int64_t source_end_pts() const noexcept {
    const auto live = decoder_.end_pts();
    return live != AV_NOPTS_VALUE ? live : source_end_pts_.load();
  }
  bool has_pulled_frames() const noexcept { return pulled_frames_.load() != 0; }
  void Stop() noexcept { decoder_.Stop(); }
  bool wants_packet() const noexcept { return decoder_.wants_packet(); }
  bool blocked() const noexcept {
    return ended_ ||
           (ready_ ? decoder_.blocked() : decoder_.waiting_for_packet());
  }
  void SnapshotParser(int repeat_pict) noexcept {
    parser_repeat_pict_.store(repeat_pict);
  }
  const ffmpeg::Frame& media_frame() const noexcept { return frame_; }
  void Consume() noexcept {
    ready_ = false;
    frame_.Unref();
  }
  bool ready() const noexcept { return ready_; }
  bool ended() const noexcept { return ended_; }
  bool AnnounceEnd() noexcept {
    if (!ended_ || end_announced_) return false;
    end_announced_ = true;
    return true;
  }
  int stream_index() const noexcept { return stream_index_; }
  const ffmpeg::StreamInfo& description() const noexcept {
    return description_;
  }
  std::int64_t end_pts() const noexcept { return decoder_.end_pts(); }
  std::size_t queued_bytes() const noexcept { return decoder_.queued_bytes(); }

 private:
  int stream_index_;
  ffmpeg::StreamInfo description_;
  std::atomic<int> parser_repeat_pict_{-1};
  std::atomic<std::uint64_t> pulled_frames_{0};
  std::atomic<std::int64_t> source_end_pts_{AV_NOPTS_VALUE};
  internal::FfmpegFrameDecoder decoder_;
  ffmpeg::Frame frame_;
  bool ready_ = false;
  bool ended_ = false;
  bool end_announced_ = false;
};

FfmpegInput::FfmpegInput(FfmpegInputConfig config)
    : config_(std::move(config)) {
  if ((config_.mode != InputMode::kLive && config_.mode != InputMode::kRemux &&
       config_.mode != InputMode::kBatch) ||
      config_.max_retries < -1 || config_.retry_interval.count() <= 0 ||
      config_.open_timeout.count() <= 0 || config_.read_timeout.count() <= 0) {
    MW_LOG_ERROR("streamer",
                 "Input 配置无效: mode={}, max_retries={}, retry_ms={}, "
                 "open_timeout_ms={}, "
                 "read_timeout_ms={}, packet_buffer_bytes={}",
                 static_cast<int>(config_.mode), config_.max_retries,
                 config_.retry_interval.count(), config_.open_timeout.count(),
                 config_.read_timeout.count(), config_.max_packet_buffer_bytes);
    throw std::invalid_argument("FFmpeg Input 配置无效");
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

void FfmpegInput::SetOnSelectStreams(OnSelectStreams callback) {
  on_select_streams_ = std::move(callback);
}

void FfmpegInput::SetOnPacket(OnPacket callback) {
  on_packet_ = std::move(callback);
}

void FfmpegInput::SetOnFrame(OnFrame callback) {
  if (callback && on_decoded_ready_)
    throw std::logic_error("Input 不能同时配置自动交付和按轨读取");
  on_frame_ = std::move(callback);
}

void FfmpegInput::SetOnDecodedReady(OnDecodedReady callback) {
  if (callback && on_frame_)
    throw std::logic_error("Input 不能同时配置自动交付和按轨读取");
  on_decoded_ready_ = std::move(callback);
}

void FfmpegInput::SetOnTrackEnded(OnTrackEnded callback) {
  on_track_ended_ = std::move(callback);
}

void FfmpegInput::SetOnStateChanged(OnStateChanged callback) {
  on_state_changed_ = std::move(callback);
}

void FfmpegInput::Start(std::string_view url) {
  if (url.empty() || std::all_of(url.begin(), url.end(), [](unsigned char ch) {
        return std::isspace(ch);
      })) {
    MW_LOG_ERROR("streamer", "Input[{}] 启动失败: URL 为空",
                 static_cast<const void*>(this));
    throw std::invalid_argument("Input URL 不能为空");
  }
  std::lock_guard<std::mutex> lock(control_mutex_);
  const auto current = state();
  if (current == InputState::kConnecting || current == InputState::kConnected ||
      current == InputState::kWaitingRetry) {
    MW_LOG_ERROR("streamer", "Input[{}] 重复启动: state={}",
                 static_cast<const void*>(this), StateName(current));
    throw std::logic_error("Input 已经启动");
  }
  JoinWorkers();
  {
    std::lock_guard<std::mutex> performance_lock(performance_mutex_);
    performance_.Start(this);
  }
  ready_streams_.clear();
  generation_ = 0;
  url_ = url;
  const char* protocol = FindInputProtocol(url_.c_str());
  network_source_ = protocol && std::strcmp(protocol, "file") != 0;
  {
    std::lock_guard<std::mutex> wait_lock(wait_mutex_);
    seek_position_.reset();
    stop_requested_.store(false);
    read_stop_requested_.store(false);
  }
  retries_ = 0;
  state_.store(InputState::kConnecting);
  MW_LOG_INFO("streamer",
              "Input[{}] 开始启动: protocol={}, mode={}, "
              "video_device={}, loop={}, "
              "auto_reconnect={}, max_retries={}",
              static_cast<const void*>(this), protocol ? protocol : "unknown",
              ModeName(config_.mode),
              video_device_ && video_device_->get() ? "cuda" : "cpu",
              config_.loop, config_.auto_reconnect, config_.max_retries);
  try {
    std::lock_guard<std::mutex> thread_lock(thread_mutex_);
    media_thread_ = std::thread(&FfmpegInput::RunMedia, this);
  } catch (...) {
    state_.store(current);
    ReportPerformance(true);
    MW_LOG_ERROR("streamer", "Input[{}] 启动失败: 无法创建媒体线程",
                 static_cast<const void*>(this));
    throw;
  }
}

void FfmpegInput::Stop() { StopWorkers(true); }

void FfmpegInput::Seek(std::chrono::milliseconds position) {
  if (position.count() < 0) {
    MW_LOG_ERROR("streamer", "Input[{}] Seek 位置无效: position_ms={}",
                 static_cast<const void*>(this), position.count());
    throw std::invalid_argument("Seek 位置不能为负数");
  }
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    const auto current = state();
    if (stop_requested_.load() || (current != InputState::kConnecting &&
                                   current != InputState::kConnected)) {
      MW_LOG_DEBUG(
          "streamer", "Input[{}] 忽略 Seek 请求: state={}, position_ms={}",
          static_cast<const void*>(this), StateName(current), position.count());
      return;
    }
    if (network_source_.load()) {
      MW_LOG_ERROR("streamer", "Input[{}] Seek 失败: 网络输入不支持 Seek",
                   static_cast<const void*>(this));
      throw std::invalid_argument("Seek 仅支持本地文件");
    }
    seek_position_ = position;
    MW_LOG_DEBUG("streamer", "Input[{}] 提交 Seek 请求: position_ms={}",
                 static_cast<const void*>(this), position.count());
  }
  wake_.notify_all();
}

void FfmpegInput::StopWorkers(bool notify) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  const auto current = state();
  const bool already_stopped = current == InputState::kStopped;
  MW_LOG_DEBUG("streamer", "Input[{}] 请求停止: state={}, notify={}",
               static_cast<const void*>(this), StateName(current), notify);
  {
    std::lock_guard<std::mutex> wait_lock(wait_mutex_);
    stop_requested_.store(true);
    seek_position_.reset();
    if (packet_reader_) packet_reader_->RequestStop();
  }
  wake_.notify_all();
  JoinWorkers();
  state_.store(InputState::kStopped);
  ReportPerformance(true);
  if (!already_stopped && (notify || current != InputState::kIdle)) {
    MW_LOG_INFO("streamer", "Input[{}] 停止完成: generation={}",
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
    MW_LOG_DEBUG("streamer", "Input[{}] 状态通知: {}, error={}",
                 static_cast<const void*>(this), StateName(state), error);
    if (state == InputState::kConnected) {
      MW_LOG_INFO("streamer", "Input[{}] 已连接: generation={}, tracks={}",
                  static_cast<const void*>(this), generation_, streams_.size());
    } else if (state == InputState::kEnded) {
      MW_LOG_INFO("streamer", "Input[{}] 读取结束: generation={}",
                  static_cast<const void*>(this), generation_);
    } else if (state == InputState::kWaitingRetry) {
      MW_LOG_WARNING(
          "streamer",
          "Input[{}] 等待重连: retry={}, max_retries={}, delay_ms={}, "
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
  return input->stop_requested_.load() || input->read_stop_requested_.load() ||
         (deadline != 0 && NowNs() >= deadline);
}

void FfmpegInput::SetIoDeadline(std::chrono::milliseconds timeout) {
  io_deadline_ns_.store(
      NowNs() +
      std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
}

void FfmpegInput::OpenAttempt() {
  {
    std::lock_guard<std::mutex> performance_lock(performance_mutex_);
    performance_.SegmentReset();
  }
  MW_LOG_DEBUG(
      "streamer", "Input[{}] 开始打开输入: retry={}, open_timeout_ms={}",
      static_cast<const void*>(this), retries_, config_.open_timeout.count());
  format_ = avformat_alloc_context();
  if (!format_) {
    throw std::bad_alloc();
  }
  // Installed before open and probing, so Stop can interrupt both phases.
  format_->interrupt_callback = {&FfmpegInput::InterruptCallback, this};
  SetIoDeadline(config_.open_timeout);
  AVDictionary* options = nullptr;
  const AVInputFormat* input_format = nullptr;
  std::string input_url = url_;
  const auto separator = url_.find("://");
  const auto scheme = separator == std::string::npos
                          ? std::string{}
                          : url_.substr(0, separator);
  if (scheme == "dshow" || scheme == "v4l2" || scheme == "iec61883") {
    input_format = av_find_input_format(scheme.c_str());
    if (!input_format) {
      throw ffmpeg::FfmpegException(AVERROR_DEMUXER_NOT_FOUND,
                                    "查找输入格式 " + scheme);
    }
    input_url = url_.substr(separator + 3);
  }
  int result = 0;
  const auto set_option = [&](const char* key, const char* value) {
    if (result >= 0) result = av_dict_set(&options, key, value, 0);
  };
  if (scheme == "http" || scheme == "https") {
    set_option("multiple_requests", "1");
    set_option("reconnect", "1");
    set_option("reconnect_streamed", "1");
    set_option("reconnect_delay_max", "120");
    set_option("referer", url_.c_str());
  } else if (scheme == "rtmp" || scheme == "rtmps") {
    set_option("rtmp_live", "live");
  }
  if (!input_format) set_option("rw_timeout", "60000000");
  for (const auto& [key, value] : config_.options) {
    if (result < 0) break;
    result = av_dict_set(&options, key.c_str(), value.c_str(), 0);
  }
  if (result >= 0) {
    result = avformat_open_input(&format_, input_url.c_str(), input_format,
                                 &options);
  }
  if (result >= 0) {
    const AVDictionaryEntry* unused = nullptr;
    while ((unused = av_dict_get(options, "", unused, AV_DICT_IGNORE_SUFFIX))) {
      MW_LOG_WARNING("streamer", "Input[{}] FFmpeg 未使用输入选项: {}",
                     static_cast<const void*>(this), unused->key);
    }
  }
  av_dict_free(&options);
  ffmpeg::FfmpegException::throwIfError(result, "avformat_open_input");
  MW_LOG_DEBUG("streamer", "Input[{}] 输入已打开，开始探测轨道",
               static_cast<const void*>(this));
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(format_, nullptr), "avformat_find_stream_info");
  io_deadline_ns_.store(0);
  MW_LOG_DEBUG("streamer", "Input[{}] 轨道探测完成: streams={}",
               static_cast<const void*>(this), format_->nb_streams);

  PrepareStreams();
  CreateDecoders();
}

void FfmpegInput::PrepareStreams() {
  std::vector<ffmpeg::StreamInfo> available;
  // Demux all packets, but construct decoders only for requested sources.
  for (unsigned int index = 0; index < format_->nb_streams; ++index) {
    auto* stream = format_->streams[index];
    const auto type = stream->codecpar->codec_type;
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;
    ffmpeg::StreamInfo info{stream->index,
                            ffmpeg::CodecParameters(*stream->codecpar),
                            stream->time_base};
    info.disposition = stream->disposition;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(stream->metadata, "", entry,
                                AV_DICT_IGNORE_SUFFIX))) {
      info.metadata.emplace(entry->key, entry->value);
    }
    MW_LOG_DEBUG("streamer",
                 "Input[{}] 描述轨道: stream={}, type={}, codec={}, format={}, "
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
    available.push_back(std::move(info));
  }
  if (available.empty()) {
    throw ffmpeg::FfmpegException(AVERROR_STREAM_NOT_FOUND,
                                  "Input 没有音视频轨道");
  }
  std::vector<int> indices;
  if (on_select_streams_) {
    // Selection is part of preparation, rather than a best-effort notification.
    indices = on_select_streams_(available);
  } else {
    for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
      for (const auto& info : available) {
        if (info.codec_parameters.get()->codec_type != type) continue;
        indices.push_back(info.stream_index);
        break;
      }
    }
  }
  if (indices.empty()) throw std::invalid_argument("Input 选轨结果不能为空");
  std::vector<ffmpeg::StreamInfo> selected;
  for (const auto index : indices) {
    const auto found = std::find_if(
        available.begin(), available.end(),
        [&](const auto& info) { return info.stream_index == index; });
    if (found == available.end())
      throw std::invalid_argument("Input 选轨索引不存在");
    if (std::any_of(selected.begin(), selected.end(), [&](const auto& info) {
          return info.stream_index == index;
        }))
      throw std::invalid_argument("Input 选轨索引不能重复");
    found->Validate();
    if (found->codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO &&
        found->codec_parameters.get()->ch_layout.nb_channels == 0)
      throw std::invalid_argument("Input 所选音轨没有有效声道");
    selected.push_back(*found);
  }
  streams_ = std::move(selected);
}

void FfmpegInput::CreateDecoders() {
  // Compare the full selected stream set before opening any new decoder.
  if (config_.mode != InputMode::kRemux) {
    for (const auto& info : streams_) {
      auto* stream = format_->streams[info.stream_index];
      const auto type = info.codec_parameters.get()->codec_type;
      if (stop_requested_.load()) return;
      auto decoder_stream = info;
      if (type == AVMEDIA_TYPE_VIDEO) {
        auto* parameters = decoder_stream.codec_parameters.get();
        parameters->framerate = av_guess_frame_rate(nullptr, stream, nullptr);
        parameters->sample_aspect_ratio =
            av_guess_sample_aspect_ratio(nullptr, stream, nullptr);
        const auto alpha = info.metadata.find("alpha_mode");
        if (alpha != info.metadata.end() && !alpha->second.empty() &&
            alpha->second.front() == '1') {
          switch (parameters->format) {
            case AV_PIX_FMT_YUV420P:
              parameters->format = AV_PIX_FMT_YUVA420P;
              break;
            case AV_PIX_FMT_YUV422P:
              parameters->format = AV_PIX_FMT_YUVA422P;
              break;
            case AV_PIX_FMT_YUV444P:
              parameters->format = AV_PIX_FMT_YUVA444P;
              break;
            default:
              break;
          }
        }
      }
      const auto& decoder_name = type == AVMEDIA_TYPE_VIDEO
                                     ? config_.video_decoder_name
                                     : config_.audio_decoder_name;
      MW_LOG_DEBUG("streamer",
                   "Input[{}] 创建解码器: stream={}, type={}, name={}",
                   static_cast<const void*>(this), info.stream_index,
                   av_get_media_type_string(type),
                   decoder_name.empty() ? "auto" : decoder_name.c_str());
      std::unique_ptr<ffmpeg::Decoder> decoder;
      if (type == AVMEDIA_TYPE_VIDEO) {
        if (video_device_) {
          decoder = std::make_unique<ffmpeg::VideoDecoder>(
              decoder_stream, *video_device_, config_.video_decoder_name);
        } else {
          decoder = std::make_unique<ffmpeg::VideoDecoder>(
              decoder_stream, config_.video_decoder_name);
        }
      } else {
        decoder = std::make_unique<ffmpeg::AudioDecoder>(
            decoder_stream, config_.audio_decoder_name);
      }
      tracks_.push_back(
          std::make_shared<Track>(*this, std::move(decoder), decoder_stream));
    }
  }
}

void FfmpegInput::ReportPerformance(bool final) {
  std::size_t buffered = 0;
  std::size_t high_water = 0;
  if (packet_reader_) {
    buffered = packet_reader_->outstanding_bytes();
    high_water = packet_reader_->high_water_bytes();
  } else {
    for (const auto& track : tracks_) buffered += track->queued_bytes();
  }
  std::lock_guard<std::mutex> performance_lock(performance_mutex_);
  performance_.Queued(high_water);
  if (!performance_.Sample(final)) return;
  performance_.Report(generation_, state(), buffered);
}

int FfmpegInput::ReadSelectedPacket(ffmpeg::Packet& packet) {
  while (!stop_requested_.load() && !read_stop_requested_.load()) {
    SetIoDeadline(config_.read_timeout);
    {
      std::lock_guard<std::mutex> performance_lock(performance_mutex_);
      performance_.BeginRead();
    }
    int result = av_read_frame(format_, packet.get());
    const auto deadline = io_deadline_ns_.exchange(0);
    // Distinguish deadline expiry from cancellation before the reader handles
    // AVERROR_EXIT so network timeouts retain the retry policy.
    if (result == AVERROR_EXIT && deadline && NowNs() >= deadline &&
        !stop_requested_.load() && !read_stop_requested_.load())
      result = AVERROR(ETIMEDOUT);
    {
      std::lock_guard<std::mutex> performance_lock(performance_mutex_);
      performance_.Read(*packet.get(), result,
                        stop_requested_.load() || read_stop_requested_.load(),
                        streams_);
    }
    if (result < 0) return result;
    for (const auto& track : tracks_) {
      if (track->stream_index() != packet->stream_index) continue;
      const auto* parser =
          av_stream_get_parser(format_->streams[packet->stream_index]);
      track->SnapshotParser(parser ? parser->repeat_pict : -1);
      break;
    }
    if (packet->size > 0) {
      return 0;
    }
    packet.Unref();
  }
  return AVERROR_EXIT;
}

bool FfmpegInput::ReadPacket(ffmpeg::Packet& packet) {
  if (!packet_reader_) return false;
  const bool received = packet_reader_->Pop(packet);
  ReportPerformance();
  if (!received && !stop_requested_.load()) input_eof_ = true;
  return received;
}

void FfmpegInput::StartReadAhead() {
  if (stop_requested_.load()) return;
  read_stop_requested_.store(false);
  auto reader = std::make_unique<internal::FfmpegPacketReader>(
      [this](auto& packet) { return ReadSelectedPacket(packet); },
      [this] { read_stop_requested_.store(true); },
      config_.max_packet_buffer_bytes
          ? config_.max_packet_buffer_bytes
          : std::numeric_limits<std::size_t>::max());
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    if (!stop_requested_.load()) {
      packet_reader_ = std::move(reader);
      return;
    }
  }
  reader->Stop();
}

void FfmpegInput::StopReadAhead() noexcept {
  std::unique_ptr<internal::FfmpegPacketReader> reader;
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    reader = std::move(packet_reader_);
  }
  if (reader) {
    reader->Stop();
    std::lock_guard<std::mutex> performance_lock(performance_mutex_);
    performance_.Queued(reader->high_water_bytes());
  }
  read_stop_requested_.store(false);
}

void FfmpegInput::SubmitPacket(const ffmpeg::Packet& packet) {
  if (std::none_of(streams_.begin(), streams_.end(), [&](const auto& stream) {
        return stream.stream_index == packet->stream_index;
      }))
    return;
  InvokeCallback(on_packet_, generation_, packet);
}

void FfmpegInput::PlayPackets(bool initialized) {
  bool received = false;
  while (!stop_requested_.load()) {
    ApplySeek();
    ffmpeg::Packet packet;
    if (!ReadPacket(packet)) {
      break;
    }
    const bool media =
        std::any_of(streams_.begin(), streams_.end(), [&](const auto& stream) {
          return stream.stream_index == packet->stream_index;
        });
    SubmitPacket(packet);
    if (packet_reader_)
      packet_reader_->Release(static_cast<std::size_t>(packet->size));
    if (!media) continue;
    received = true;
    retries_ = 0;
    if (state() != InputState::kConnected) {
      NotifyState(InputState::kConnected);
    }
  }
  if (!stop_requested_.load() && !received && (config_.loop || initialized)) {
    throw ffmpeg::FfmpegException(AVERROR_EOF, "恢复输入未读取到音视频 Packet");
  }
}

bool FfmpegInput::PumpPackets() {
  if (!packet_reader_ || input_eof_ || stop_requested_.load()) return false;
  bool progress = false;
  while (!stop_requested_.load() && !HasSeekRequest() &&
         std::any_of(tracks_.begin(), tracks_.end(),
                     [](const auto& track) { return track->wants_packet(); })) {
    ffmpeg::Packet packet;
    bool received = false;
    try {
      received = packet_reader_->TryPop(packet);
    } catch (...) {
      // Already-dispatched packets belong to this attempt. Drain their frames
      // before retrying or failing, preserving the original read exception.
      read_error_ = std::current_exception();
      input_eof_ = true;
      for (const auto& track : tracks_) track->Finish();
      return true;
    }
    if (!received) {
      if (packet_reader_->finished()) {
        input_eof_ = true;
        for (const auto& track : tracks_) track->Finish();
        progress = true;
      }
      break;
    }
    SubmitPacket(packet);
    bool dispatched = false;
    for (const auto& track : tracks_) {
      if (track->stream_index() == packet->stream_index) {
        track->Push(std::move(packet));
        dispatched = true;
        break;
      }
    }
    if (!dispatched)
      packet_reader_->Release(static_cast<std::size_t>(packet->size));
    progress = true;
  }
  return progress;
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
  // Stop all codec users before releasing their shared packet reservations.
  for (const auto& track : tracks_) track->Stop();
  StopReadAhead();
  {
    std::lock_guard<std::mutex> performance_lock(performance_mutex_);
    performance_.Seek();
  }
  tracks_.clear();
  if (position->count() == 0) {
    // Reopen at the start to support formats that cannot seek to zero.
    avformat_close_input(&format_);
    streams_.clear();
    OpenAttempt();
    ready_streams_ = streams_;
    ++generation_;
    InvokeCallback(on_ready_, ready_streams_);
  } else {
    auto timestamp =
        av_rescale_q(position->count(), AVRational{1, 1000}, AV_TIME_BASE_Q);
    if (format_->start_time != AV_NOPTS_VALUE) timestamp += format_->start_time;
    SetIoDeadline(config_.read_timeout);
    const int result = avformat_seek_file(
        format_, -1, std::numeric_limits<std::int64_t>::min(), timestamp,
        timestamp, 0);
    io_deadline_ns_.store(0);
    ffmpeg::FfmpegException::throwIfError(result, "avformat_seek_file");
    // Recreate decoders so seeking does not retain prior codec state.
    PrepareStreams();
    CreateDecoders();
    ++generation_;
    ready_streams_ = streams_;
    InvokeCallback(on_ready_, ready_streams_);
  }
  MW_LOG_INFO("streamer", "Input[{}] Seek 完成: position_ms={}, generation={}",
              static_cast<const void*>(this), position->count(), generation_);
  input_eof_ = false;
  read_error_ = nullptr;
  const auto start_time_us =
      (format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time) +
      position->count() * 1000;
  AnnounceDecodedSource(start_time_us, true);
  StartReadAhead();
  return true;
}

void FfmpegInput::AnnounceDecodedSource(std::int64_t start_time_us,
                                        bool flush) {
  if (on_decoded_ready_) {
    source_completed_frames_ = std::make_shared<std::atomic<std::int64_t>>(-1);
    DecodedInputSource source;
    source.start_time_us = start_time_us;
    source.media_origin_us =
        format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time;
    source.loop = config_.loop;
    source.flush = flush;
    if (format_->duration > 0) {
      const auto origin =
          format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time;
      source.duration_us = std::max<std::int64_t>(
          0, format_->duration - (start_time_us - origin));
    }
    source.complete = [completed = source_completed_frames_](auto count) {
      if (count < 0) throw std::invalid_argument("输入段 frame 计数不能为负");
      std::int64_t pending = -1;
      completed->compare_exchange_strong(pending, count);
    };
    for (const auto& track : tracks_) {
      source.tracks.push_back(
          {track->description(),
           [track](auto& frame) { return track->TryReceive(frame); },
           [track] { return track->source_finished(); },
           [track] { return track->source_end_pts(); }});
    }
    InvokeCallback(on_decoded_ready_, source);
  }
}

std::int64_t FfmpegInput::PlayAttempt() {
  const bool initialized = !ready_streams_.empty();
  if (initialized) ++generation_;
  ready_streams_ = streams_;
  InvokeCallback(on_ready_, ready_streams_);
  AnnounceDecodedSource(
      format_->start_time == AV_NOPTS_VALUE ? 0 : format_->start_time);
  StartReadAhead();
  if (config_.mode == InputMode::kRemux) {
    PlayPackets(initialized);
    // Remux has no decoded channel-frame producer.
    return 0;
  }
  bool received = false;
  std::int64_t delivered = 0;
  const auto has_video = [this] {
    return std::any_of(streams_.begin(), streams_.end(),
                       [](const auto& stream) {
                         return stream.codec_parameters.get()->codec_type ==
                                AVMEDIA_TYPE_VIDEO;
                       });
  };
  while (!stop_requested_.load()) {
    if (HasSeekRequest() && ApplySeek()) {
      delivered = 0;
    }
    if (source_completed_frames_ && source_completed_frames_->load() >= 0) {
      for (const auto& track : tracks_) {
        if (track->has_pulled_frames()) {
          retries_ = 0;
          if (state() != InputState::kConnected)
            NotifyState(InputState::kConnected);
        }
        track->CheckEnd();
        if (track->AnnounceEnd() && !read_error_)
          InvokeCallback(on_track_ended_, track->stream_index(),
                         track->source_end_pts());
      }
      if (read_error_) std::rethrow_exception(read_error_);
      return source_completed_frames_->load();
    }
    bool progress = PumpPackets();
    bool ended = true;
    for (const auto& track : tracks_) {
      if (stop_requested_.load() || HasSeekRequest()) break;
      if (on_decoded_ready_) {
        track->CheckEnd();
        if (track->has_pulled_frames()) {
          received = true;
          retries_ = 0;
          if (state() != InputState::kConnected)
            NotifyState(InputState::kConnected);
        }
      } else
        track->Prepare();
      if (track->ready()) {
        retries_ = 0;
        if (state() != InputState::kConnected)
          NotifyState(InputState::kConnected);
        InvokeCallback(on_frame_, track->stream_index(), track->media_frame());
        if (has_video() ? track->media_frame()->width > 0
                        : track->media_frame()->nb_samples > 0)
          ++delivered;
        track->Consume();
        received = progress = true;
      }
      if (track->AnnounceEnd()) {
        if (!read_error_) {
          InvokeCallback(on_track_ended_, track->stream_index(),
                         track->end_pts());
        }
        progress = true;
      }
      ended = ended && track->ended();
    }
    if (stop_requested_.load()) return delivered;
    if (HasSeekRequest()) continue;
    if (ended) {
      if (read_error_) std::rethrow_exception(read_error_);
      if (!received && (config_.loop || initialized)) {
        throw ffmpeg::FfmpegException(AVERROR_EOF, "恢复输入未解码出音视频帧");
      }
      if (!on_decoded_ready_) return delivered;
    }
    if (!on_decoded_ready_ &&
        std::all_of(tracks_.begin(), tracks_.end(),
                    [](const auto& track) { return track->blocked(); }) &&
        packet_reader_ && packet_reader_->budget_blocked()) {
      throw std::length_error("Input 轨道 Packet 缓存超过字节上限");
    }
    ReportPerformance();
    if (!progress) {
      std::unique_lock<std::mutex> lock(wait_mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(1));
    }
  }
  return delivered;
}

void FfmpegInput::ClearAttempt() noexcept {
  MW_LOG_TRACE("streamer", "Input[{}] 释放本轮输入与解码器资源",
               static_cast<const void*>(this));
  for (const auto& track : tracks_) track->Stop();
  StopReadAhead();
  tracks_.clear();
  source_completed_frames_.reset();
  streams_.clear();
  avformat_close_input(&format_);
  input_eof_ = false;
  read_error_ = nullptr;
  io_deadline_ns_.store(0);
}

void FfmpegInput::RunMedia() {
  MW_LOG_DEBUG("streamer", "Input[{}] 媒体线程开始运行",
               static_cast<const void*>(this));
  if (stop_requested_.load()) {
    return;
  }
  int error_code = 0;
  std::string message;
  bool retryable = true;
  bool ended = false;
  try {
    NotifyState(InputState::kConnecting);
    while (!stop_requested_.load()) {
      OpenAttempt();
      const auto frames = PlayAttempt();
      if (stop_requested_.load()) {
        break;
      }
      if (!config_.loop || (config_.mode != InputMode::kRemux && frames <= 2)) {
        ended = true;
        break;
      }
      MW_LOG_DEBUG("streamer",
                   "Input[{}] EOF 循环，重新打开输入: generation={}",
                   static_cast<const void*>(this), generation_);
      {
        std::lock_guard<std::mutex> performance_lock(performance_mutex_);
        performance_.Loop();
      }
      ClearAttempt();
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
    message = "Input 发生未知异常";
    retryable = false;
  }
  // Release the old connection on its media worker before handing off retry.
  ClearAttempt();
  if (stop_requested_.load()) {
    MW_LOG_DEBUG("streamer", "Input[{}] 媒体线程响应停止并退出",
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
    MW_LOG_CRITICAL("streamer", "Input[{}] 内部状态错误: error={}, reason={}",
                    static_cast<const void*>(this), error_code, message);
  } else {
    MW_LOG_ERROR("streamer", "Input[{}] 运行失败: error={}, reason={}",
                 static_cast<const void*>(this), error_code, message);
  }
  if (!retryable || !can_reconnect || !config_.auto_reconnect ||
      (config_.max_retries >= 0 && retries_ >= config_.max_retries)) {
    MW_LOG_DEBUG("streamer",
                 "Input[{}] 停止重试: retryable={}, can_reconnect={}, "
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
    MW_LOG_ERROR("streamer", "Input[{}] 无法创建重连线程: {}",
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
  MW_LOG_DEBUG("streamer", "Input[{}] 重连线程开始运行",
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
      MW_LOG_DEBUG("streamer", "Input[{}] 取消重连等待",
                   static_cast<const void*>(this));
      return;
    }
  }
  try {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (!stop_requested_.load()) {
      MW_LOG_INFO("streamer", "Input[{}] 开始重连: retry={}",
                  static_cast<const void*>(this), retries_);
      {
        std::lock_guard<std::mutex> performance_lock(performance_mutex_);
        performance_.Reconnect();
      }
      media_thread_ = std::thread(&FfmpegInput::RunMedia, this);
    }
  } catch (const std::exception& exception) {
    MW_LOG_ERROR("streamer", "Input[{}] 重连无法创建媒体线程: {}",
                 static_cast<const void*>(this), exception.what());
    NotifyState(InputState::kFailed, AVERROR_UNKNOWN, exception.what());
    ReportPerformance(true);
  }
}

}  // namespace mw::streamer
