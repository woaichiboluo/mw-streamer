#include "mw/pipeline/internal/streaming/output_worker.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "mw/common/thread.h"
#include "mw/output/output_session.h"

namespace mw::streamer::pipeline::internal::streaming {

OutputWorker::OutputWorker(bool has_audio, bool has_video,
                           std::vector<std::string> output_targets,
                           zlm::OutputConfig zlm_config,
                           std::size_t startup_packet_capacity,
                           std::shared_ptr<toolkit::EventPoller> poller,
                           Callbacks callbacks)
    : has_audio_(has_audio),
      has_video_(has_video),
      output_targets_(std::move(output_targets)),
      zlm_config_(std::move(zlm_config)),
      startup_packet_capacity_(startup_packet_capacity),
      poller_(std::move(poller)),
      callbacks_(std::move(callbacks)) {}

OutputWorker::~OutputWorker() { Stop(); }

void OutputWorker::Start() {
  if (thread_) {
    throw std::logic_error("OutputWorker只能启动一次");
  }
  thread_ = std::make_unique<common::Thread>("mw-output", [this]() { Run(); });
}

void OutputWorker::RegisterOutputStream(AVMediaType media_type,
                                        const ffmpeg::StreamInfo& stream_info) {
  queue_.Push({WorkType::kStreamReady, media_type, stream_info, std::nullopt});
}

void OutputWorker::Write(const ffmpeg::Packet& packet) {
  queue_.Push(
      {WorkType::kPacket, AVMEDIA_TYPE_UNKNOWN, std::nullopt, packet.Ref()});
}

void OutputWorker::EndTrack(AVMediaType media_type) {
  queue_.Push({WorkType::kTrackEnd, media_type, std::nullopt, std::nullopt});
}

void OutputWorker::RequestStop() noexcept {
  queue_.Clear();
  queue_.Close();
}

void OutputWorker::Stop() noexcept {
  RequestStop();
  if (thread_ && !thread_->IsCurrent()) {
    thread_->Join();
  }
  CloseOutput();
}

void OutputWorker::Run() noexcept {
  try {
    RunLoop();
  } catch (const std::exception& error) {
    callbacks_.on_failed(error.what());
  } catch (...) {
    callbacks_.on_failed("未知异常");
  }
}

void OutputWorker::RunLoop() {
  while (auto work = queue_.WaitPop()) {
    switch (work->type) {
      case WorkType::kStreamReady:
        HandleStreamReady(work->media_type, std::move(*work->stream_info));
        break;
      case WorkType::kPacket:
        HandlePacket(std::move(*work->packet));
        break;
      case WorkType::kTrackEnd:
        HandleTrackEnd(work->media_type);
        break;
    }
  }
}

void OutputWorker::HandleStreamReady(AVMediaType media_type,
                                     ffmpeg::StreamInfo stream_info) {
  if (media_type == AVMEDIA_TYPE_AUDIO) {
    if (audio_stream_) {
      throw std::logic_error("音频编码器重复注册StreamInfo");
    }
    audio_stream_ = std::move(stream_info);
  } else if (media_type == AVMEDIA_TYPE_VIDEO) {
    if (video_stream_) {
      throw std::logic_error("视频编码器重复注册StreamInfo");
    }
    video_stream_ = std::move(stream_info);
  } else {
    throw std::invalid_argument("输出注册包含未知媒体类型");
  }
  if (AllStreamsReady()) {
    OpenOutput();
  }
}

void OutputWorker::HandlePacket(ffmpeg::Packet packet) {
  if (output_) {
    output_->Write(packet);
    return;
  }
  if (pending_packets_.size() >= startup_packet_capacity_) {
    throw std::runtime_error("Output启动缓存已满");
  }
  pending_packets_.push_back(std::move(packet));
}

void OutputWorker::HandleTrackEnd(AVMediaType media_type) {
  if (media_type == AVMEDIA_TYPE_AUDIO) {
    if (!has_audio_ || audio_ended_) {
      throw std::logic_error("Output收到无效的音频结束事件");
    }
    audio_ended_ = true;
  } else if (media_type == AVMEDIA_TYPE_VIDEO) {
    if (!has_video_ || video_ended_) {
      throw std::logic_error("Output收到无效的视频结束事件");
    }
    video_ended_ = true;
  } else {
    throw std::invalid_argument("Output收到未知媒体类型的结束事件");
  }
  if ((!has_audio_ || audio_ended_) && (!has_video_ || video_ended_)) {
    CloseOutput();
    callbacks_.on_completed();
    queue_.Close();
  }
}

void OutputWorker::OpenOutput() {
  if (output_) {
    return;
  }
  output_ = std::make_unique<output::OutputSession>(
      output::OutputConfig{EncodedStreams(), output_targets_, zlm_config_},
      poller_);
  output_->Open();
  for (const auto& packet : pending_packets_) {
    output_->Write(packet);
  }
  pending_packets_.clear();
  callbacks_.on_ready();
}

void OutputWorker::CloseOutput() noexcept {
  if (output_) {
    output_->Close();
    output_.reset();
  }
}

bool OutputWorker::AllStreamsReady() const noexcept {
  return (!has_audio_ || audio_stream_.has_value()) &&
         (!has_video_ || video_stream_.has_value());
}

std::vector<ffmpeg::StreamInfo> OutputWorker::EncodedStreams() const {
  std::vector<ffmpeg::StreamInfo> streams;
  if (audio_stream_) {
    streams.push_back(*audio_stream_);
  }
  if (video_stream_) {
    streams.push_back(*video_stream_);
  }
  std::sort(streams.begin(), streams.end(),
            [](const auto& left, const auto& right) {
              return left.stream_index < right.stream_index;
            });
  return streams;
}

}  // namespace mw::streamer::pipeline::internal::streaming
