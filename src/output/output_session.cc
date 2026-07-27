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
#include "mw/log/logging.h"
#include "output/recording_target.h"

namespace mw::streamer::output {
namespace {

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

mediakit::CodecId GetZlmCodecId(AVCodecID codec_id) {
  switch (codec_id) {
    case AV_CODEC_ID_H264:
      return mediakit::CodecH264;
    case AV_CODEC_ID_HEVC:
      return mediakit::CodecH265;
    case AV_CODEC_ID_AAC:
      return mediakit::CodecAAC;
    case AV_CODEC_ID_PCM_ALAW:
      return mediakit::CodecG711A;
    case AV_CODEC_ID_PCM_MULAW:
      return mediakit::CodecG711U;
    case AV_CODEC_ID_OPUS:
      return mediakit::CodecOpus;
    case AV_CODEC_ID_MJPEG:
      return mediakit::CodecJPEG;
    case AV_CODEC_ID_VP8:
      return mediakit::CodecVP8;
    case AV_CODEC_ID_VP9:
      return mediakit::CodecVP9;
    default:
      return mediakit::CodecInvalid;
  }
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
      GetZlmCodecId(parameters.codec_id), std::move(buffer), 0, 0);
  if (frame) {
    frame->setIndex(track->getIndex());
    track->inputFrame(frame);
  }
}

mediakit::Track::Ptr CreateTrack(const OutputStreamInfo& stream) {
  const auto& parameters = *stream.codec_parameters;
  const auto codec_id = GetZlmCodecId(parameters.codec_id);
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
    const std::vector<NetworkTarget>& targets, std::size_t stream_count) {
  mediakit::ProtocolOption option;
  option.modify_stamp = mediakit::ProtocolOption::kModifyStampOff;
  option.enable_audio = true;
  option.add_mute_audio = false;
  option.auto_close = false;
  option.paced_sender_ms = 0;

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

  void Write(const AVPacket* packet) {
    if (!packet) {
      return;
    }
    if (poller_->isCurrentThread()) {
      WriteOnPoller(packet);
      return;
    }

    auto owned_packet = std::shared_ptr<AVPacket>(
        av_packet_clone(packet),
        [](AVPacket* value) { av_packet_free(&value); });
    if (!owned_packet) {
      log::Module<log::LogModule::kStreamer>::Error(
          "复制输出AVPacket失败，数据包已丢弃");
      return;
    }

    auto self = shared_from_this();
    poller_->async(
        [self, owned_packet = std::move(owned_packet)]() {
          self->WriteOnPoller(owned_packet.get());
        },
        false);
  }

  void Close() noexcept {
    auto self = shared_from_this();
    if (poller_->isCurrentThread()) {
      CloseOnPoller();
      return;
    }
    poller_->sync([self]() { self->CloseOnPoller(); });
  }

 private:
  enum class State {
    kCreated,
    kOpen,
    kClosed,
  };

  struct ConverterBinding {
    converter::AvPacketToZlmFrameConverter converter;

    explicit ConverterBinding(const OutputStreamInfo& stream)
        : converter(*stream.codec_parameters, stream.time_base,
                    stream.stream_index) {}
  };

  void ValidateConfigOnPoller() {
    if (config_.streams.empty()) {
      throw std::invalid_argument("OutputSession至少需要一路输出流");
    }
    if (config_.targets.empty()) {
      throw std::invalid_argument("OutputSession至少需要一个输出目标");
    }

    std::unordered_set<int> stream_indexes;
    for (const auto& stream : config_.streams) {
      if (stream.stream_index < 0 || !stream.codec_parameters) {
        throw std::invalid_argument("输出流参数无效");
      }
      if (!stream_indexes.emplace(stream.stream_index).second) {
        throw std::invalid_argument("输出流stream_index重复");
      }
      if (stream.time_base.num <= 0 || stream.time_base.den <= 0) {
        throw std::invalid_argument("输出流time_base必须为正数");
      }
      const auto& parameters = *stream.codec_parameters;
      if (parameters.extradata_size < 0 ||
          (parameters.extradata_size > 0 && !parameters.extradata)) {
        throw std::invalid_argument("输出流codec extradata无效");
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

    for (const auto& stream : config_.streams) {
      converters_.try_emplace(stream.stream_index, stream);
    }

    tuple_ = MakeMediaTuple();
    const auto option =
        MakeProtocolOption(network_targets_, config_.streams.size());
    muxer_ =
        std::make_shared<mediakit::MultiMediaSourceMuxer>(tuple_, 0.0F, option);
    muxer_->setTrackListener(shared_from_this());

    for (const auto& stream : config_.streams) {
      if (!muxer_->addTrack(CreateTrack(stream))) {
        throw std::invalid_argument("ZLM Muxer不支持输出Track");
      }
    }
    muxer_->addTrackCompleted();
    recording_start_time_ = std::chrono::system_clock::now();
    state_ = State::kOpen;
  }

  void WriteOnPoller(const AVPacket* packet) noexcept {
    if (state_ != State::kOpen || !packet) {
      return;
    }
    const auto binding = converters_.find(packet->stream_index);
    if (binding == converters_.end()) {
      return;
    }

    try {
      auto frame = binding->second.converter.Convert(packet);
      if (!frame) {
        return;
      }

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
          log::Module<log::LogModule::kStreamer>::Error(
              "ZLM输出Muxer写入失败：{}", error.what());
        }
      }
      if (!awaiting_recordings) {
        WriteRecordingsOnPoller(frame);
      }
    } catch (const std::exception& error) {
      log::Module<log::LogModule::kStreamer>::Error("处理输出AVPacket失败：{}",
                                                    error.what());
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
        log::Module<log::LogModule::kStreamer>::Error(
            "{}录像写入失败，已停止该目标：{}", target_name, error.what());
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
        auto target = std::make_unique<Fmp4FileTarget>(path, tracks,
                                                       recording_start_time_);
        log::Module<log::LogModule::kStreamer>::Info("开始fMP4录像：{}",
                                                     target->path().string());
        fmp4_targets_.push_back(std::move(target));
      } catch (const std::exception& error) {
        log::Module<log::LogModule::kStreamer>::Error(
            "创建fMP4录像目标失败，已停止该目标：{}", error.what());
      }
    }

    for (const auto& path : hls_paths_) {
      try {
        auto target = std::make_unique<HlsFmp4FileTarget>(
            path, tracks, recording_start_time_);
        log::Module<log::LogModule::kStreamer>::Info("开始HLS-fMP4录像：{}",
                                                     target->path().string());
        hls_targets_.push_back(std::move(target));
      } catch (const std::exception& error) {
        log::Module<log::LogModule::kStreamer>::Error(
            "创建HLS-fMP4录像目标失败，已停止该目标：{}", error.what());
      }
    }
  }

  void StartPushersOnPoller() {
    if (state_ != State::kOpen || !muxer_) {
      return;
    }

    for (const auto& target : network_targets_) {
      const auto schema = GetMediaSourceSchema(target.protocol);
      auto source = mediakit::MediaSource::find(schema, tuple_.vhost,
                                                tuple_.app, tuple_.stream);
      if (!source) {
        log::Module<log::LogModule::kStreamer>::Error(
            "ZLM未生成{} MediaSource，无法启动推流：{}", schema, target.url);
        continue;
      }

      try {
        auto pusher =
            std::make_shared<mediakit::PusherProxy>(source, -1, poller_);
        pusher->setPushCallbackOnce(
            [url = target.url](const toolkit::SockException& error) {
              if (error) {
                log::Module<log::LogModule::kStreamer>::Warning(
                    "首次推流失败，ZLM将继续重试：{}，{}", url, error.what());
              }
            });
        pusher->setOnClose(
            [url = target.url](const toolkit::SockException& error) {
              log::Module<log::LogModule::kStreamer>::Error(
                  "ZLM已停止推流目标：{}，{}", url, error.what());
            });
        pusher->publish(target.url);
        pushers_.push_back(std::move(pusher));
      } catch (const std::exception& error) {
        log::Module<log::LogModule::kStreamer>::Error(
            "创建ZLM推流目标失败，已停止该目标：{}，{}", target.url,
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
        log::Module<log::LogModule::kStreamer>::Error(
            "关闭{}录像目标失败：{}", target_name, error.what());
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
      std::weak_ptr<Impl> weak_self = shared_from_this();
      poller_->async(
          [weak_self]() {
            if (auto self = weak_self.lock()) {
              self->StartPushersOnPoller();
            }
          },
          false);
    }
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
  std::vector<std::unique_ptr<Fmp4FileTarget>> fmp4_targets_;
  std::vector<std::unique_ptr<HlsFmp4FileTarget>> hls_targets_;
  bool recordings_initialized_ = false;
  std::vector<mediakit::Frame::Ptr> pending_recording_frames_;
};

OutputSession::OutputSession(OutputConfig config,
                             std::shared_ptr<toolkit::EventPoller> poller)
    : impl_(std::make_shared<Impl>(std::move(config), std::move(poller))) {}

OutputSession::~OutputSession() { Close(); }

void OutputSession::Open() { impl_->Open(); }

void OutputSession::Write(const AVPacket* packet) { impl_->Write(packet); }

void OutputSession::Close() noexcept { impl_->Close(); }

}  // namespace mw::streamer::output
