#include "mw/streamer/output/internal/remux_output.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Common/MultiMediaSourceMuxer.h"
#include "Common/config.h"
#include "Common/macros.h"
#include "Extension/Factory.h"
#include "Network/Buffer.h"
#include "Poller/EventPoller.h"
#include "Pusher/PusherProxy.h"
#include "mw/log.h"
#include "mw/streamer/converter/av_packet_to_zlm_frame_converter.h"
#include "mw/streamer/converter/internal/codec_bridge.h"
#include "mw/streamer/output/recording_target.h"
#include "mw/streamer/performance/operation_recorder.h"
#include "mw/streamer/zlm/internal/config_validator.h"

namespace mw::streamer::internal {
namespace {

enum class TargetKind { kRtmp, kRtsp, kSrt, kFmp4, kHls };

TargetKind ParseTarget(const std::string& target) {
  if (target.empty()) {
    throw std::invalid_argument("Remux输出目标不能为空");
  }
  const auto separator = target.find("://");
  if (separator != std::string::npos) {
    auto scheme = target.substr(0, separator);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char value) { return std::tolower(value); });
    if (scheme == "rtmp") return TargetKind::kRtmp;
    if (scheme == "rtsp") return TargetKind::kRtsp;
    if (scheme == "srt") return TargetKind::kSrt;
    throw std::invalid_argument("不支持的网络输出协议: " + scheme);
  }
  const auto extension = std::filesystem::path(target).extension();
  if (extension == ".mp4") return TargetKind::kFmp4;
  if (extension == ".m3u8") return TargetKind::kHls;
  throw std::invalid_argument("文件输出目标必须使用.mp4或.m3u8扩展名");
}

bool IsRecording(TargetKind kind) {
  return kind == TargetKind::kFmp4 || kind == TargetKind::kHls;
}

std::string GetSchema(TargetKind kind) {
  switch (kind) {
    case TargetKind::kRtmp:
      return RTMP_SCHEMA;
    case TargetKind::kRtsp:
      return RTSP_SCHEMA;
    case TargetKind::kSrt:
      return TS_SCHEMA;
    default:
      return {};
  }
}

bool StartsWithAnnexB(const std::uint8_t* data, std::size_t size) {
  return data && size >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

mediakit::Track::Ptr CreateTrack(const StreamInfo& stream) {
  const auto& parameters = *stream.codec_parameters.get();
  const auto codec = internal::ToZlmCodecId(parameters.codec_id);
  if (codec == mediakit::CodecInvalid) {
    throw std::invalid_argument("输出流包含不支持的codec");
  }
  const bool audio = mediakit::getTrackType(codec) == mediakit::TrackAudio;
  auto track = mediakit::Factory::getTrackByCodecId(
      codec, audio ? parameters.sample_rate : 0,
      audio ? parameters.ch_layout.nb_channels : 1,
      audio && parameters.bits_per_raw_sample > 0
          ? parameters.bits_per_raw_sample
          : 16);
  if (!track) throw std::invalid_argument("无法为输出流创建ZLM Track");
  track->setIndex(stream.stream_index);
  track->setBitRate(parameters.bit_rate);
  if (parameters.extradata_size <= 0) return track;

  const auto size = static_cast<std::size_t>(parameters.extradata_size);
  if (parameters.codec_id == AV_CODEC_ID_AAC ||
      !StartsWithAnnexB(parameters.extradata, size)) {
    track->setExtraData(parameters.extradata, size);
    return track;
  }
  auto buffer = std::make_shared<toolkit::BufferLikeString>();
  buffer->assign(reinterpret_cast<const char*>(parameters.extradata), size);
  auto frame =
      mediakit::Factory::getFrameFromBuffer(codec, std::move(buffer), 0, 0);
  if (frame) {
    frame->setIndex(stream.stream_index);
    track->inputFrame(frame);
  }
  return track;
}

mediakit::ProtocolOption MakeProtocolOption(TargetKind kind,
                                            std::size_t stream_count,
                                            const MuxerConfig& config) {
  mediakit::ProtocolOption option;
  option.modify_stamp = mediakit::ProtocolOption::kModifyStampOff;
  option.preserve_startup_packets = true;
  option.enable_audio = true;
  option.add_mute_audio = false;
  option.auto_close = false;
  option.paced_sender_ms =
      static_cast<std::uint32_t>(config.paced_sender_interval.count());
  option.enable_hls = false;
  option.enable_hls_fmp4 = false;
  option.enable_mp4 = false;
  option.enable_rtsp = kind == TargetKind::kRtsp;
  option.enable_rtmp = kind == TargetKind::kRtmp;
  option.enable_ts = kind == TargetKind::kSrt;
  option.enable_fmp4 = false;
  option.rtsp_demand = false;
  option.rtmp_demand = false;
  option.ts_demand = false;
  option.max_track = stream_count;
  return option;
}

mediakit::MediaTuple MakeMediaTuple() {
  static std::atomic<std::uint64_t> next_id{1};
  return {DEFAULT_VHOST,
          "mw-streamer-remux",
          fmt::format("output-{}", next_id.fetch_add(1)),
          {}};
}

std::chrono::system_clock::time_point NextRecordingStartTime() {
  // RecordingTarget includes milliseconds in its file name. A fast rebuild
  // must not truncate the previous recording opened within the same
  // millisecond.
  static std::atomic<std::int64_t> last_milliseconds{0};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  auto previous = last_milliseconds.load();
  std::int64_t next;
  do {
    next = std::max<std::int64_t>(now, previous + 1);
  } while (!last_milliseconds.compare_exchange_weak(previous, next));
  return std::chrono::system_clock::time_point(std::chrono::milliseconds(next));
}

}  // namespace

void ValidateRemuxOutputConfig(const RemuxOutputConfig& config) {
  static_cast<void>(ParseTarget(config.target));
  internal::ValidateOutputConfig(config.zlm);
}

class RemuxOutput::Impl final {
 public:
  Impl(RemuxOutputConfig config, std::vector<StreamInfo> streams,
       std::shared_ptr<toolkit::EventPoller> poller,
       std::function<void(const std::string&)> on_failed,
       OperationRecorder* performance)
      : config_(std::move(config)),
        streams_(std::move(streams)),
        poller_(std::move(poller)),
        on_failed_(std::move(on_failed)),
        kind_(ParseTarget(config_.target)),
        performance_(performance) {
    if (!poller_) throw std::invalid_argument("Remux输出需要Poller");
  }

  void Open() {
    if (state_ != State::kCreated) {
      throw std::logic_error("Remux输出只能打开一次");
    }
    try {
      OpenOutput();
    } catch (...) {
      Close();
      throw;
    }
  }

  void Write(const Packet& packet) {
    if (state_ != State::kOpen) {
      throw std::logic_error("Remux输出尚未打开或已停止");
    }
    if (!packet.get()) throw std::invalid_argument("输出Packet不能为空");
    if (!startup_complete_) {
      CacheStartup(packet);
      if (seen_tracks_.size() == streams_.size() && TracksReady()) {
        StartPackets();
      }
      return;
    }
    WritePacket(packet);
  }

  void Finish() {
    if (state_ != State::kOpen) return;
    if (!startup_packets_.empty()) StartPackets();
    if (fmp4_) fmp4_->Close();
    if (hls_) hls_->Close();
  }

  void Close() noexcept {
    if (state_ == State::kClosed) return;
    state_ = State::kClosed;
    on_failed_ = nullptr;
    if (bridge_) bridge_->Detach();
    if (pusher_) {
      pusher_->setPushCallbackOnce(nullptr);
      pusher_->setOnClose([](const toolkit::SockException&) {});
    }
    pusher_.reset();
    CloseRecording(fmp4_);
    CloseRecording(hls_);
    if (muxer_) {
      muxer_->setMediaListener({});
    }
    muxer_.reset();
    bridge_.reset();
    converters_.clear();
    startup_packets_.clear();
    tracks_.clear();
  }

  NetworkOutputSnapshot GetNetworkOutputSnapshot() const {
    NetworkOutputSnapshot result;
    result.target = config_.target;
    if (pusher_) {
      result.connected = pusher_->getStatus() == 0;
      result.reconnect_count = pusher_->getRePublishCount();
      result.sent_bytes =
          static_cast<std::uint64_t>(pusher_->getSendTotalBytes());
    }
    return result;
  }

 private:
  enum class State { kCreated, kOpen, kFailed, kClosed };

  // ZLM listeners are weak shared references. Only this bridge is shared; the
  // output and all business state remain exclusively owned by RemuxOutput.
  class ListenerBridge final
      : public mediakit::MediaSourceEvent,
        public std::enable_shared_from_this<ListenerBridge> {
   public:
    ListenerBridge(Impl& owner, toolkit::EventPoller::Ptr poller)
        : owner_(&owner), poller_(std::move(poller)) {}

    void Detach() { owner_ = nullptr; }

    void Post(std::function<void(Impl&)> action) {
      std::weak_ptr<ListenerBridge> weak_self = shared_from_this();
      poller_->async(
          [weak_self, action = std::move(action)]() {
            const auto self = weak_self.lock();
            if (!self || !self->owner_) return;
            try {
              action(*self->owner_);
            } catch (const std::exception& error) {
              self->owner_->Fail(error.what());
            } catch (...) {
              self->owner_->Fail("Remux输出异步操作发生未知异常");
            }
          },
          false);
    }

    toolkit::EventPoller::Ptr getOwnerPoller(mediakit::MediaSource&) override {
      return poller_;
    }

    void onRegist(mediakit::MediaSource& sender, bool regist) override {
      if (!regist) return;
      auto action = [tuple = sender.getMediaTuple(),
                     schema = sender.getSchema()](Impl& owner) {
        owner.OnRegistered(tuple, schema);
      };
      if (poller_->isCurrentThread()) {
        if (owner_) action(*owner_);
      } else {
        Post(std::move(action));
      }
    }

   private:
    Impl* owner_;
    const toolkit::EventPoller::Ptr poller_;
  };

  void OpenOutput() {
    internal::ValidateOutputConfig(config_.zlm);
    if (streams_.empty())
      throw std::invalid_argument("Remux输出至少需要一路轨道");
    for (const auto& stream : streams_) {
      stream.Validate();
      if (!converters_
               .try_emplace(stream.stream_index, stream.codec_parameters,
                            stream.time_base, stream.stream_index)
               .second) {
        throw std::invalid_argument("输出轨道stream_index重复");
      }
    }
    for (const auto& stream : streams_) tracks_.push_back(CreateTrack(stream));
    state_ = State::kOpen;
    // Recording consumes the supplied packets directly. MediaSink's live
    // track discovery cache may reorder or evict packets before tracks ready.
    if (IsRecording(kind_)) return;
    tuple_ = MakeMediaTuple();
    bridge_ = std::make_shared<ListenerBridge>(*this, poller_);
    muxer_ = std::make_shared<mediakit::MultiMediaSourceMuxer>(
        tuple_, 0.0F,
        MakeProtocolOption(kind_, streams_.size(), config_.zlm.muxer));
    muxer_->setTrackReadyTimeoutMS(0);
    muxer_->setMediaListener(bridge_);
    state_ = State::kOpen;
    for (const auto& track : tracks_) {
      if (!muxer_->addTrack(track)) {
        throw std::invalid_argument("ZLM Muxer不支持输出Track");
      }
    }
    muxer_->addTrackCompleted();
  }

  bool TracksReady() const {
    return std::all_of(tracks_.begin(), tracks_.end(),
                       [](const auto& track) { return track->ready(); });
  }

  void CacheStartup(const Packet& packet) {
    if (startup_packets_.size() >= config_.startup_packet_capacity) {
      throw std::runtime_error("Remux等待轨道信息的启动包缓存已满");
    }
    const auto index = packet->stream_index;
    const auto stream = std::find_if(
        streams_.begin(), streams_.end(),
        [index](const auto& item) { return item.stream_index == index; });
    if (stream == streams_.end()) {
      throw std::invalid_argument("输出Packet引用了未知轨道");
    }
    const auto minimum = std::min(
        av_rescale_q(packet->dts, stream->time_base, AVRational{1, 1000}),
        av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000}));
    if (startup_packets_.empty()) timestamp_origin_ms_ = minimum;
    timestamp_origin_ms_ = std::min(timestamp_origin_ms_, minimum);
    // Temporary frames only populate codec parameters. They have no consumers;
    // the original packets are replayed once the common origin is known.
    auto frames = converters_.at(index).Convert(packet, minimum);
    if (frames.empty()) {
      throw std::invalid_argument("Remux无法转换编码包");
    }
    const auto track = std::find_if(
        tracks_.begin(), tracks_.end(),
        [index](const auto& item) { return item->getIndex() == index; });
    for (const auto& frame : frames) (*track)->inputFrame(frame);
    seen_tracks_.insert(index);
    startup_packets_.push_back(packet);
  }

  void StartPackets() {
    if (!TracksReady()) {
      throw std::runtime_error("Remux无法封装：轨道编码参数尚未就绪");
    }
    if (IsRecording(kind_)) OpenRecording();
    startup_complete_ = true;
    for (const auto& packet : startup_packets_) WritePacket(packet);
    startup_packets_.clear();
  }

  void WritePacket(const Packet& packet) {
    std::optional<OperationRecorder::Call> call;
    if (performance_) call.emplace(*performance_);
    auto frames = converters_.at(packet->stream_index)
                      .Convert(packet, timestamp_origin_ms_);
    if (frames.empty()) {
      throw std::invalid_argument("Remux无法转换编码包或时间戳早于输出起点");
    }
    for (const auto& frame : frames) {
      if (fmp4_) fmp4_->Write(frame);
      if (hls_) hls_->Write(frame);
      if (muxer_) muxer_->inputFrame(frame);
    }
    if (performance_) performance_->AddOutput(1, packet->size);
  }

  void OpenRecording() {
    const auto start_time = NextRecordingStartTime();
    if (kind_ == TargetKind::kFmp4) {
      fmp4_ = std::make_unique<Fmp4FileTarget>(
          config_.target, tracks_, config_.zlm.recording, start_time, true);
    } else {
      hls_ = std::make_unique<HlsFmp4FileTarget>(
          config_.target, tracks_, config_.zlm.recording, start_time, true);
    }
  }

  void OnRegistered(const mediakit::MediaTuple& tuple,
                    const std::string& schema) {
    if (state_ != State::kOpen || IsRecording(kind_) ||
        !mediakit::equalMediaTuple(tuple, tuple_) ||
        schema != GetSchema(kind_)) {
      return;
    }
    StartPusher();
  }

  void StartPusher() {
    if (state_ != State::kOpen || pusher_) return;
    auto source = mediakit::MediaSource::find(GetSchema(kind_), tuple_.vhost,
                                              tuple_.app, tuple_.stream);
    if (!source) return;
    auto pusher = std::make_shared<mediakit::PusherProxy>(source, -1, poller_);
    pusher->setOnCreateSocket([](const toolkit::EventPoller::Ptr& poller) {
      auto socket = toolkit::Socket::createSocket(poller, true);
      static_cast<void>(socket->getSendTotalBytes());
      return socket;
    });
    (*pusher)[mediakit::Client::kTimeoutMS] =
        config_.zlm.pusher.connect_timeout.count();
    if (!config_.zlm.pusher.local_bind_ip.empty()) {
      (*pusher)[mediakit::Client::kNetAdapter] =
          config_.zlm.pusher.local_bind_ip;
    }
    pusher->setPushCallbackOnce(
        [target = config_.target](const toolkit::SockException& error) {
          if (error) {
            MW_LOG_WARNING("streamer", "首次推流失败，ZLM将继续重试：{}，{}",
                           target, error.what());
          } else {
            MW_LOG_INFO("streamer", "首次推流成功：{}", target);
          }
        });
    std::weak_ptr<ListenerBridge> weak_bridge = bridge_;
    pusher->setOnClose([weak_bridge](const toolkit::SockException& error) {
      if (const auto bridge = weak_bridge.lock()) {
        bridge->Post([reason = std::string(error.what())](Impl& owner) {
          owner.Fail(reason);
        });
      }
    });
    pusher_ = std::move(pusher);
    pusher_->publish(config_.target);
  }

  void Fail(const std::string& reason) {
    if (state_ != State::kOpen) return;
    state_ = State::kFailed;
    MW_LOG_ERROR("streamer", "Remux输出目标已失效：{}，{}", config_.target,
                 reason);
    if (!on_failed_) return;
    try {
      on_failed_(reason);
    } catch (const std::exception& error) {
      MW_LOG_ERROR("streamer", "Remux输出失败通知异常：{}", error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "Remux输出失败通知发生未知异常");
    }
  }

  template <typename Target>
  void CloseRecording(std::unique_ptr<Target>& target) noexcept {
    if (!target) return;
    try {
      target->Close();
    } catch (const std::exception& error) {
      MW_LOG_ERROR("streamer", "关闭录像失败：{}，{}", target->path().string(),
                   error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "关闭录像发生未知异常：{}", config_.target);
    }
    target.reset();
  }

  RemuxOutputConfig config_;
  std::vector<StreamInfo> streams_;
  toolkit::EventPoller::Ptr poller_;
  std::function<void(const std::string&)> on_failed_;
  const TargetKind kind_;
  OperationRecorder* const performance_;
  State state_ = State::kCreated;
  mediakit::MediaTuple tuple_;
  std::shared_ptr<ListenerBridge> bridge_;
  mediakit::MultiMediaSourceMuxer::Ptr muxer_;
  mediakit::PusherProxy::Ptr pusher_;
  std::unique_ptr<Fmp4FileTarget> fmp4_;
  std::unique_ptr<HlsFmp4FileTarget> hls_;
  std::unordered_map<int, AvPacketToZlmFrameConverter> converters_;
  std::vector<mediakit::Track::Ptr> tracks_;
  std::unordered_set<int> seen_tracks_;
  std::vector<Packet> startup_packets_;
  std::int64_t timestamp_origin_ms_ = 0;
  bool startup_complete_ = false;
};

RemuxOutput::RemuxOutput(RemuxOutputConfig config,
                         std::vector<StreamInfo> streams,
                         std::shared_ptr<toolkit::EventPoller> poller,
                         std::function<void(const std::string&)> on_failed,
                         OperationRecorder* performance)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(streams),
                                   std::move(poller), std::move(on_failed),
                                   performance)) {}

RemuxOutput::~RemuxOutput() { Close(); }
void RemuxOutput::Open() { impl_->Open(); }
void RemuxOutput::Write(const Packet& packet) { impl_->Write(packet); }
void RemuxOutput::Finish() { impl_->Finish(); }
void RemuxOutput::Close() noexcept { impl_->Close(); }
NetworkOutputSnapshot RemuxOutput::GetNetworkOutputSnapshot() const {
  return impl_->GetNetworkOutputSnapshot();
}

}  // namespace mw::streamer::internal
