#include "mw/streamer/pipeline/remux_pipeline.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/pipeline/internal/audio_probe_timeout.h"
#include "mw/streamer/pipeline/internal/remux_audio_probe.h"
#include "mw/streamer/platform/thread.h"
#include "mw/streamer/remuxer/remux_output.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;
using Registration = internal::OutputRegistration;

void ValidateSpeed(double speed) {
  if (!std::isfinite(speed) || speed <= 0)
    throw std::invalid_argument("RemuxPipeline 倍速必须为有限正数");
}

RemuxPipelineConfig PrepareConfig(RemuxPipelineConfig config) {
  ValidateSpeed(config.speed);
  if (config.max_ahead.count() < 0)
    throw std::invalid_argument("RemuxPipeline 提前交付窗口不能为负数");
  // Keep the generated public stream identity stable across Remuxers.
  config.remuxer = Registration::PrepareConfig(std::move(config.remuxer));
  return config;
}

std::vector<ffmpeg::StreamInfo> SelectedStreams(const AvInputInfo& info) {
  std::vector<ffmpeg::StreamInfo> result;
  for (const auto& stream : info.streams) {
    if (stream.stream_index == info.video_stream_index ||
        stream.stream_index == info.audio_stream_index)
      result.push_back(stream);
  }
  if (result.empty())
    throw std::invalid_argument("RemuxPipeline 输入没有音视频轨道");
  return result;
}

bool SameTracks(const std::vector<ffmpeg::StreamInfo>& previous,
                const std::vector<ffmpeg::StreamInfo>& current) {
  if (previous.size() != current.size()) return false;
  for (std::size_t i = 0; i < previous.size(); ++i) {
    if (!(previous[i] == current[i])) return false;
    const auto* left = previous[i].codec_parameters.get();
    const auto* right = current[i].codec_parameters.get();
    if (left->profile != right->profile || left->level != right->level ||
        left->extradata_size != right->extradata_size ||
        (left->extradata_size > 0 &&
         std::memcmp(left->extradata, right->extradata,
                     static_cast<std::size_t>(left->extradata_size)) != 0))
      return false;
  }
  return true;
}

}  // namespace

class RemuxPipeline::Impl final {
 public:
  explicit Impl(RemuxPipelineConfig config)
      : config_(PrepareConfig(std::move(config))),
        input_(config_.input),
        current_(NewRemuxer()),
        speed_(config_.speed) {
    input_.SetOnStateChanged([this](const AvInputProxyStatus& status) {
      if (status.state == AvInputProxyState::kFailed) {
        Fail(status.error);
        return;
      }
      bool waiting_retry = false;
      {
        std::lock_guard lock(mutex_);
        if (started_ && !stopping_ && state_ != RemuxPipelineState::kDraining &&
            state_ != RemuxPipelineState::kEnded &&
            (status.state == AvInputProxyState::kWaitingRetry ||
             (status.state == AvInputProxyState::kConnecting && has_media_))) {
          waiting_retry = state_ != RemuxPipelineState::kWaitingRetry;
          state_ = RemuxPipelineState::kWaitingRetry;
        }
      }
      if (waiting_retry)
        MW_LOG_DEBUG("streamer", "RemuxPipeline 等待输入恢复: instance={}",
                     static_cast<const void*>(this));
      changed_.notify_all();
    });
  }

  ~Impl() { Stop(); }

  void SetOnPacket(OnPacket callback) {
    CheckStopped();
    on_packet_ = std::move(callback);
  }
  void SetOnError(OnError callback) {
    CheckStopped();
    on_error_ = std::move(callback);
  }

  std::string AddPushUrl(const std::string& url) {
    std::lock_guard control(control_);
    RequireAddable();
    const auto output = EnsureRemuxer();
    const auto actual = output->AddPushUrl(url);
    if (!registration_.FindPush(url)) {
      Registration::Entry entry{};
      entry.kind = url.find("://") == std::string::npos
                       ? Registration::Kind::kRecording
                       : Registration::Kind::kPush;
      entry.original = url;
      entry.actual = actual;
      registration_.Add(std::move(entry));
    }
    UpdateOutputInfo(output);
    return actual;
  }

  std::string AddPublish(Registration::Kind kind, const std::string& ip,
                         std::uint16_t port, bool tls) {
    std::lock_guard control(control_);
    RequireAddable();
    auto entry = Registration::PreparePublish(
        kind, config_.remuxer.app, config_.remuxer.stream, ip, port, tls);
    const auto output = EnsureRemuxer();
    if (kind == Registration::Kind::kRtsp)
      output->AddRtspPublish(ip, port, tls);
    else
      output->AddHlsPublish(ip, port, tls);
    const auto actual = entry.actual;
    registration_.Add(std::move(entry));
    UpdateOutputInfo(output);
    return actual;
  }

  bool Start(std::string_view url) {
    {
      std::lock_guard lock(mutex_);
      if (started_) return false;
      started_ = true;
      stopping_ = ended_ = has_media_ = worker_done_ = false;
      state_ = RemuxPipelineState::kStarting;
      error_ = {};
      history_.clear();
      media_clock_.reset();
    }
    MW_LOG_INFO("streamer",
                "RemuxPipeline 开始启动: instance={} speed={} max_ahead_ms={}",
                static_cast<const void*>(this), speed(),
                config_.max_ahead.count());
    fixed_streams_.clear();
    last_times_.clear();
    try {
      {
        std::lock_guard control(control_);
        EnsureRemuxer();
      }
      if (url.empty() || url.find('\0') != std::string_view::npos ||
          url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
        throw std::invalid_argument("RemuxPipeline 输入地址无效");
      const std::string owned_url(url);
      if (!retirement_.Start([this] { Reap(); }))
        throw std::runtime_error("RemuxPipeline 无法创建录像回收线程");
      if (!worker_.Start([this, owned_url] { Run(owned_url); }))
        throw std::runtime_error("RemuxPipeline 无法创建取包线程");
      MW_LOG_DEBUG("streamer", "RemuxPipeline 工作线程已启动: instance={}",
                   static_cast<const void*>(this));
      return true;
    } catch (...) {
      Fail(std::current_exception());
      {
        std::lock_guard lock(mutex_);
        worker_done_ = true;
      }
      changed_.notify_all();
      Stop();
      return false;
    }
  }

  void SetSpeed(double speed) {
    ValidateSpeed(speed);
    {
      std::lock_guard lock(mutex_);
      AdvanceClock(Clock::now());
      speed_ = speed;
      ++speed_revision_;
    }
    changed_.notify_all();
    MW_LOG_INFO("streamer", "RemuxPipeline 设置取包倍速: instance={} speed={}",
                static_cast<const void*>(this), speed);
  }

  double speed() const {
    std::lock_guard lock(mutex_);
    return speed_;
  }
  RemuxPipelineState state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }
  AvInputProxyStatus input_status() const { return input_.status(); }
  std::vector<Remuxer::PushStatus> remuxer_status() const {
    const auto output = Current();
    return output ? output->push_statuses()
                  : std::vector<Remuxer::PushStatus>{};
  }
  std::vector<Remuxer::RecordingStatus> recording_statuses() const {
    std::vector<Remuxer::RecordingStatus> result;
    std::vector<std::shared_ptr<Remuxer>> outputs;
    {
      std::lock_guard lock(mutex_);
      result = history_;
      if (retiring_active_) outputs.push_back(retiring_active_);
      outputs.insert(outputs.end(), retired_.begin(), retired_.end());
      if (current_) outputs.push_back(current_);
    }
    for (const auto& output : outputs) {
      auto recordings = output->recording_statuses();
      result.insert(result.end(), recordings.begin(), recordings.end());
    }
    return result;
  }
  Remuxer::OutputInfo output_info() const {
    std::lock_guard lock(mutex_);
    return output_info_;
  }
  RemuxPipelineStatus status() const {
    RemuxPipelineStatus result;
    {
      std::lock_guard lock(mutex_);
      result.state = state_;
      result.error = error_;
      result.retiring_sessions = retired_.size() + (retiring_active_ ? 1 : 0);
    }
    result.input = input_status();
    result.remuxer = remuxer_status();
    result.recordings = recording_statuses();
    return result;
  }
  std::exception_ptr error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

  void RequestStop() noexcept {
    std::shared_ptr<Remuxer> output;
    bool requested = false;
    {
      std::lock_guard lock(mutex_);
      requested = started_ && !stopping_;
      stopping_ = true;
      if (started_ && !error_ && !ended_)
        state_ = RemuxPipelineState::kStopping;
      output = current_;
    }
    if (requested)
      MW_LOG_DEBUG("streamer", "RemuxPipeline 请求停止: instance={}",
                   static_cast<const void*>(this));
    input_.RequestStop();
    if (output) output->Drain();
    changed_.notify_all();
  }

  void Stop() noexcept {
    bool was_started;
    {
      std::lock_guard lock(mutex_);
      was_started = started_;
    }
    RequestStop();
    if (was_started)
      MW_LOG_DEBUG("streamer", "RemuxPipeline 等待取包与输入停止: instance={}",
                   static_cast<const void*>(this));
    worker_.Join();
    input_.Stop();
    {
      std::lock_guard control(control_);
      CloseCurrent();
      registration_ = {};
    }
    {
      std::lock_guard lock(mutex_);
      worker_done_ = true;
    }
    changed_.notify_all();
    if (was_started)
      MW_LOG_DEBUG("streamer", "RemuxPipeline 等待录像收尾完成: instance={}",
                   static_cast<const void*>(this));
    retirement_.Join();
    {
      std::lock_guard lock(mutex_);
      started_ = false;
      state_ = error_   ? RemuxPipelineState::kFailed
               : ended_ ? RemuxPipelineState::kEnded
                        : RemuxPipelineState::kStopped;
    }
    if (was_started)
      MW_LOG_INFO("streamer", "RemuxPipeline 已停止: instance={} state={}",
                  static_cast<const void*>(this), static_cast<int>(state()));
  }

 private:
  struct PacketTime {
    std::int64_t dts_us;
    std::int64_t duration_us;
  };

  std::shared_ptr<Remuxer> NewRemuxer() {
    auto result = std::make_shared<Remuxer>(config_.remuxer);
    result->SetOnError([this](std::exception_ptr error) { Fail(error); });
    return result;
  }
  std::shared_ptr<Remuxer> Current() const {
    std::lock_guard lock(mutex_);
    return current_;
  }
  // control_ serializes output registration/session transitions only. Never
  // hold it across input reads, pacing waits, callbacks or worker joins.
  std::shared_ptr<Remuxer> EnsureRemuxer() {
    if (auto output = Current()) return output;
    auto output = NewRemuxer();
    Registration registration;
    for (const auto& old : registration_.entries()) {
      auto entry = old;
      if (entry.kind == Registration::Kind::kPush ||
          entry.kind == Registration::Kind::kRecording)
        entry.actual = output->AddPushUrl(entry.original);
      else if (entry.kind == Registration::Kind::kRtsp)
        output->AddRtspPublish(entry.ip, entry.port, entry.tls);
      else
        entry.actual = output->AddHlsPublish(entry.ip, entry.port, entry.tls);
      registration.Add(std::move(entry));
    }
    registration_ = std::move(registration);
    MW_LOG_DEBUG(
        "streamer", "RemuxPipeline 重建输出组件: instance={} outputs={}",
        static_cast<const void*>(this), registration_.entries().size());
    {
      std::lock_guard lock(mutex_);
      current_ = output;
    }
    UpdateOutputInfo(output);
    return output;
  }
  void UpdateOutputInfo(const std::shared_ptr<Remuxer>& output) {
    auto info = output->output_info();
    std::lock_guard lock(mutex_);
    output_info_ = std::move(info);
  }
  void CheckStopped() const {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("配置 RemuxPipeline 前需要 Stop");
  }
  void RequireAddable() const {
    std::lock_guard lock(mutex_);
    if (started_ &&
        (stopping_ || ended_ || state_ == RemuxPipelineState::kDraining))
      throw std::logic_error("RemuxPipeline 正在结束");
  }
  bool Stopping() const {
    std::lock_guard lock(mutex_);
    return stopping_;
  }

  void AdvanceClock(Clock::time_point now) {
    if (media_clock_) {
      *media_clock_ +=
          std::chrono::duration<long double, std::micro>(now - clock_wall_)
              .count() *
          speed_;
    }
    clock_wall_ = now;
  }
  bool Pace(std::int64_t dts_us) {
    std::unique_lock lock(mutex_);
    const auto now = Clock::now();
    AdvanceClock(now);
    // A late source/backpressure reanchors rather than bursting through an
    // accumulated backlog. Small cross-track interleaving stays on one clock.
    if (!media_clock_ || *media_clock_ - dts_us > 250000.0L * speed_) {
      MW_LOG_DEBUG(
          "streamer",
          "RemuxPipeline 建立取包时间基准: instance={} dts_us={} speed={}",
          static_cast<const void*>(this), dts_us, speed_);
      media_clock_ = static_cast<long double>(dts_us);
      clock_wall_ = now;
    }
    MW_LOG_TRACE("streamer",
                 "RemuxPipeline 调度包: instance={} dts_us={} clock_us={} "
                 "speed={} max_ahead_ms={}",
                 static_cast<const void*>(this), dts_us,
                 static_cast<double>(*media_clock_), speed_,
                 config_.max_ahead.count());
    while (!stopping_) {
      const auto current = Clock::now();
      AdvanceClock(current);
      const auto remaining =
          (dts_us - *media_clock_) / speed_ -
          static_cast<long double>(config_.max_ahead.count()) * 1000;
      if (remaining <= 0) return true;
      const auto maximum =
          std::chrono::duration_cast<std::chrono::microseconds>(
              Clock::time_point::max() - current)
              .count();
      const auto wake =
          remaining >= static_cast<long double>(maximum)
              ? Clock::time_point::max()
              : current + std::chrono::microseconds(
                              static_cast<std::int64_t>(std::ceil(remaining)));
      const auto revision = speed_revision_;
      changed_.wait_until(
          lock, wake, [&] { return stopping_ || speed_revision_ != revision; });
    }
    return false;
  }

  void CloseCurrent() {
    const auto output = Current();
    if (!output) return;
    MW_LOG_DEBUG(
        "streamer",
        "RemuxPipeline 关闭网络输出并移交录像收尾: instance={} active={}",
        static_cast<const void*>(this), session_started_);
    output->StopProtocols();
    {
      std::lock_guard lock(mutex_);
      current_.reset();
      if (session_started_) retired_.push_back(output);
    }
    session_started_ = false;
    changed_.notify_all();
  }

  std::vector<ffmpeg::StreamInfo> PrepareStreams(
      const AvInputProxyReadResult& first,
      std::deque<AvInputProxyReadResult>& pending) {
    auto streams = SelectedStreams(*first.info);
    const auto audio =
        std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
          return stream.stream_index == first.info->audio_stream_index;
        });
    if (audio == streams.end() ||
        audio->codec_parameters.get()->codec_id != AV_CODEC_ID_AAC ||
        audio->codec_parameters.get()->extradata_size > 0)
      return streams;

    // Only startup packets are decoded. The source is assumed to keep its
    // audio coding configuration stable within this input connection.
    constexpr std::size_t kAudioPackets = 8;
    constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;
    std::vector<ffmpeg::Packet> packets;
    std::size_t bytes = 0;
    const auto collect = [&](const AvInputProxyReadResult& input) {
      if (!input.packet) return false;
      bytes += static_cast<std::size_t>(input.packet->get()->size);
      if (bytes > kMaxBytes)
        throw std::runtime_error("RemuxPipeline 音频探测缓存超过 16 MiB");
      if (input.packet->get()->stream_index != audio->stream_index)
        return false;
      packets.push_back(input.packet->Ref());
      packets.back()->time_base = audio->time_base;
      std::size_t size = 0;
      const auto* packet = packets.back().get();
      return av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA,
                                     &size) ||
             (packet->size > 2 && packet->data[0] == 0xff &&
              (packet->data[1] & 0xf0) == 0xf0);
    };
    bool configured = collect(first);
    std::optional<internal::AudioProbeTimeout> timeout;
    if (!configured) timeout.emplace(input_);
    while (!configured && packets.size() < kAudioPackets && !Stopping()) {
      auto input = input_.ReadPacket();
      if (Stopping()) return streams;
      if (input.status == AvInputReadStatus::kNull) {
        const bool expired = timeout->Finish();
        if (Stopping()) return streams;
        if (expired) throw std::runtime_error("RemuxPipeline 音频探测等待超时");
        if (const auto failure = input_.status().error)
          std::rethrow_exception(failure);
        throw std::runtime_error("RemuxPipeline 音频探测未取得输入包");
      }
      const bool boundary =
          input.info != first.info || input.status == AvInputReadStatus::kEof;
      if (!boundary) configured = collect(input);
      pending.push_back(std::move(input));
      if (boundary) break;
    }
    const bool expired = timeout && timeout->Finish();
    if (Stopping()) return streams;
    if (expired) throw std::runtime_error("RemuxPipeline 音频探测等待超时");
    if (!Stopping()) {
      *audio = internal::ProbeRemuxAudio(*audio, packets);
      MW_LOG_INFO("streamer",
                  "RemuxPipeline 音频启动探测完成: stream={} packets={} "
                  "cached_bytes={} extradata={}",
                  audio->stream_index, packets.size(), bytes,
                  audio->codec_parameters.get()->extradata_size);
    }
    return streams;
  }

  void Initialize(const std::vector<ffmpeg::StreamInfo>& streams,
                  bool discontinuity) {
    for (const auto& stream : streams) stream.Validate();
    if (!fixed_streams_.empty() && !SameTracks(fixed_streams_, streams))
      throw std::invalid_argument("RemuxPipeline 固定输入源的轨道配置发生变化");
    std::lock_guard control(control_);
    if (session_started_) CloseCurrent();
    if (Stopping()) return;
    const auto output = EnsureRemuxer();
    if (!output->Start(streams))
      throw std::runtime_error("RemuxPipeline Remuxer 启动失败");
    session_started_ = true;
    fixed_streams_ = streams;
    last_times_.clear();
    {
      std::lock_guard lock(mutex_);
      media_clock_.reset();
      has_media_ = true;
      if (!stopping_) state_ = RemuxPipelineState::kRunning;
    }
    MW_LOG_INFO(
        "streamer",
        "RemuxPipeline 建立输出会话: instance={} tracks={} discontinuity={}",
        static_cast<const void*>(this), streams.size(), discontinuity);
  }

  PacketTime Timing(const ffmpeg::Packet& packet) const {
    if (!packet.get() || packet->dts == AV_NOPTS_VALUE ||
        packet->pts == AV_NOPTS_VALUE || packet->time_base.num <= 0 ||
        packet->time_base.den <= 0)
      throw std::invalid_argument("RemuxPipeline 需要有效包时间戳和时间基");
    return {
        av_rescale_q(packet->dts, packet->time_base, AV_TIME_BASE_Q),
        packet->duration > 0
            ? av_rescale_q(packet->duration, packet->time_base, AV_TIME_BASE_Q)
            : 0};
  }
  bool Discontinuous(int stream_index, PacketTime time) const {
    const auto previous = last_times_.find(stream_index);
    if (previous == last_times_.end()) return false;
    const long double delta =
        static_cast<long double>(time.dts_us) - previous->second.dts_us;
    const auto limit = std::max(
        1000000.0L,
        4.0L * std::max(time.duration_us, previous->second.duration_us));
    return delta < 0 || delta > limit;
  }

  void Run(const std::string& url) noexcept {
    bool eof = false;
    try {
      if (!Stopping()) {
        if (!input_.Start(url))
          throw std::runtime_error("RemuxPipeline AvInputProxy 启动失败");
        MW_LOG_DEBUG("streamer", "RemuxPipeline 输入组件启动完成: instance={}",
                     static_cast<const void*>(this));
        if (Stopping()) input_.RequestStop();
      }
      std::shared_ptr<const AvInputInfo> description;
      std::vector<ffmpeg::StreamInfo> streams;
      std::deque<AvInputProxyReadResult> pending;
      while (!Stopping()) {
        auto input =
            pending.empty() ? input_.ReadPacket() : std::move(pending.front());
        if (!pending.empty()) pending.pop_front();
        if (Stopping()) break;
        if (input.status == AvInputReadStatus::kNull) {
          if (const auto failure = input_.status().error)
            std::rethrow_exception(failure);
          throw std::runtime_error("RemuxPipeline 输入意外停止");
        }
        if (input.status == AvInputReadStatus::kEof) {
          MW_LOG_DEBUG("streamer",
                       "RemuxPipeline 输入 EOF: instance={} loop={}",
                       static_cast<const void*>(this), config_.input.loop);
          if (config_.input.loop) continue;
          eof = true;
          {
            std::lock_guard lock(mutex_);
            if (!stopping_) state_ = RemuxPipelineState::kDraining;
          }
          break;
        }
        if (!input.info || !input.packet)
          throw std::logic_error("RemuxPipeline 缺少包或连接描述");
        const auto stream_index = input.packet->get()->stream_index;
        if (stream_index != input.info->video_stream_index &&
            stream_index != input.info->audio_stream_index)
          continue;
        const auto stream =
            std::find_if(input.info->streams.begin(), input.info->streams.end(),
                         [stream_index](const auto& value) {
                           return value.stream_index == stream_index;
                         });
        if (stream == input.info->streams.end())
          throw std::logic_error("RemuxPipeline 缺少包对应的轨道描述");
        // av_read_frame supplies timestamps in the AVStream time base but
        // leaves AVPacket::time_base unset. Attach metadata without rescaling.
        input.packet->get()->time_base = stream->time_base;
        const auto time = Timing(*input.packet);
        const bool discontinuity =
            description == input.info &&
            input.info->timestamp_discontinuities &&
            Discontinuous(input.packet->get()->stream_index, time);
        if (description != input.info || discontinuity) {
          MW_LOG_DEBUG(
              "streamer",
              "RemuxPipeline 输入会话边界: instance={} connection_changed={} "
              "discontinuity={}",
              static_cast<const void*>(this), description != input.info,
              discontinuity);
          if (description != input.info)
            streams = PrepareStreams(input, pending);
          if (Stopping()) break;
          Initialize(streams, discontinuity);
          description = input.info;
        }
        if (!Pace(time.dts_us)) break;
        if (on_packet_) on_packet_(*input.packet);
        if (Stopping()) break;
        const auto output = Current();
        if (!output || !output->SubmitPacket(*input.packet)) {
          if (Stopping()) break;
          throw std::runtime_error("RemuxPipeline 输出拒绝编码包");
        }
        MW_LOG_TRACE("streamer",
                     "RemuxPipeline 已交付包: instance={} stream={} pts={} "
                     "dts={} duration={} bytes={}",
                     static_cast<const void*>(this), stream_index,
                     input.packet->get()->pts, input.packet->get()->dts,
                     input.packet->get()->duration, input.packet->get()->size);
        last_times_[input.packet->get()->stream_index] = time;
        {
          std::lock_guard lock(mutex_);
          if (!stopping_) state_ = RemuxPipelineState::kRunning;
        }
      }
    } catch (...) {
      Fail(std::current_exception());
    }
    if (eof) {
      MW_LOG_DEBUG("streamer",
                   "RemuxPipeline 等待输入 EOF 状态确认: instance={}",
                   static_cast<const void*>(this));
      // ReadPacket acknowledges EOF before the proxy control worker publishes
      // Ended. Let that transition finish before Stop requests cancellation.
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] {
        return stopping_ || input_.status().state == AvInputProxyState::kEnded;
      });
    }
    input_.Stop();
    {
      std::lock_guard control(control_);
      CloseCurrent();
    }
    {
      std::unique_lock lock(mutex_);
      worker_done_ = true;
      changed_.notify_all();
      if (eof) {
        changed_.wait(lock, [&] {
          return stopping_ || (retired_.empty() && !retiring_active_);
        });
        if (!stopping_ && !error_) {
          ended_ = true;
          state_ = RemuxPipelineState::kEnded;
        }
      }
    }
    MW_LOG_INFO("streamer", "RemuxPipeline 取包线程结束: instance={} eof={}",
                static_cast<const void*>(this), eof);
  }

  void Reap() noexcept {
    try {
      for (;;) {
        std::shared_ptr<Remuxer> output;
        {
          std::unique_lock lock(mutex_);
          changed_.wait(lock,
                        [&] { return worker_done_ || !retired_.empty(); });
          if (retired_.empty()) break;
          output = std::move(retired_.front());
          retired_.pop_front();
          retiring_active_ = output;
        }
        MW_LOG_DEBUG("streamer", "RemuxPipeline 开始回收输出会话: instance={}",
                     static_cast<const void*>(this));
        output->Stop();
        auto recordings = output->recording_statuses();
        {
          std::lock_guard lock(mutex_);
          history_.insert(history_.end(), recordings.begin(), recordings.end());
          retiring_active_.reset();
        }
        MW_LOG_DEBUG(
            "streamer",
            "RemuxPipeline 输出会话回收完成: instance={} recordings={}",
            static_cast<const void*>(this), recordings.size());
        changed_.notify_all();
      }
    } catch (...) {
      Fail(std::current_exception());
    }
  }

  void Fail(std::exception_ptr error) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (!started_ || error_) return;
      error_ = error;
      state_ = RemuxPipelineState::kFailed;
    }
    RequestStop();
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer",
                   "RemuxPipeline 不可恢复错误: instance={} error={}",
                   static_cast<const void*>(this), failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer",
                   "RemuxPipeline 不可恢复错误: instance={} 未知异常",
                   static_cast<const void*>(this));
    }
    if (on_error_) {
      try {
        on_error_(error);
      } catch (const std::exception& failure) {
        MW_LOG_WARNING("streamer",
                       "RemuxPipeline OnError 回调异常: instance={} error={}",
                       static_cast<const void*>(this), failure.what());
      } catch (...) {
        MW_LOG_WARNING("streamer",
                       "RemuxPipeline OnError 回调发生未知异常: instance={}",
                       static_cast<const void*>(this));
      }
    }
  }

  const RemuxPipelineConfig config_;
  AvInputProxy input_;
  std::mutex control_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::shared_ptr<Remuxer> current_;
  Registration registration_;
  Remuxer::OutputInfo output_info_;
  std::vector<Remuxer::RecordingStatus> history_;
  std::deque<std::shared_ptr<Remuxer>> retired_;
  std::shared_ptr<Remuxer> retiring_active_;
  RemuxPipelineState state_ = RemuxPipelineState::kStopped;
  bool started_ = false;
  bool stopping_ = false;
  bool ended_ = false;
  bool has_media_ = false;
  bool worker_done_ = false;
  // control_ owns session_started_; only the packet worker uses track timing.
  bool session_started_ = false;
  std::vector<ffmpeg::StreamInfo> fixed_streams_;
  std::map<int, PacketTime> last_times_;
  double speed_;
  std::uint64_t speed_revision_ = 0;
  std::optional<long double> media_clock_;
  Clock::time_point clock_wall_{};
  std::exception_ptr error_;
  OnPacket on_packet_;
  OnError on_error_;
  internal::Thread retirement_;
  internal::Thread worker_;
};

RemuxPipeline::RemuxPipeline(RemuxPipelineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
RemuxPipeline::~RemuxPipeline() = default;
void RemuxPipeline::SetOnPacket(OnPacket callback) {
  impl_->SetOnPacket(std::move(callback));
}
void RemuxPipeline::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
std::string RemuxPipeline::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void RemuxPipeline::AddRtspPublish(const std::string& ip, std::uint16_t port,
                                   bool tls) {
  impl_->AddPublish(Registration::Kind::kRtsp, ip, port, tls);
}
std::string RemuxPipeline::AddHlsPublish(const std::string& ip,
                                         std::uint16_t port, bool tls) {
  return impl_->AddPublish(Registration::Kind::kHls, ip, port, tls);
}
Remuxer::OutputInfo RemuxPipeline::output_info() const {
  return impl_->output_info();
}
bool RemuxPipeline::Start(std::string_view url) { return impl_->Start(url); }
void RemuxPipeline::SetSpeed(double speed) { impl_->SetSpeed(speed); }
double RemuxPipeline::speed() const { return impl_->speed(); }
RemuxPipelineState RemuxPipeline::state() const { return impl_->state(); }
AvInputProxyStatus RemuxPipeline::input_status() const {
  return impl_->input_status();
}
std::vector<Remuxer::PushStatus> RemuxPipeline::remuxer_status() const {
  return impl_->remuxer_status();
}
std::vector<Remuxer::RecordingStatus> RemuxPipeline::recording_statuses()
    const {
  return impl_->recording_statuses();
}
RemuxPipelineStatus RemuxPipeline::status() const { return impl_->status(); }
std::exception_ptr RemuxPipeline::error() const { return impl_->error(); }
void RemuxPipeline::RequestStop() noexcept { impl_->RequestStop(); }
void RemuxPipeline::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
