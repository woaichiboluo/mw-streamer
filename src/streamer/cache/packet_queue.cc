#include "mw/streamer/cache/packet_queue.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/streamer/common/blocking_queue.h"
#include "mw/streamer/common/thread.h"

namespace mw::streamer {

class PacketQueue::Impl final {
 public:
  Impl(std::chrono::milliseconds cache_duration, Sink& consumer)
      : consumer_(consumer),
        cache_duration_(ValidateDuration(cache_duration)) {}

  ~Impl() { Stop(); }

  void OnStreamsReady(const StreamsReady& streams) {
    if (!aborted_.load()) {
      Work work;
      work.kind = WorkKind::kStreams;
      work.streams = streams;
      Submit(std::move(work));
    }
  }

  void OnPacket(const PacketReady& packet) {
    if (!aborted_.load()) {
      Work work;
      work.kind = WorkKind::kPacket;
      work.packet = packet;
      Submit(std::move(work));
    }
  }

  void OnTimelineReset(const TimelineReset& reset) {
    if (!aborted_.load()) {
      Work work;
      work.kind = WorkKind::kReset;
      work.reset = reset;
      Submit(std::move(work));
    }
  }

  void OnInputEnded(const StreamEnded& end) {
    if (!aborted_.load()) {
      Work work;
      work.kind = WorkKind::kEnd;
      work.end = end;
      Submit(std::move(work));
    }
  }

  void Abort() noexcept {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      aborted_.store(true);
      if (state_.load() != PacketSinkState::kFailed) {
        state_.store(PacketSinkState::kStopped);
      }
    }
    input_.Close();
    input_.Clear();
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    Abort();
    if (worker_) {
      worker_->Join();
    }
  }

  PacketSinkState state() const noexcept { return state_.load(); }

  std::uint64_t generation() const noexcept {
    return generation_snapshot_.load();
  }

  std::string error() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return error_;
  }

 private:
  using Clock = std::chrono::steady_clock;

  static std::chrono::microseconds ValidateDuration(
      std::chrono::milliseconds duration) {
    if (duration.count() != 0 && (duration < std::chrono::seconds(1) ||
                                  duration > std::chrono::seconds(30))) {
      throw std::invalid_argument("缓存时长必须为0或1至30秒");
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(duration);
  }

  enum class WorkKind { kStreams, kPacket, kReset, kEnd };

  struct Work {
    WorkKind kind = WorkKind::kPacket;
    std::optional<StreamsReady> streams;
    std::optional<PacketReady> packet;
    std::optional<TimelineReset> reset;
    std::optional<StreamEnded> end;
  };

  void Submit(Work work) {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (aborted_.load()) {
      return;
    }
    if (!worker_) {
      worker_ = std::make_unique<Thread>("mw-packet-cache",
                                                 [this]() { Run(); });
    }
    input_.Push(std::move(work));
  }

  struct CachedPacket {
    Packet packet;
    std::int64_t dts_us;
  };

  struct Track {
    int stream_index = -1;
    AVRational time_base{0, 1};
    std::optional<std::int64_t> latest_dts;
    std::deque<CachedPacket> packets;

    bool configured() const { return stream_index >= 0; }
  };

  void Run() noexcept {
    try {
      while (!aborted_.load()) {
        RefreshPlayback();
        if (!WaitAndProcessInput()) {
          break;
        }
        RefreshPlayback();
        OutputDuePacket();
        // At most one input command and one due packet per iteration keeps
        // both control traffic and playback moving under continuous input.
      }
    } catch (const std::exception& error) {
      Fail(error.what());
    } catch (...) {
      Fail("PacketQueue发生未知异常");
    }
    audio_.packets.clear();
    video_.packets.clear();
  }

  // A deadline wakeup needs no input command; it lets Run release a due packet.
  bool WaitAndProcessInput() {
    if (aborted_.load()) {
      return false;
    }
    const auto deadline = Deadline();
    auto work = deadline ? input_.WaitPopUntil(*deadline) : input_.WaitPop();
    if (aborted_.load()) {
      return false;
    }
    if (work) {
      Handle(std::move(*work));
    }
    return !aborted_.load();
  }

  void Handle(Work work) {
    switch (work.kind) {
      case WorkKind::kStreams:
        Configure(*work.streams);
        break;
      case WorkKind::kPacket:
        AcceptPacket(std::move(*work.packet));
        break;
      case WorkKind::kReset:
        Reset(*work.reset);
        break;
      case WorkKind::kEnd:
        End(*work.end);
        break;
    }
  }

  void Configure(const StreamsReady& streams) {
    if (streams.generation == 0 || streams.generation <= generation_ ||
        (pending_reset_ && streams.generation < pending_reset_->generation)) {
      return;
    }
    if (generation_ != 0 &&
        (!pending_reset_ || pending_reset_->generation != streams.generation)) {
      throw std::logic_error("新输入代次必须先投递时间线重置");
    }
    Track audio;
    Track video;
    for (const auto& stream : streams.streams) {
      const auto* parameters = stream.codec_parameters.get();
      if (!parameters) {
        throw std::invalid_argument("StreamInfo参数无效");
      }
      if (parameters->codec_type != AVMEDIA_TYPE_AUDIO &&
          parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
        continue;
      }
      stream.Validate();
      auto& track =
          parameters->codec_type == AVMEDIA_TYPE_AUDIO ? audio : video;
      if (track.configured()) {
        throw std::invalid_argument("PacketQueue最多支持一路音频和一路视频");
      }
      track.stream_index = stream.stream_index;
      track.time_base = stream.time_base;
    }
    if ((!audio.configured() && !video.configured()) ||
        (audio.configured() && video.configured() &&
         audio.stream_index == video.stream_index)) {
      throw std::invalid_argument("PacketQueue音视频轨道配置无效");
    }
    audio_ = std::move(audio);
    video_ = std::move(video);
    generation_ = streams.generation;
    pending_reset_.reset();
    end_.reset();
    drained_ = false;
    playing_ = false;
    if (Publish(PacketSinkState::kRunning, streams.generation)) {
      consumer_generation_ = streams.generation;
      consumer_.OnStreamsReady(streams);
    }
  }

  void Reset(const TimelineReset& reset) {
    if (reset.generation <= generation_ ||
        (pending_reset_ && reset.generation <= pending_reset_->generation)) {
      return;
    }
    audio_.packets.clear();
    video_.packets.clear();
    playing_ = false;
    end_.reset();
    pending_reset_ = reset;
    if (Publish(PacketSinkState::kRunning, reset.generation)) {
      consumer_.OnTimelineReset(reset);
    }
  }

  Track* FindTrack(int stream_index) {
    if (audio_.configured() && audio_.stream_index == stream_index) {
      return &audio_;
    }
    if (video_.configured() && video_.stream_index == stream_index) {
      return &video_;
    }
    return nullptr;
  }

  void AcceptPacket(PacketReady packet) {
    const auto* raw = packet.packet.get();
    if (packet.generation != generation_ || generation_ == 0 ||
        pending_reset_ || end_ || !raw || raw->dts == AV_NOPTS_VALUE) {
      return;
    }
    auto* track = FindTrack(raw->stream_index);
    if (!track || (track->latest_dts && raw->dts < *track->latest_dts)) {
      return;
    }
    track->latest_dts = raw->dts;
    if (cache_duration_.count() == 0) {
      consumer_.OnPacket(packet);
    } else {
      const auto dts_us =
          av_rescale_q(raw->dts, track->time_base, AVRational{1, 1000000});
      track->packets.push_back({std::move(packet.packet), dts_us});
    }
  }

  void End(const StreamEnded& end) {
    if (end.generation != generation_ || generation_ == 0 || pending_reset_ ||
        end_) {
      return;
    }
    end_ = end;
    Publish(PacketSinkState::kDraining, end.generation);
    if (end.reason == StreamEndReason::kStopped ||
        end.reason == StreamEndReason::kFailed) {
      audio_.packets.clear();
      video_.packets.clear();
      playing_ = false;
    }
  }

  bool TrackReady(const Track& track) const {
    return !track.configured() ||
           (track.packets.size() >= 2 &&
            static_cast<long double>(track.packets.back().dts_us) -
                    track.packets.front().dts_us >=
                cache_duration_.count());
  }

  static void TrimLeading(Track& track, std::int64_t common_dts_us) {
    while (track.packets.size() > 1 &&
           track.packets[1].dts_us <= common_dts_us) {
      track.packets.pop_front();
    }
  }

  void AlignTracks() {
    if (audio_.packets.empty() || video_.packets.empty()) {
      return;
    }
    const auto common_dts_us =
        std::max(audio_.packets.front().dts_us, video_.packets.front().dts_us);
    TrimLeading(audio_, common_dts_us);
    TrimLeading(video_, common_dts_us);
  }

  Track* NextTrack() {
    if (audio_.packets.empty()) {
      return !video_.packets.empty() && (!audio_.configured() || end_)
                 ? &video_
                 : nullptr;
    }
    if (video_.packets.empty()) {
      return !video_.configured() || end_ ? &audio_ : nullptr;
    }
    const auto audio_dts = audio_.packets.front().dts_us;
    const auto video_dts = video_.packets.front().dts_us;
    if (audio_dts == video_dts) {
      return audio_.stream_index < video_.stream_index ? &audio_ : &video_;
    }
    return audio_dts < video_dts ? &audio_ : &video_;
  }

  void RefreshPlayback() {
    if (pending_reset_ || generation_ == 0) {
      return;
    }
    auto* next = NextTrack();
    if (!next) {
      // Temporary starvation must not silently move the generation clock.
      // Downstream real-time scheduling uses the same source timeline.
      CompleteInput();
      return;
    }
    if (!playing_ && (end_ || (TrackReady(audio_) && TrackReady(video_)))) {
      AlignTracks();
      clock_media_us_ = NextTrack()->packets.front().dts_us;
      clock_wall_ = Clock::now();
      playing_ = true;
    }
  }

  void CompleteInput() {
    if (!end_ || drained_) {
      return;
    }
    drained_ = true;
    if (aborted_.load()) {
      return;
    }
    consumer_generation_.reset();
    consumer_.OnInputEnded(*end_);
    Publish(PacketSinkState::kEnded, generation_);
  }

  std::optional<Clock::time_point> Deadline() {
    auto* next = NextTrack();
    if (!playing_ || !next) {
      return std::nullopt;
    }
    // Saturate before converting media microseconds into the clock's duration;
    // a distant timestamp must remain interruptible rather than overflow.
    const auto offset_us =
        static_cast<long double>(next->packets.front().dts_us) -
        clock_media_us_;
    const auto maximum_us = std::chrono::duration<long double, std::micro>(
                                Clock::time_point::max() - clock_wall_)
                                .count();
    if (offset_us >= maximum_us) {
      return Clock::time_point::max();
    }
    return clock_wall_ + std::chrono::duration_cast<Clock::duration>(
                             std::chrono::duration<long double, std::micro>(
                                 std::max<long double>(0, offset_us)));
  }

  void OutputDuePacket() {
    const auto deadline = Deadline();
    if (!deadline || *deadline > Clock::now()) {
      return;
    }
    auto* track = NextTrack();
    auto packet = std::move(track->packets.front().packet);
    track->packets.pop_front();
    consumer_.OnPacket({generation_, std::move(packet)});
  }

  bool Publish(PacketSinkState state, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (aborted_.load()) {
      return false;
    }
    generation_snapshot_.store(generation);
    state_.store(state);
    return true;
  }

  void Fail(const char* error) noexcept {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      if (state_.load() != PacketSinkState::kFailed) {
        error_ = error;
        state_.store(PacketSinkState::kFailed);
      }
      aborted_.store(true);
    }
    input_.Close();
    input_.Clear();
    if (consumer_generation_) {
      const StreamEnded end{*consumer_generation_,
                                   StreamEndReason::kFailed};
      consumer_generation_.reset();
      consumer_.OnInputEnded(end);
    }
  }

  Sink& consumer_;
  const std::chrono::microseconds cache_duration_;
  BlockingQueue<Work> input_;
  std::atomic<bool> aborted_{false};
  std::mutex stop_mutex_;
  mutable std::mutex status_mutex_;
  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
  std::atomic<std::uint64_t> generation_snapshot_{0};
  std::string error_;
  // Scheduler-thread state; producers only touch input_ and aborted_.
  Track audio_;
  Track video_;
  std::uint64_t generation_ = 0;
  std::optional<std::uint64_t> consumer_generation_;
  std::optional<TimelineReset> pending_reset_;
  std::optional<StreamEnded> end_;
  bool drained_ = false;
  bool playing_ = false;
  std::int64_t clock_media_us_ = 0;
  Clock::time_point clock_wall_;
  std::unique_ptr<Thread> worker_;
};

PacketQueue::PacketQueue(std::chrono::milliseconds cache_duration,
                         Sink& consumer)
    : impl_(std::make_unique<Impl>(cache_duration, consumer)) {}

PacketQueue::~PacketQueue() = default;

void PacketQueue::OnStreamsReady(const StreamsReady& streams) {
  impl_->OnStreamsReady(streams);
}

void PacketQueue::OnPacket(const PacketReady& packet) {
  impl_->OnPacket(packet);
}

void PacketQueue::OnTimelineReset(const TimelineReset& reset) {
  impl_->OnTimelineReset(reset);
}

void PacketQueue::OnInputEnded(const StreamEnded& end) {
  impl_->OnInputEnded(end);
}

void PacketQueue::Abort() noexcept { impl_->Abort(); }

void PacketQueue::Stop() noexcept { impl_->Stop(); }

PacketSinkState PacketQueue::state() const noexcept {
  return impl_->state();
}

std::uint64_t PacketQueue::generation() const noexcept {
  return impl_->generation();
}

std::string PacketQueue::error() const { return impl_->error(); }

}  // namespace mw::streamer
