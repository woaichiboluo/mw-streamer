#include "mw/output/output_session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
}

#include "Common/MultiMediaSourceMuxer.h"
#include "Common/config.h"
#include "Common/macros.h"
#include "Extension/Factory.h"
#include "Extension/Track.h"
#include "Network/Buffer.h"
#include "Poller/EventPoller.h"
#include "Pusher/PusherProxy.h"
#include "mw/converter/av_packet_to_zlm_frame_converter.h"
#include "mw/converter/internal/codec_bridge.h"
#include "mw/log/logging.h"
#include "mw/output/recording_target.h"
#include "mw/zlm/internal/config_validator.h"

namespace mw::streamer::output {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

constexpr char kInternalApp[] = "mw-streamer-output";
constexpr char kRtmpScheme[] = "rtmp";
constexpr char kRtspScheme[] = "rtsp";
constexpr char kSrtScheme[] = "srt";

enum class NetworkProtocol {
  kRtmp,
  kRtsp,
  kSrt,
};

struct NetworkTarget {
  std::string url;
  NetworkProtocol protocol;
};

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::string GetScheme(const std::string& target) {
  const auto separator = target.find("://");
  if (separator == std::string::npos) {
    return {};
  }
  return ToLower(target.substr(0, separator));
}

bool StartsWithAnnexB(const std::uint8_t* data, std::size_t size) {
  return data && size >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

void ApplyExtraData(const AVCodecParameters& parameters,
                    const mediakit::Track::Ptr& track) {
  if (!track || parameters.extradata_size <= 0) {
    return;
  }

  const auto size = static_cast<std::size_t>(parameters.extradata_size);
  if (parameters.codec_id == AV_CODEC_ID_AAC ||
      !StartsWithAnnexB(parameters.extradata, size)) {
    track->setExtraData(parameters.extradata, size);
    return;
  }

  auto buffer = std::make_shared<toolkit::BufferLikeString>();
  buffer->assign(reinterpret_cast<const char*>(parameters.extradata), size);
  auto frame = mediakit::Factory::getFrameFromBuffer(
      converter::internal::ToZlmCodecId(parameters.codec_id), std::move(buffer),
      0, 0);
  if (frame) {
    frame->setIndex(track->getIndex());
    track->inputFrame(frame);
  }
}

mediakit::Track::Ptr CreateTrack(const ffmpeg::StreamInfo& stream) {
  const auto& parameters = *stream.codec_parameters.get();
  const auto codec_id = converter::internal::ToZlmCodecId(parameters.codec_id);
  if (codec_id == mediakit::CodecInvalid) {
    throw std::invalid_argument("输出流包含不支持的codec");
  }

  int sample_rate = 0;
  int channels = 1;
  int sample_bits = 16;
  if (mediakit::getTrackType(codec_id) == mediakit::TrackAudio) {
    sample_rate = parameters.sample_rate;
    channels = parameters.ch_layout.nb_channels;
    if (parameters.bits_per_raw_sample > 0) {
      sample_bits = parameters.bits_per_raw_sample;
    }
  }

  auto track = mediakit::Factory::getTrackByCodecId(codec_id, sample_rate,
                                                    channels, sample_bits);
  if (!track) {
    throw std::invalid_argument("无法为输出流创建ZLM Track");
  }
  track->setBitRate(parameters.bit_rate);
  track->setIndex(stream.stream_index);
  ApplyExtraData(parameters, track);
  return track;
}

std::string GetMediaSourceSchema(NetworkProtocol protocol) {
  switch (protocol) {
    case NetworkProtocol::kRtmp:
      return RTMP_SCHEMA;
    case NetworkProtocol::kRtsp:
      return RTSP_SCHEMA;
    case NetworkProtocol::kSrt:
      return TS_SCHEMA;
  }
  throw std::logic_error("未知网络输出协议");
}

mediakit::ProtocolOption MakeProtocolOption(
    const std::vector<NetworkTarget>& targets, std::size_t stream_count,
    const zlm::MuxerConfig& config) {
  mediakit::ProtocolOption option;
  option.modify_stamp = mediakit::ProtocolOption::kModifyStampOff;
  option.enable_audio = true;
  option.add_mute_audio = false;
  option.auto_close = false;
  option.paced_sender_ms =
      static_cast<std::uint32_t>(config.paced_sender_interval.count());

  option.enable_hls = false;
  option.enable_hls_fmp4 = false;
  option.enable_mp4 = false;
  option.enable_rtsp = false;
  option.enable_rtmp = false;
  option.enable_ts = false;
  option.enable_fmp4 = false;

  option.rtsp_demand = false;
  option.rtmp_demand = false;
  option.ts_demand = false;
  option.max_track = stream_count;

  for (const auto& target : targets) {
    switch (target.protocol) {
      case NetworkProtocol::kRtmp:
        option.enable_rtmp = true;
        break;
      case NetworkProtocol::kRtsp:
        option.enable_rtsp = true;
        break;
      case NetworkProtocol::kSrt:
        option.enable_ts = true;
        break;
    }
  }
  return option;
}

mediakit::MediaTuple MakeMediaTuple() {
  static std::atomic<std::uint64_t> next_id{1};
  return {DEFAULT_VHOST,
          kInternalApp,
          "output-" + std::to_string(next_id.fetch_add(1)),
          {}};
}

}  // namespace

class OutputSession::Impl final
    : public mediakit::MultiMediaSourceMuxer::Listener,
      public mediakit::MediaSourceEvent,
      public std::enable_shared_from_this<OutputSession::Impl> {
 public:
  Impl(OutputConfig config, std::shared_ptr<toolkit::EventPoller> poller)
      : config_(std::move(config)),
        poller_(poller ? std::move(poller)
                       : toolkit::EventPollerPool::Instance().getPoller()) {}

  void Open() {
    std::exception_ptr error;
    auto self = shared_from_this();
    poller_->sync([self, &error]() {
      try {
        self->OpenOnPoller();
      } catch (...) {
        self->CloseOnPoller();
        error = std::current_exception();
      }
    });
    if (error) {
      std::rethrow_exception(error);
    }
  }

  void Write(const ffmpeg::Packet& packet) {
    if (poller_->isCurrentThread()) {
      try {
        WriteOnPoller(packet);
      } catch (const std::exception& error) {
        Log::Error("引用输出AVPacket失败，数据包已丢弃：{}", error.what());
      }
      return;
    }

    try {
      auto self = shared_from_this();
      poller_->async(
          [self, packet]() mutable { self->WriteOnPoller(std::move(packet)); },
          false);
    } catch (const std::exception& error) {
      Log::Error("引用输出AVPacket失败，数据包已丢弃：{}", error.what());
    }
  }

  void Close() noexcept {
    auto self = shared_from_this();
    if (poller_->isCurrentThread()) {
      CloseOnPoller();
      return;
    }
    poller_->sync([self]() { self->CloseOnPoller(); });
  }

  std::vector<NetworkTraffic> GetNetworkTraffic() {
    if (poller_->isCurrentThread()) {
      return GetNetworkTrafficOnPoller();
    }
    std::vector<NetworkTraffic> traffic;
    auto self = shared_from_this();
    poller_->sync(
        [self, &traffic]() { traffic = self->GetNetworkTrafficOnPoller(); });
    return traffic;
  }

 private:
  enum class State {
    kCreated,
    kOpen,
    kClosed,
  };

  struct ConverterBinding {
    converter::AvPacketToZlmFrameConverter converter;

    explicit ConverterBinding(const ffmpeg::StreamInfo& stream)
        : converter(stream.codec_parameters, stream.time_base,
                    stream.stream_index) {}
  };

  void ValidateConfigOnPoller() {
    zlm::internal::ValidateOutputConfig(config_.zlm);
    if (config_.streams.empty()) {
      throw std::invalid_argument("OutputSession至少需要一路输出流");
    }
    if (config_.targets.empty()) {
      throw std::invalid_argument("OutputSession至少需要一个输出目标");
    }

    std::unordered_set<int> stream_indexes;
    for (const auto& stream : config_.streams) {
      stream.Validate();
      if (!stream_indexes.emplace(stream.stream_index).second) {
        throw std::invalid_argument("输出流stream_index重复");
      }
    }

    for (const auto& target : config_.targets) {
      if (target.empty()) {
        throw std::invalid_argument("输出目标不能为空");
      }

      const auto scheme = GetScheme(target);
      if (scheme == kRtmpScheme) {
        network_targets_.push_back({target, NetworkProtocol::kRtmp});
        continue;
      }
      if (scheme == kRtspScheme) {
        network_targets_.push_back({target, NetworkProtocol::kRtsp});
        continue;
      }
      if (scheme == kSrtScheme) {
        network_targets_.push_back({target, NetworkProtocol::kSrt});
        continue;
      }
      if (!scheme.empty()) {
        throw std::invalid_argument("不支持的网络输出协议: " + scheme);
      }

      const std::filesystem::path path(target);
      if (path.extension() == ".mp4") {
        fmp4_paths_.push_back(path);
      } else if (path.extension() == ".m3u8") {
        hls_paths_.push_back(path);
      } else {
        throw std::invalid_argument("文件输出目标必须使用.mp4或.m3u8扩展名");
      }
    }
  }

  void OpenOnPoller() {
    if (state_ != State::kCreated) {
      throw std::logic_error("OutputSession只能打开一次");
    }
    ValidateConfigOnPoller();
    has_video_ =
        std::any_of(config_.streams.begin(), config_.streams.end(),
                    [](const ffmpeg::StreamInfo& stream) {
                      return stream.codec_parameters.get()->codec_type ==
                             AVMEDIA_TYPE_VIDEO;
                    });
    output_started_ = !has_video_;

    for (const auto& stream : config_.streams) {
      converters_.try_emplace(stream.stream_index, stream);
    }

    tuple_ = MakeMediaTuple();
    const auto option = MakeProtocolOption(
        network_targets_, config_.streams.size(), config_.zlm.muxer);
    muxer_ =
        std::make_shared<mediakit::MultiMediaSourceMuxer>(tuple_, 0.0F, option);
    muxer_->setTrackReadyTimeoutMS(0);
    muxer_->setMediaListener(shared_from_this());
    muxer_->setTrackListener(shared_from_this());
    pusher_started_.resize(network_targets_.size(), false);
    pushers_.resize(network_targets_.size());

    for (const auto& stream : config_.streams) {
      if (!muxer_->addTrack(CreateTrack(stream))) {
        throw std::invalid_argument("ZLM Muxer不支持输出Track");
      }
    }
    muxer_->addTrackCompleted();
    recording_start_time_ = std::chrono::system_clock::now();
    state_ = State::kOpen;
    Log::Info(
        "OutputSession已打开: streams={}, network_targets={}, "
        "fmp4_targets={}, hls_fmp4_targets={}",
        config_.streams.size(), network_targets_.size(), fmp4_paths_.size(),
        hls_paths_.size());
  }

  void WriteOnPoller(ffmpeg::Packet packet) noexcept {
    const auto* raw_packet = packet.get();
    if (state_ != State::kOpen || !raw_packet) {
      return;
    }
    const auto binding = converters_.find(raw_packet->stream_index);
    if (binding == converters_.end()) {
      return;
    }

    try {
      auto frames = binding->second.converter.Convert(std::move(packet));
      if (frames.empty()) {
        return;
      }

      const bool contains_video_frame =
          frames.front()->getTrackType() == mediakit::TrackVideo;
      const bool contains_video_key_frame =
          contains_video_frame &&
          std::any_of(frames.begin(), frames.end(),
                      [](const auto& frame) { return frame->keyFrame(); });
      if (has_video_ && !output_started_) {
        if (!contains_video_key_frame) {
          for (const auto& frame : frames) {
            if (frame->getTrackType() == mediakit::TrackVideo &&
                frame->configFrame()) {
              pending_video_config_frames_.push_back(
                  mediakit::Frame::getCacheAbleFrame(frame));
            }
          }
          return;
        }
        frames.insert(frames.begin(), pending_video_config_frames_.begin(),
                      pending_video_config_frames_.end());
        pending_video_config_frames_.clear();
        output_started_ = true;
      }

      for (const auto& frame : frames) {
        const bool awaiting_recordings =
            !recordings_initialized_ &&
            (!fmp4_paths_.empty() || !hls_paths_.empty());
        if (awaiting_recordings) {
          pending_recording_frames_.push_back(
              mediakit::Frame::getCacheAbleFrame(frame));
        }

        if (muxer_) {
          try {
            muxer_->inputFrame(frame);
          } catch (const std::exception& error) {
            Log::Error("ZLM输出Muxer写入失败：{}", error.what());
          }
        }
        if (!awaiting_recordings) {
          WriteRecordingsOnPoller(frame);
        }
      }
      // ZLM的协议包缓存会在下一个视频AU到来时提交前一个关键AU。
      // 只在提交完成后启动Pusher，避免从未包含关键帧的GOP缓存中途加入。
      if (contains_video_frame && !schemas_with_pending_key_.empty()) {
        ready_schemas_.insert(schemas_with_pending_key_.begin(),
                              schemas_with_pending_key_.end());
        schemas_with_pending_key_.clear();
        StartPushersOnPoller();
      }
      if (contains_video_key_frame) {
        for (const auto& schema : registered_schemas_) {
          if (ready_schemas_.find(schema) == ready_schemas_.end()) {
            schemas_with_pending_key_.insert(schema);
          }
        }
      }
    } catch (const std::exception& error) {
      Log::Error("处理输出AVPacket失败：{}", error.what());
    }
  }

  template <typename Target>
  void WriteRecordingListOnPoller(std::vector<std::unique_ptr<Target>>& targets,
                                  const mediakit::Frame::Ptr& frame,
                                  const char* target_name) {
    for (auto target = targets.begin(); target != targets.end();) {
      try {
        (*target)->Write(frame);
        ++target;
      } catch (const std::exception& error) {
        Log::Error("{}录像写入失败，已停止该目标：{}，{}", target_name,
                   (*target)->path().string(), error.what());
        target = targets.erase(target);
      }
    }
  }

  void WriteRecordingsOnPoller(const mediakit::Frame::Ptr& frame) {
    WriteRecordingListOnPoller(fmp4_targets_, frame, "fMP4");
    WriteRecordingListOnPoller(hls_targets_, frame, "HLS-fMP4");
  }

  void InitializeRecordingsOnPoller(
      const std::vector<mediakit::Track::Ptr>& tracks) {
    for (const auto& path : fmp4_paths_) {
      try {
        auto target = std::make_unique<Fmp4FileTarget>(
            path, tracks, config_.zlm.recording, recording_start_time_);
        Log::Info("开始fMP4录像：{}", target->path().string());
        fmp4_targets_.push_back(std::move(target));
      } catch (const std::exception& error) {
        Log::Error("创建fMP4录像目标失败，已停止该目标：{}，{}", path.string(),
                   error.what());
      }
    }

    for (const auto& path : hls_paths_) {
      try {
        auto target = std::make_unique<HlsFmp4FileTarget>(
            path, tracks, config_.zlm.recording, recording_start_time_);
        Log::Info("开始HLS-fMP4录像：{}", target->path().string());
        hls_targets_.push_back(std::move(target));
      } catch (const std::exception& error) {
        Log::Error("创建HLS-fMP4录像目标失败，已停止该目标：{}，{}",
                   path.string(), error.what());
      }
    }
  }

  void StartPushersOnPoller(const std::string& registered_schema = {}) {
    if (state_ != State::kOpen || !muxer_) {
      return;
    }

    for (std::size_t index = 0; index < network_targets_.size(); ++index) {
      if (pusher_started_[index]) {
        continue;
      }
      const auto& target = network_targets_[index];
      const auto schema = GetMediaSourceSchema(target.protocol);
      if (!registered_schema.empty() && registered_schema != schema) {
        continue;
      }
      if (ready_schemas_.find(schema) == ready_schemas_.end()) {
        continue;
      }
      auto source = mediakit::MediaSource::find(schema, tuple_.vhost,
                                                tuple_.app, tuple_.stream);
      if (!source) {
        continue;
      }

      try {
        auto pusher =
            std::make_shared<mediakit::PusherProxy>(source, -1, poller_);
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
            [url = target.url](const toolkit::SockException& error) {
              if (error) {
                Log::Warning("首次推流失败，ZLM将继续重试：{}，{}", url,
                             error.what());
                return;
              }
              Log::Info("首次推流成功：{}", url);
            });
        pusher->setOnClose(
            [url = target.url](const toolkit::SockException& error) {
              Log::Error("ZLM已停止推流目标：{}，{}", url, error.what());
            });
        pusher->publish(target.url);
        pushers_[index] = std::move(pusher);
        pusher_started_[index] = true;
      } catch (const std::exception& error) {
        Log::Error("创建ZLM推流目标失败，已停止该目标：{}，{}", target.url,
                   error.what());
      }
    }
  }

  template <typename Target>
  void CloseRecordingListOnPoller(std::vector<std::unique_ptr<Target>>& targets,
                                  const char* target_name) noexcept {
    for (auto& target : targets) {
      try {
        target->Close();
      } catch (const std::exception& error) {
        Log::Error("关闭{}录像目标失败：{}，{}", target_name,
                   target->path().string(), error.what());
      }
    }
    targets.clear();
  }

  void CloseOnPoller() noexcept {
    if (state_ == State::kClosed) {
      return;
    }
    state_ = State::kClosed;
    pushers_.clear();
    CloseRecordingListOnPoller(fmp4_targets_, "fMP4");
    CloseRecordingListOnPoller(hls_targets_, "HLS-fMP4");
    muxer_.reset();
    converters_.clear();
    pending_video_config_frames_.clear();
    registered_schemas_.clear();
    schemas_with_pending_key_.clear();
    ready_schemas_.clear();
  }

  std::vector<NetworkTraffic> GetNetworkTrafficOnPoller() const {
    std::vector<NetworkTraffic> traffic;
    traffic.reserve(network_targets_.size());
    for (std::size_t index = 0; index < network_targets_.size(); ++index) {
      NetworkTraffic target;
      target.target = network_targets_[index].url;
      if (index < pushers_.size() && pushers_[index]) {
        target.connected = pushers_[index]->getStatus() == 0;
        target.reconnect_count = pushers_[index]->getRePublishCount();
        target.sent_bytes =
            static_cast<std::uint64_t>(pushers_[index]->getSendTotalBytes());
      }
      traffic.push_back(std::move(target));
    }
    return traffic;
  }

  void onAllTrackReady() override {
    if (state_ != State::kOpen || !muxer_) {
      return;
    }
    InitializeRecordingsOnPoller(muxer_->getTracks(true));
    recordings_initialized_ = true;
    for (const auto& frame : pending_recording_frames_) {
      WriteRecordingsOnPoller(frame);
    }
    pending_recording_frames_.clear();

    if (!network_targets_.empty()) {
      StartPushersOnPoller();
    }
  }

  void onRegist(mediakit::MediaSource& sender, bool regist) override {
    if (!regist || !mediakit::equalMediaTuple(sender.getMediaTuple(), tuple_)) {
      return;
    }

    const auto schema = sender.getSchema();
    if (poller_->isCurrentThread()) {
      registered_schemas_.insert(schema);
      if (!has_video_) {
        ready_schemas_.insert(schema);
      }
      StartPushersOnPoller(schema);
      return;
    }

    std::weak_ptr<Impl> weak_self = shared_from_this();
    poller_->async(
        [weak_self, schema]() {
          if (auto self = weak_self.lock()) {
            self->registered_schemas_.insert(schema);
            if (!self->has_video_) {
              self->ready_schemas_.insert(schema);
            }
            self->StartPushersOnPoller(schema);
          }
        },
        false);
  }

  OutputConfig config_;
  std::shared_ptr<toolkit::EventPoller> poller_;
  State state_ = State::kCreated;
  mediakit::MediaTuple tuple_;
  std::chrono::system_clock::time_point recording_start_time_;

  std::vector<NetworkTarget> network_targets_;
  std::vector<std::filesystem::path> fmp4_paths_;
  std::vector<std::filesystem::path> hls_paths_;
  std::unordered_map<int, ConverterBinding> converters_;
  mediakit::MultiMediaSourceMuxer::Ptr muxer_;
  std::vector<mediakit::PusherProxy::Ptr> pushers_;
  std::vector<bool> pusher_started_;
  std::vector<std::unique_ptr<Fmp4FileTarget>> fmp4_targets_;
  std::vector<std::unique_ptr<HlsFmp4FileTarget>> hls_targets_;
  bool recordings_initialized_ = false;
  std::vector<mediakit::Frame::Ptr> pending_recording_frames_;
  std::vector<mediakit::Frame::Ptr> pending_video_config_frames_;
  std::unordered_set<std::string> registered_schemas_;
  std::unordered_set<std::string> schemas_with_pending_key_;
  std::unordered_set<std::string> ready_schemas_;
  bool has_video_ = false;
  bool output_started_ = false;
};

OutputSession::OutputSession(OutputConfig config,
                             std::shared_ptr<toolkit::EventPoller> poller)
    : impl_(std::make_shared<Impl>(std::move(config), std::move(poller))) {}

OutputSession::~OutputSession() { Close(); }

void OutputSession::Open() { impl_->Open(); }

void OutputSession::Write(const ffmpeg::Packet& packet) {
  impl_->Write(packet);
}

std::vector<NetworkTraffic> OutputSession::GetNetworkTraffic() const {
  return impl_->GetNetworkTraffic();
}

void OutputSession::Close() noexcept { impl_->Close(); }

}  // namespace mw::streamer::output
