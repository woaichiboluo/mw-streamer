#include "mw/streamer/remuxer/remuxer.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "Poller/EventPoller.h"
#include "mw/log.h"
#include "mw/streamer/performance/latency.h"
#include "mw/streamer/remuxer/packet_converter.h"
#include "mw/streamer/remuxer/packet_interleaver.h"
#include "mw/streamer/remuxer/remux_output.h"

namespace mw::streamer {
class Remuxer::Impl final : public std::enable_shared_from_this<Impl> {
 public:
  explicit Impl(RemuxerConfig config)
      : config_(
            internal::OutputRegistration::PrepareConfig(std::move(config))) {}

  void SetOnError(OnError callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(callback);
  }

  void Start(const std::vector<ffmpeg::StreamInfo>& streams) {
    std::lock_guard<std::mutex> control(control_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_) throw std::logic_error("Remuxer 已经启动");
    }
    // A prior Drain/error may have completed without Stop. Join file workers
    // on the lifecycle caller before replacing their last status snapshots.
    for (const auto& recording : recordings_) recording.recorder->Stop();
    auto session_streams = streams;
    std::vector<Recording> recordings;
    recordings.reserve(registration_.entries().size());
    auto poller =
        poller_ ? poller_ : toolkit::EventPollerPool::Instance().getPoller();
    auto info = registration_.info();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      output_info_ = std::move(info);
    }
    try {
      Sync(poller, [&] {
        auto interleaver =
            std::make_unique<internal::PacketInterleaver>(streams);
        auto converter = std::make_unique<internal::PacketConverter>(streams);
        auto timestamps =
            std::make_unique<internal::MediaTimestampConverter>(streams);
        std::weak_ptr<Impl> weak = shared_from_this();
        auto output = std::make_shared<internal::RemuxOutput>(
            poller,
            [weak](std::exception_ptr error) {
              if (auto self = weak.lock()) self->Error(error);
            },
            config_);
        output->Start(converter->tracks());
        bool record_only = !registration_.entries().empty();
        try {
          for (const auto& entry : registration_.entries()) {
            if (entry.kind != internal::OutputRegistration::Kind::kRecording) {
              output->AddPrepared(entry);
              record_only = false;
            }
          }
          for (const auto& entry : registration_.entries()) {
            if (entry.kind == internal::OutputRegistration::Kind::kRecording) {
              // Keep ownership on the lifecycle stack until commit, so
              // rollback joins these workers outside the output poller.
              recordings.push_back(
                  {entry.actual, std::make_shared<Recorder>()});
              StartRecording(recordings.back(), streams);
            }
          }
        } catch (...) {
          const auto error = std::current_exception();
          try {
            output->Close();
          } catch (const std::exception& failure) {
            MW_LOG_ERROR("streamer", "Remuxer 启动回滚失败: {}",
                         failure.what());
          }
          std::rethrow_exception(error);
        }
        output_ = std::move(output);
        converter_ = std::move(converter);
        record_only_ = record_only;
        convert_latency_.Start(PerformanceStage::kRemuxerConvert, this);
        output_latency_.Start(PerformanceStage::kRemuxerOutput, this);
        std::lock_guard<std::mutex> lock(mutex_);
        recordings_ = std::move(recordings);
        streams_ = std::move(session_streams);
        if (!poller_) poller_ = poller;
        interleaver_ = std::move(interleaver);
        timestamps_ = std::move(timestamps);
        handoff_.reset();
        running_ = accepting_ = true;
        draining_ = closing_ = scheduled_ = false;
        error_reported_ = false;
      });
    } catch (...) {
      const auto error = std::current_exception();
      Sync(poller, [] {});
      std::rethrow_exception(error);
    }
    MW_LOG_INFO(
        "streamer",
        "Remuxer 已启动: instance={} app={} stream={} tracks={} outputs={}",
        static_cast<const void*>(this), config_.app, config_.stream,
        streams.size(), registration_.entries().size());
  }

  bool SubmitPacket(const ffmpeg::Packet& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!accepting_) return false;
    if (poller_->isCurrentThread())
      throw std::logic_error("Remuxer 不允许在输出 poller 上提交编码包");
    interleaver_->Validate(packet);
    const bool blocked = handoff_.has_value();
    if (blocked)
      MW_LOG_DEBUG("streamer", "Remuxer 等待交接槽: instance={} stream={}",
                   static_cast<const void*>(this), packet->stream_index);
    writable_.wait(lock, [&] { return !accepting_ || !handoff_; });
    if (blocked)
      MW_LOG_DEBUG("streamer",
                   "Remuxer 交接槽等待结束: instance={} accepting={}",
                   static_cast<const void*>(this), accepting_);
    if (!accepting_) return false;
    handoff_ = internal::PacketInterleaver::Item{packet.Ref()};
    MW_LOG_TRACE("streamer",
                 "Remuxer 接收包: instance={} stream={} pts={} dts={} "
                 "time_base={}/{} size={} interleaved_queue={}",
                 static_cast<const void*>(this), packet->stream_index,
                 packet->pts, packet->dts, packet->time_base.num,
                 packet->time_base.den, packet->size,
                 interleaver_->queued_packets());
    Schedule();
    return true;
  }

  std::string AddPushUrl(const std::string& url) {
    std::lock_guard<std::mutex> control(control_);
    RequireRegisterable();
    if (const auto* existing = registration_.FindPush(url))
      return existing->actual;
    return Register(internal::OutputRegistration::PreparePush(url));
  }

  void AddRtspPublish(const std::string& ip, std::uint16_t port, bool tls) {
    std::lock_guard<std::mutex> control(control_);
    Register(internal::OutputRegistration::PreparePublish(
        internal::OutputRegistration::Kind::kRtsp, config_.app, config_.stream,
        ip, port, tls));
  }

  std::string AddHlsPublish(const std::string& ip, std::uint16_t port,
                            bool tls) {
    std::lock_guard<std::mutex> control(control_);
    return Register(internal::OutputRegistration::PreparePublish(
        internal::OutputRegistration::Kind::kHls, config_.app, config_.stream,
        ip, port, tls));
  }

  Remuxer::OutputInfo output_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_info_;
  }

  std::vector<Remuxer::PushStatus> push_statuses() const {
    toolkit::EventPoller::Ptr poller;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      poller = poller_;
    }
    std::vector<Remuxer::PushStatus> result;
    if (poller) {
      Sync(poller, [&] {
        if (output_) result = output_->push_statuses();
      });
    }
    return result;
  }

  std::vector<Remuxer::RecordingStatus> recording_statuses() const {
    std::vector<Recording> recordings;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      recordings = recordings_;
    }
    std::vector<Remuxer::RecordingStatus> result;
    result.reserve(recordings.size());
    for (const auto& recording : recordings)
      result.push_back({recording.path, recording.recorder->status()});
    return result;
  }

  void Drain() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || closing_) return;
    if (!draining_)
      MW_LOG_DEBUG(
          "streamer",
          "Remuxer 开始排空: instance={} handoff={} interleaved_queue={}",
          static_cast<const void*>(this), handoff_.has_value(),
          interleaver_->queued_packets());
    accepting_ = false;
    draining_ = true;
    writable_.notify_all();
    Schedule();
  }

  void StopProtocols() noexcept {
    std::lock_guard<std::mutex> control(control_);
    StopProtocolsLocked();
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> control(control_);
    StopProtocolsLocked();
    // control_ prevents additions/restarts while status queries remain free
    // to inspect the Recorders. Never join file IO on a ZLM poller.
    for (const auto& recording : recordings_) recording.recorder->Stop();
  }

 private:
  // The lifecycle caller holds control_; Recorder joins remain outside this
  // helper so protocol teardown never waits for the file writer.
  void StopProtocolsLocked() noexcept {
    Drain();
    std::unique_lock<std::mutex> lock(mutex_);
    stopped_.wait(lock, [&] { return !running_; });
    if (!registration_.entries().empty())
      MW_LOG_DEBUG("streamer", "Remuxer 释放输出注册: instance={} outputs={}",
                   static_cast<const void*>(this),
                   registration_.entries().size());
    registration_ = {};
    draining_ = closing_ = false;
  }

  struct Recording {
    std::string path;
    std::shared_ptr<Recorder> recorder;
  };

  void StartRecording(const Recording& recording,
                      const std::vector<ffmpeg::StreamInfo>& streams) const {
    // Recorder reports file errors through its own status. Do not forward its
    // optional OnError to the session's fatal error handler.
    if (!recording.recorder->Start(
            streams,
            RecorderConfig{recording.path, config_.recording_queue_bytes}))
      MW_LOG_WARNING("streamer", "Remuxer 无法启动录像线程: {}",
                     recording.path);
  }

  template <typename Action>
  static void Sync(const toolkit::EventPoller::Ptr& poller, Action&& action) {
    std::exception_ptr error;
    poller->sync([&] {
      try {
        action();
      } catch (...) {
        error = std::current_exception();
      }
    });
    if (error) std::rethrow_exception(error);
  }

  bool RequireRegisterable() {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((running_ && !accepting_) || draining_ || closing_)
      throw std::logic_error("Remuxer 正在排空");
    return running_;
  }
  std::string Register(internal::OutputRegistration::Entry entry) {
    const bool active = RequireRegisterable();
    auto registration = registration_;
    registration.Add(entry);
    auto info = registration.info();
    if (active) {
      if (entry.kind == internal::OutputRegistration::Kind::kRecording) {
        Recording recording{entry.actual, std::make_shared<Recorder>()};
        StartRecording(recording, streams_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) throw std::logic_error("Remuxer 未启动或已经排空");
        recordings_.push_back(std::move(recording));
      } else {
        Sync(poller_, [&] {
          RequireAccepting();
          output_->AddPrepared(entry);
          record_only_ = false;
        });
      }
    }
    registration_ = std::move(registration);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      output_info_ = std::move(info);
    }
    MW_LOG_DEBUG("streamer",
                 "Remuxer 添加输出: instance={} kind={} active={} outputs={}",
                 static_cast<const void*>(this), static_cast<int>(entry.kind),
                 active, registration_.entries().size());
    return entry.actual;
  }
  void RequireAccepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) throw std::logic_error("Remuxer 未启动或已经排空");
  }
  void Error(std::exception_ptr error) {
    OnError callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) return;
      if (!error_reported_) {
        error_reported_ = true;
        accepting_ = false;
        draining_ = true;
        writable_.notify_all();
        if (!closing_) Schedule();
        callback = on_error_;
      }
    }
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer", "Remuxer error: {}", failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "Remuxer error: unknown exception");
    }
    if (callback) {
      try {
        callback(error);
      } catch (const std::exception& failure) {
        MW_LOG_ERROR("streamer", "Remuxer OnError callback failed: {}",
                     failure.what());
      } catch (...) {
        MW_LOG_ERROR("streamer",
                     "Remuxer OnError callback failed: unknown exception");
      }
    }
  }

  // Admission has one packet slot; media interleaving is separate so waiting
  // for the other track can never block that track's next encoded packet.
  void Schedule() {
    if (scheduled_ || closing_) return;
    scheduled_ = true;
    auto self = shared_from_this();
    poller_->async([self] { self->Process(); }, false);
  }

  void Process() {
    bool finish = false;
    try {
      std::vector<internal::PacketInterleaver::Item> packets;
      std::vector<ffmpeg::Packet> first_packets;
      std::vector<Recording> recordings;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || closing_) {
          scheduled_ = false;
          return;
        }
        if (handoff_) {
          interleaver_->Push(handoff_->packet);
          MW_LOG_TRACE("streamer",
                       "Remuxer 交接到交织队列: instance={} stream={} pts={} "
                       "dts={} time_base={}/{} interleaved_queue={}",
                       static_cast<const void*>(this),
                       handoff_->packet->stream_index, handoff_->packet->pts,
                       handoff_->packet->dts, handoff_->packet->time_base.num,
                       handoff_->packet->time_base.den,
                       interleaver_->queued_packets());
          handoff_.reset();
          writable_.notify_all();
        }
        if (!timestamps_->initialized())
          first_packets = interleaver_->FirstPackets();
        packets = interleaver_->PopReady(draining_);
        MW_LOG_TRACE(
            "streamer",
            "Remuxer 取出就绪包: instance={} ready={} remaining={} draining={}",
            static_cast<const void*>(this), packets.size(),
            interleaver_->queued_packets(), draining_);
        scheduled_ = false;
        finish = draining_;
        recordings = recordings_;
      }
      if (!packets.empty() && !timestamps_->initialized())
        timestamps_->Initialize(first_packets);
      std::set<int> eof_tracks;
      if (finish && !record_only_ && !output_->tracks_ready()) {
        for (const auto& packet : packets)
          eof_tracks.insert(packet.packet->stream_index);
      }
      for (const auto& packet : packets) {
        for (const auto& recording : recordings)
          recording.recorder->TrySubmitPacket(packet.packet);
        // Local recording retains the original packet without protocol NAL
        // validation or adaptation. Dynamic protocol outputs resume conversion.
        if (record_only_) continue;
        const auto convert_started = convert_latency_.Begin();
        const auto timestamp = timestamps_->Convert(packet.packet);
        const auto dts_ms = timestamp.dts_ms;
        const auto pts_ms = timestamp.pts_ms;
        auto frame = converter_->Convert(packet.packet, dts_ms, pts_ms);
        convert_latency_.End(convert_started);
        if (!frame) continue;
        const auto output_started = output_latency_.Begin();
        output_->InputFrame(frame);
        // This measures synchronous dispatch, not asynchronous network writes.
        output_latency_.End(output_started);
        MW_LOG_TRACE("streamer",
                     "Remuxer 完成输出: instance={} stream={} pts={} dts={} "
                     "time_base={}/{} size={} pts_ms={} dts_ms={} recorders={}",
                     static_cast<const void*>(this),
                     packet.packet->stream_index, packet.packet->pts,
                     packet.packet->dts, packet.packet->time_base.num,
                     packet.packet->time_base.den, packet.packet->size, pts_ms,
                     dts_ms, recordings.size());
        if (!eof_tracks.empty()) {
          eof_tracks.erase(packet.packet->stream_index);
          // EOF identifies which declared tracks actually have packets.
          // Finalize them after their first frames, before ZLM's unready cache
          // fills.
          if (eof_tracks.empty() && !output_->tracks_ready()) output_->Flush();
        }
      }
    } catch (...) {
      Error(std::current_exception());
      finish = true;
    }
    if (finish) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        closing_ = true;
        writable_.notify_all();
      }
      try {
        if (!record_only_) output_->Flush();
      } catch (...) {
        Error(std::current_exception());
      }
      auto self = shared_from_this();
      // Allow already queued protocol dispatches to run before teardown.
      poller_->async([self] { self->Finish(); }, false);
    }
  }

  void Finish() {
    MW_LOG_DEBUG("streamer", "Remuxer 关闭协议输出: instance={}",
                 static_cast<const void*>(this));
    try {
      output_->Close();
    } catch (...) {
      Error(std::current_exception());
    }
    convert_latency_.Finish();
    output_latency_.Finish();
    output_.reset();
    converter_.reset();
    timestamps_.reset();
    record_only_ = false;
    // No further additions are accepted once closing_. This only requests
    // draining; file finalization and errors remain on each recorder thread.
    for (const auto& recording : recordings_) recording.recorder->Drain();
    auto self = shared_from_this();
    // Wait behind the pusher teardown tasks queued on this same poller.
    poller_->async(
        [self] {
          {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->handoff_.reset();
            self->interleaver_.reset();
            self->running_ = self->scheduled_ = false;
          }
          MW_LOG_INFO("streamer", "Remuxer 已停止: instance={}",
                      static_cast<const void*>(self.get()));
          self->stopped_.notify_all();
          self->writable_.notify_all();
        },
        false);
  }

  const RemuxerConfig config_;
  std::mutex control_;
  mutable std::mutex mutex_;
  internal::OutputRegistration registration_;
  Remuxer::OutputInfo output_info_;
  std::vector<Recording> recordings_;
  std::vector<ffmpeg::StreamInfo> streams_;
  std::condition_variable stopped_;
  std::condition_variable writable_;
  std::optional<internal::PacketInterleaver::Item> handoff_;
  bool running_ = false;
  bool accepting_ = false;
  bool draining_ = false;
  bool closing_ = false;
  bool scheduled_ = false;
  bool error_reported_ = false;
  // Accessed only on the output poller, including startup and registration.
  bool record_only_ = false;
  OnError on_error_;
  toolkit::EventPoller::Ptr poller_;
  std::shared_ptr<internal::RemuxOutput> output_;
  std::unique_ptr<internal::PacketInterleaver> interleaver_;
  std::unique_ptr<internal::PacketConverter> converter_;
  std::unique_ptr<internal::MediaTimestampConverter> timestamps_;
  internal::StageLatency convert_latency_;
  internal::StageLatency output_latency_;
};

Remuxer::Remuxer() : Remuxer(RemuxerConfig{}) {}
Remuxer::Remuxer(RemuxerConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {}
Remuxer::~Remuxer() { Stop(); }
void Remuxer::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
bool Remuxer::Start(const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
  try {
    impl_->Start(streams);
    return true;
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "Remuxer 启动失败: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "Remuxer 启动失败: 未知异常");
  }
  return false;
}
bool Remuxer::SubmitPacket(const ffmpeg::Packet& packet) {
  return impl_->SubmitPacket(packet);
}
std::string Remuxer::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void Remuxer::AddRtspPublish(const std::string& ip, std::uint16_t port) {
  AddRtspPublish(ip, port, false);
}
void Remuxer::AddRtspPublish(const std::string& ip, std::uint16_t port,
                             bool tls) {
  impl_->AddRtspPublish(ip, port, tls);
}
std::string Remuxer::AddHlsPublish(const std::string& ip, std::uint16_t port) {
  return AddHlsPublish(ip, port, false);
}
std::string Remuxer::AddHlsPublish(const std::string& ip, std::uint16_t port,
                                   bool tls) {
  return impl_->AddHlsPublish(ip, port, tls);
}
void Remuxer::Drain() noexcept { impl_->Drain(); }
void Remuxer::StopProtocols() noexcept { impl_->StopProtocols(); }
void Remuxer::Stop() noexcept { impl_->Stop(); }
Remuxer::OutputInfo Remuxer::output_info() const {
  return impl_->output_info();
}

std::vector<Remuxer::PushStatus> Remuxer::push_statuses() const {
  return impl_->push_statuses();
}

std::vector<Remuxer::RecordingStatus> Remuxer::recording_statuses() const {
  return impl_->recording_statuses();
}

}  // namespace mw::streamer
