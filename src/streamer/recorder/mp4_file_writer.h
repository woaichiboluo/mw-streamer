#ifndef MW_SRC_STREAMER_RECORDER_MP4_FILE_WRITER_H_
#define MW_SRC_STREAMER_RECORDER_MP4_FILE_WRITER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
}

#include "mw/streamer/ffmpeg/av_dictionary.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

using ffmpeg::FfmpegException;

struct FormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context->pb) avio_closep(&context->pb);
    avformat_free_context(context);
  }
};

struct BitstreamFilterDeleter {
  void operator()(AVBSFContext* context) const noexcept {
    av_bsf_free(&context);
  }
};

// The caller serializes file access and retains streams for this lifetime.
class Mp4FileWriter final {
 public:
  Mp4FileWriter(const std::vector<ffmpeg::StreamInfo>& streams,
                const std::string& path, bool normalize_timestamps = true)
      : streams_(streams), normalize_timestamps_(normalize_timestamps) {
    AVFormatContext* raw = nullptr;
    const int result =
        avformat_alloc_output_context2(&raw, nullptr, "mp4", path.c_str());
    context_.reset(raw);
    FfmpegException::throwIfError(result, "创建录像 MP4 上下文");
    if (!context_) throw std::bad_alloc();
    for (const auto& info : streams_) {
      auto* stream = avformat_new_stream(context_.get(), nullptr);
      if (!stream) throw std::bad_alloc();
      FfmpegException::throwIfError(
          avcodec_parameters_copy(stream->codecpar,
                                  info.codec_parameters.get()),
          "复制录像轨道参数");
      stream->codecpar->codec_tag = 0;
      stream->time_base = info.time_base;
      stream->avg_frame_rate = info.frame_rate;
      stream->sample_aspect_ratio = info.sample_aspect_ratio;
    }
    // Force the local file protocol: even a colon in a filesystem name must
    // never select another FFmpeg protocol.
    const auto file_url = "file:" + path;
    FfmpegException::throwIfError(
        avio_open(&context_->pb, file_url.c_str(), AVIO_FLAG_WRITE),
        "打开录像文件");
    ffmpeg::AvDictionary options;
    // Delay moov until the first fragment so edit lists can represent initial
    // audio padding and video reorder delay.
    options.Set("movflags", "frag_keyframe+delay_moov+default_base_moof");
    options.Set("movie_timescale", "1000000");
    // Also fragment audio-only recordings and long GOPs to avoid retaining an
    // entire recording in the muxer. This does not change frame timestamps.
    options.Set("frag_duration", "1000000");
    FfmpegException::throwIfError(
        avformat_write_header(context_.get(), options.address()),
        "写入录像文件头");
    CheckIo();
  }

  Mp4FileWriter(const Mp4FileWriter&) = delete;
  Mp4FileWriter& operator=(const Mp4FileWriter&) = delete;

  void Write(ffmpeg::Packet packet) {
    const auto found =
        std::find_if(streams_.begin(), streams_.end(), [&](const auto& info) {
          return info.stream_index == packet->stream_index;
        });
    if (found == streams_.end())
      throw std::invalid_argument("MP4 编码包的轨道索引无效");
    const auto index = static_cast<std::size_t>(found - streams_.begin());
    const auto& info = streams_[index];
    // Match the MP4 muxer's detection: only ADTS needs aac_adtstoasc. Raw AAC
    // can be copied even when its input track has no AudioSpecificConfig.
    if (info.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC &&
        packet->size > 2 && (AV_RB16(packet->data) & 0xfff0) == 0xfff0) {
      if (!audio_filter_) {
        const auto* filter = av_bsf_get_by_name("aac_adtstoasc");
        if (!filter)
          throw FfmpegException(AVERROR_BSF_NOT_FOUND, "查找录像 AAC 过滤器");
        AVBSFContext* raw_filter = nullptr;
        FfmpegException::throwIfError(av_bsf_alloc(filter, &raw_filter),
                                      "创建录像 AAC 过滤器");
        audio_filter_.reset(raw_filter);
        audio_index_ = index;
        FfmpegException::throwIfError(
            avcodec_parameters_copy(audio_filter_->par_in,
                                    info.codec_parameters.get()),
            "复制录像 AAC 过滤器参数");
        audio_filter_->time_base_in = info.time_base;
        FfmpegException::throwIfError(av_bsf_init(audio_filter_.get()),
                                      "初始化录像 AAC 过滤器");
      }
      WriteAudio(packet.get());
    } else {
      WritePacket(std::move(packet), index);
    }
  }

  void Finish() {
    if (audio_filter_) WriteAudio(nullptr);
    FfmpegException::throwIfError(av_write_trailer(context_.get()),
                                  "写入录像文件尾");
    avio_flush(context_->pb);
    CheckIo();
    FfmpegException::throwIfError(avio_closep(&context_->pb), "关闭录像文件");
  }

 private:
  void WriteAudio(AVPacket* packet) {
    // Also handle HLS/TS packets that retain ADTS despite having global ASC.
    FfmpegException::throwIfError(
        av_bsf_send_packet(audio_filter_.get(), packet), "提交录像 AAC 编码包");
    for (;;) {
      ffmpeg::Packet filtered;
      const int result =
          av_bsf_receive_packet(audio_filter_.get(), filtered.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      FfmpegException::throwIfError(result, "读取录像 AAC 编码包");
      WritePacket(std::move(filtered), audio_index_);
    }
  }

  void WritePacket(ffmpeg::Packet packet, std::size_t index) {
    const auto& info = streams_[index];
    const auto input_base = info.time_base;
    if (normalize_timestamps_) {
      if (!origin_) {
        origin_ = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        origin_base_ = input_base;
        // Padding is decoded but not presented. Keep it before presentation
        // zero, including for audio-only recording, so MP4 can retain the trim.
        const auto* codec = info.codec_parameters.get();
        std::size_t size = 0;
        const auto* skip = av_packet_get_side_data(
            packet.get(), AV_PKT_DATA_SKIP_SAMPLES, &size);
        if (skip && size >= 4 && codec->sample_rate > 0) {
          *origin_ +=
              av_rescale_q(AV_RL32(skip), {1, codec->sample_rate}, input_base);
        }
      }
      const auto offset = av_rescale_q(*origin_, origin_base_, input_base);
      if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset;
      if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset;
    }
    av_packet_rescale_ts(packet.get(), input_base,
                         context_->streams[index]->time_base);
    packet->stream_index = static_cast<int>(index);
    packet->pos = -1;
    FfmpegException::throwIfError(
        av_interleaved_write_frame(context_.get(), packet.get()),
        "写入录像编码包");
    CheckIo();
  }

  void CheckIo() const {
    FfmpegException::throwIfError(context_->pb->error, "录像文件 IO");
  }

  const std::vector<ffmpeg::StreamInfo>& streams_;
  const bool normalize_timestamps_;
  std::unique_ptr<AVFormatContext, FormatDeleter> context_;
  std::unique_ptr<AVBSFContext, BitstreamFilterDeleter> audio_filter_;
  std::size_t audio_index_ = 0;
  std::optional<std::int64_t> origin_;
  AVRational origin_base_{0, 1};
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_RECORDER_MP4_FILE_WRITER_H_
