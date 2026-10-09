#include "mw/streamer/remuxer/sync_remuxer.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include "Common/config.h"
#include "Poller/EventPoller.h"
#include "mw/log.h"
#include "mw/streamer/remuxer/packet_converter.h"
#include "mw/streamer/remuxer/remux_output.h"

namespace mw::streamer {
namespace {

constexpr AVRational kNanoseconds{1, 1000000000};
constexpr AVRational kMilliseconds{1, 1000};

std::int64_t Add(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    throw std::invalid_argument("输入时间线溢出");
  }
  return left + right;
}

std::int64_t Offset(std::int64_t value, std::int64_t origin) {
  if (value < origin ||
      (origin < 0 &&
       value > std::numeric_limits<std::int64_t>::max() + origin)) {
    throw std::invalid_argument("输入时间戳早于共同起点或溢出");
  }
  return value - origin;
}

std::int64_t Nanoseconds(std::int64_t value, AVRational time_base) {
  const auto result = av_rescale_q(value, time_base, kNanoseconds);
  if (result == std::numeric_limits<std::int64_t>::min() ||
      result == std::numeric_limits<std::int64_t>::max()) {
    throw std::invalid_argument("输入时间戳无法换算");
  }
  return result;
}

template <typename Action>
void Sync(const toolkit::EventPoller::Ptr& poller, Action&& action) {
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

}  // namespace

class SyncRemuxer::Impl final : public std::enable_shared_from_this<Impl> {
 public:
  explicit Impl(SyncRemuxerConfig config) : config_(config) {
    if (!config_.max_pending_bytes)
      throw std::invalid_argument("SyncRemuxer缓存容量必须大于零");
  }

  void SetOnError(OnError callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(callback);
  }
  void SetOnEnded(OnEnded callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_ended_ = std::move(callback);
  }

  void Start(const std::vector<ffmpeg::StreamInfo>& streams) {
    std::lock_guard<std::mutex> control(control_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_) throw std::logic_error("SyncRemuxer已经启动");
    }
    if (worker_.joinable()) worker_.join();
    if (streams.empty()) throw std::invalid_argument("输入录像轨道不能为空");
    std::vector<Track> tracks;
    for (const auto& stream : streams) {
      stream.Validate();
      const auto& parameters = *stream.codec_parameters.get();
      if (std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) {
            return track.index == stream.stream_index ||
                   track.type == parameters.codec_type;
          })) {
        throw std::invalid_argument("输入录像只支持一条视频和一条音频轨道");
      }
      tracks.push_back({stream.stream_index, parameters.codec_type,
                        stream.time_base, parameters.framerate,
                        parameters.sample_rate, std::nullopt, false});
    }
    auto converter = std::make_unique<internal::PacketConverter>(streams);
    std::vector<mediakit::Track::Ptr> startup_probes;
    for (const auto& track : converter->tracks()) {
      startup_probes.push_back(track->clone());
      const auto index = startup_probes.size() - 1;
      startup_probes.back()->addDelegate(
          [this, index](const mediakit::Frame::Ptr&) {
            ++unready_frames_[index];
            return true;
          });
    }
    auto poller =
        poller_ ? poller_ : toolkit::EventPollerPool::Instance().getPoller();
    std::weak_ptr<Impl> weak = shared_from_this();
    auto output = std::make_shared<internal::RemuxOutput>(
        poller,
        [weak](std::string_view target, int code, std::string_view message) {
          if (auto self = weak.lock()) self->ReportError(target, code, message);
        });
    Sync(poller, [&] { output->Start(converter->tracks()); });
    {
      std::lock_guard<std::mutex> lock(mutex_);
      poller_ = std::move(poller);
      output_ = std::move(output);
      converter_ = std::move(converter);
      startup_probes_ = std::move(startup_probes);
      unready_frames_.assign(startup_probes_.size(), 0);
      tracks_ = std::move(tracks);
      queue_.clear();
      startup_.clear();
      pending_bytes_ = 0;
      ++session_id_;
      producer_generation_.reset();
      generation_.reset();
      origin_.reset();
      base_ns_ = end_ns_ = 0;
      failure_.clear();
      draining_ = processing_ = false;
      startup_waiting_ = true;
      running_ = accepting_ = true;
    }
    try {
      worker_ = std::thread([self = shared_from_this()] { self->Run(); });
    } catch (...) {
      Sync(poller_, [&] { output_->Close(); });
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = accepting_ = false;
      output_.reset();
      converter_.reset();
      throw;
    }
  }

  bool SubmitPacket(std::uint64_t generation, const AVPacket& packet) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!accepting_) return false;
    const auto session_id = session_id_;
    auto track = std::find_if(
        tracks_.begin(), tracks_.end(),
        [&](const auto& t) { return t.index == packet.stream_index; });
    if (track == tracks_.end() || !packet.data || packet.size <= 0 ||
        packet.pts == AV_NOPTS_VALUE || packet.dts == AV_NOPTS_VALUE ||
        packet.duration < 0) {
      throw std::invalid_argument("输入录像需要有效轨道、负载和PTS/DTS");
    }
    if (packet.time_base.num < 0 || packet.time_base.den < 0 ||
        (packet.time_base.num > 0 && packet.time_base.den == 0)) {
      throw std::invalid_argument("输入录像包时间基无效");
    }
    const auto time_base =
        packet.time_base.num == 0 ? track->time_base : packet.time_base;
    if (av_cmp_q(time_base, track->time_base) != 0) {
      throw std::invalid_argument("输入录像包与轨道时间基不匹配");
    }
    if (producer_generation_ && generation < *producer_generation_)
      throw std::invalid_argument("输入录像generation不得倒退");
    if (producer_generation_ == generation && track->last_dts &&
        packet.dts <= *track->last_dts) {
      throw std::invalid_argument("输入录像同轨DTS必须严格递增");
    }
    const auto pts_ns = Nanoseconds(packet.pts, time_base);
    const auto dts_ns = Nanoseconds(packet.dts, time_base);
    std::int64_t duration_ns = 0;
    if (packet.duration > 0) {
      duration_ns = Nanoseconds(packet.duration, time_base);
    } else if (track->type == AVMEDIA_TYPE_VIDEO && track->frame_rate.num > 0 &&
               track->frame_rate.den > 0) {
      duration_ns = Nanoseconds(1, av_inv_q(track->frame_rate));
    } else if (track->type == AVMEDIA_TYPE_AUDIO && track->sample_rate > 0) {
      duration_ns = Nanoseconds(1024, {1, track->sample_rate});
    }
    if (duration_ns <= 0)
      throw std::invalid_argument("输入录像包缺少有效时长或帧率");
    std::size_t bytes = static_cast<std::size_t>(packet.size);
    for (int index = 0; index < packet.side_data_elems; ++index) {
      const auto size = packet.side_data[index].size;
      if (size > config_.max_pending_bytes ||
          bytes > config_.max_pending_bytes - size) {
        throw std::length_error("输入录像单包超过缓存容量");
      }
      bytes += size;
    }
    if (bytes > config_.max_pending_bytes)
      throw std::length_error("输入录像单包超过缓存容量");
    while (accepting_ && session_id_ == session_id &&
           bytes > config_.max_pending_bytes - pending_bytes_) {
      // With no consumer work left, only startup data retains the budget.
      // The next packet cannot enter to make startup progress.
      if (startup_waiting_ && queue_.empty() && !processing_) {
        failure_ = "输入录像启动缓存不足以等待各轨首包及视频关键帧";
        accepting_ = false;
        draining_ = true;
        changed_.notify_all();
        return false;
      }
      changed_.wait(lock);
    }
    if (!accepting_ || session_id_ != session_id) return false;
    auto retained = ffmpeg::Packet::Clone(packet);
    retained->time_base = time_base;
    queue_.push_back({std::move(retained), generation,
                      static_cast<std::size_t>(track - tracks_.begin()), bytes,
                      pts_ns, dts_ns, duration_ns});
    pending_bytes_ += bytes;
    if (producer_generation_ != generation) {
      producer_generation_ = generation;
      for (auto& value : tracks_) value.last_dts.reset();
    }
    track->last_dts = packet.dts;
    changed_.notify_all();
    return true;
  }

  std::string AddPushUrl(const std::string& url) {
    std::lock_guard<std::mutex> control(control_);
    RequireAccepting();
    std::string result;
    Sync(poller_, [&] {
      RequireAccepting();
      result = output_->AddPushUrl(url, true);
    });
    return result;
  }

  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port) {
    std::lock_guard<std::mutex> control(control_);
    RequireAccepting();
    Sync(poller_, [&] {
      RequireAccepting();
      output_->AddRtspPublish(app, stream, ip, port);
    });
  }

  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port) {
    std::lock_guard<std::mutex> control(control_);
    RequireAccepting();
    std::string result;
    Sync(poller_, [&] {
      RequireAccepting();
      result = output_->AddHlsPublish(app, stream, ip, port);
    });
    return result;
  }

  void Drain() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    accepting_ = false;
    draining_ = true;
    changed_.notify_all();
  }

  void Stop() noexcept {
    // Cancellation must precede waiting for the worker/control mutex.
    Drain();
    std::lock_guard<std::mutex> control(control_);
    if (worker_.joinable()) worker_.join();
  }

 private:
  struct Track {
    int index;
    AVMediaType type;
    AVRational time_base;
    AVRational frame_rate;
    int sample_rate;
    std::optional<std::int64_t> last_dts;
    bool seen;
  };

  struct Item {
    ffmpeg::Packet packet;
    std::uint64_t generation;
    std::size_t track;
    std::size_t bytes;
    std::int64_t pts_ns;
    std::int64_t dts_ns;
    std::int64_t duration_ns;
  };

  void RequireAccepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) throw std::logic_error("SyncRemuxer未启动或已经排空");
  }

  void ReportError(std::string_view target, int code,
                   std::string_view message) {
    OnError callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (target.empty()) {
        accepting_ = false;
        draining_ = true;
        changed_.notify_all();
      }
      callback = on_error_;
    }
    MW_LOG_ERROR("streamer", "SyncRemuxer error: target={} code={} {}", target,
                 code, message);
    if (callback) callback(target, code, message);
  }

  void Release(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_bytes_ -= bytes;
    changed_.notify_all();
  }

  void Emit(const Item& item) {
    const auto pts = Add(base_ns_, Offset(item.pts_ns, *origin_));
    const auto dts = Add(base_ns_, Offset(item.dts_ns, *origin_));
    const auto end = Add(std::max(pts, dts), item.duration_ns);
    const auto pts_ms = static_cast<std::uint64_t>(
        av_rescale_q(pts, kNanoseconds, kMilliseconds));
    const auto dts_ms = static_cast<std::uint64_t>(
        av_rescale_q(dts, kNanoseconds, kMilliseconds));
    Sync(poller_, [&] {
      auto frame = converter_->Convert(item.packet, dts_ms, pts_ms);
      if (!output_->tracks_ready()) {
        // ZLM counts parsed subframes, including inserted parameter sets.
        // Probe the same track parser before its finite cache can discard data.
        auto probe_frame = frame;
        startup_probes_[item.track]->inputFrame(probe_frame);
        const auto limit =
            toolkit::mINI::Instance()[mediakit::General::kUnreadyFrameCache]
                .as<std::uint32_t>();
        if (unready_frames_[item.track] > static_cast<std::size_t>(limit) + 1) {
          throw std::length_error("输入录像超过封装层未就绪轨道缓存容量");
        }
      }
      output_->InputFrame(frame);
    });
    end_ns_ = std::max(end_ns_, end);
  }

  void EmitAndRelease(Item& item) {
    Emit(item);
    item.packet.Unref();
    Release(item.bytes);
  }

  void Initialize(bool force, bool final_eof = false) {
    if (origin_ || startup_.empty()) return;
    if (!force && std::any_of(tracks_.begin(), tracks_.end(),
                              [](const auto& track) { return !track.seen; })) {
      return;
    }
    std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
    std::set<std::size_t> actual_tracks;
    for (const auto& item : startup_) {
      minimum = std::min({minimum, item.pts_ns, item.dts_ns});
      actual_tracks.insert(item.track);
    }
    origin_ = minimum;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      startup_waiting_ = false;
      changed_.notify_all();
    }
    // Prime each actual track before sending a long startup prefix. This only
    // changes cross-track order; each track still sees its own first packet
    // first, and ZLM preserves the shared timestamps when draining its cache.
    while (!actual_tracks.empty()) {
      const auto first = std::find_if(
          startup_.begin(), startup_.end(),
          [&](const auto& item) { return actual_tracks.count(item.track); });
      auto item = std::move(*first);
      startup_.erase(first);
      actual_tracks.erase(item.track);
      EmitAndRelease(item);
    }
    if (final_eof) {
      Sync(poller_, [&] {
        if (!output_->tracks_ready()) output_->Flush();
      });
    }
    while (!startup_.empty()) {
      auto item = std::move(startup_.front());
      startup_.pop_front();
      EmitAndRelease(item);
    }
  }

  void Process(Item item) {
    if (generation_ != item.generation) {
      Initialize(true);
      base_ns_ = end_ns_;
      origin_.reset();
      generation_ = item.generation;
      for (auto& track : tracks_) track.seen = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        startup_waiting_ = true;
      }
    }
    auto& track = tracks_[item.track];
    if (track.type == AVMEDIA_TYPE_VIDEO && !track.seen &&
        !(item.packet->flags & AV_PKT_FLAG_KEY)) {
      item.packet.Unref();
      Release(item.bytes);
      return;
    }
    track.seen = true;
    if (origin_) {
      EmitAndRelease(item);
    } else {
      startup_.push_back(std::move(item));
      Initialize(false);
    }
  }

  void Run() noexcept {
    try {
      while (true) {
        std::optional<Item> item;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          changed_.wait(lock, [&] {
            return !queue_.empty() || draining_ || !failure_.empty();
          });
          if (!failure_.empty()) throw std::runtime_error(failure_);
          if (queue_.empty()) break;
          item.emplace(std::move(queue_.front()));
          queue_.pop_front();
          processing_ = true;
        }
        Process(std::move(*item));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          processing_ = false;
          changed_.notify_all();
        }
      }
      Initialize(true, true);
    } catch (const std::exception& error) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        changed_.notify_all();
      }
      ReportError({}, AVERROR_EXTERNAL, error.what());
    }
    try {
      Sync(poller_, [&] { output_->Flush(); });
    } catch (const std::exception& error) {
      ReportError({}, AVERROR_EXTERNAL, error.what());
    }
    try {
      // Separate sync calls let ring dispatches queued by Flush run first.
      Sync(poller_, [&] { output_->Close(); });
    } catch (const std::exception& error) {
      ReportError({}, AVERROR_EXTERNAL, error.what());
    }
    // Wait behind output teardown before signaling the session completion.
    Sync(poller_, [] {});
    OnEnded callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.clear();
      startup_.clear();
      pending_bytes_ = 0;
      accepting_ = processing_ = false;
      callback = on_ended_;
      changed_.notify_all();
    }
    if (callback) callback();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
  }

  const SyncRemuxerConfig config_;
  std::mutex control_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool running_ = false;
  bool accepting_ = false;
  bool draining_ = false;
  bool processing_ = false;
  bool startup_waiting_ = true;
  std::size_t pending_bytes_ = 0;
  std::uint64_t session_id_ = 0;
  std::string failure_;
  std::optional<std::uint64_t> producer_generation_;
  OnError on_error_;
  OnEnded on_ended_;
  std::thread worker_;
  toolkit::EventPoller::Ptr poller_;
  std::shared_ptr<internal::RemuxOutput> output_;
  std::unique_ptr<internal::PacketConverter> converter_;
  std::vector<mediakit::Track::Ptr> startup_probes_;
  std::vector<std::size_t> unready_frames_;
  std::vector<Track> tracks_;
  std::deque<Item> queue_;
  std::deque<Item> startup_;
  std::optional<std::uint64_t> generation_;
  std::optional<std::int64_t> origin_;
  std::int64_t base_ns_ = 0;
  std::int64_t end_ns_ = 0;
};

SyncRemuxer::SyncRemuxer(SyncRemuxerConfig config)
    : impl_(std::make_shared<Impl>(config)) {}
SyncRemuxer::~SyncRemuxer() { Stop(); }
void SyncRemuxer::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
void SyncRemuxer::SetOnEnded(OnEnded callback) {
  impl_->SetOnEnded(std::move(callback));
}
void SyncRemuxer::Start(const std::vector<ffmpeg::StreamInfo>& streams) {
  impl_->Start(streams);
}
bool SyncRemuxer::SubmitPacket(std::uint64_t generation,
                               const ffmpeg::Packet& packet) {
  if (!packet.get()) throw std::invalid_argument("输入录像需要有效包");
  return impl_->SubmitPacket(generation, *packet.get());
}
bool SyncRemuxer::SubmitPacket(std::uint64_t generation,
                               const AVPacket& packet) {
  return impl_->SubmitPacket(generation, packet);
}
std::string SyncRemuxer::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void SyncRemuxer::AddRtspPublish(const std::string& app,
                                 const std::string& stream,
                                 const std::string& ip, std::uint16_t port) {
  impl_->AddRtspPublish(app, stream, ip, port);
}
std::string SyncRemuxer::AddHlsPublish(const std::string& app,
                                       const std::string& stream,
                                       const std::string& ip,
                                       std::uint16_t port) {
  return impl_->AddHlsPublish(app, stream, ip, port);
}
void SyncRemuxer::Drain() noexcept { impl_->Drain(); }
void SyncRemuxer::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer
