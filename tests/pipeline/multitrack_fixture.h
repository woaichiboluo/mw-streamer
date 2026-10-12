#ifndef MW_STREAMER_TESTS_PIPELINE_MULTITRACK_FIXTURE_H_
#define MW_STREAMER_TESTS_PIPELINE_MULTITRACK_FIXTURE_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"

namespace mw::streamer::test {

// Duplicate source tracks exercise selection before decoding or consumption.
// An optional second video marks later tracks as preferred to verify that
// default selection follows source order rather than disposition flags.
class MultiTrackFixture final {
 public:
  explicit MultiTrackFixture(const std::string& source,
                             bool second_audio_non_default = true,
                             bool duplicate_video = false)
      : path_(
            std::filesystem::current_path() /
            ("pipeline-multitrack-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".mkv")) {
    AVFormatContext* input = nullptr;
    ffmpeg::FfmpegException::throwIfError(
        avformat_open_input(&input, source.c_str(), nullptr, nullptr),
        "open multi-track fixture source");
    const auto close_input = [](AVFormatContext* context) {
      avformat_close_input(&context);
    };
    std::unique_ptr<AVFormatContext, decltype(close_input)> reader(input,
                                                                   close_input);
    ffmpeg::FfmpegException::throwIfError(
        avformat_find_stream_info(input, nullptr), "probe multi-track fixture");
    const int video =
        av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio =
        av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    ffmpeg::FfmpegException::throwIfError(video, "select fixture video");
    ffmpeg::FfmpegException::throwIfError(audio, "select fixture audio");
    AVFormatContext* output = nullptr;
    ffmpeg::FfmpegException::throwIfError(
        avformat_alloc_output_context2(&output, nullptr, "matroska",
                                       path_.generic_u8string().c_str()),
        "allocate multi-track fixture");
    const auto close_output = [](AVFormatContext* context) {
      if (context->pb) avio_closep(&context->pb);
      avformat_free_context(context);
    };
    std::unique_ptr<AVFormatContext, decltype(close_output)> writer(
        output, close_output);
    std::vector<int> source_indices{video, audio, audio};
    if (duplicate_video) source_indices.push_back(video);
    for (const int source_index : source_indices) {
      auto* stream = avformat_new_stream(output, nullptr);
      if (!stream) throw std::bad_alloc();
      ffmpeg::FfmpegException::throwIfError(
          avcodec_parameters_copy(stream->codecpar,
                                  input->streams[source_index]->codecpar),
          "copy multi-track fixture parameters");
      stream->codecpar->codec_tag = 0;
      stream->time_base = input->streams[source_index]->time_base;
      stream->disposition = stream->index == 2 && second_audio_non_default
                                ? AV_DISPOSITION_COMMENT
                                : 0;
      if (duplicate_video) {
        stream->disposition =
            stream->index < 2 ? AV_DISPOSITION_COMMENT : AV_DISPOSITION_DEFAULT;
      }
    }
    ffmpeg::FfmpegException::throwIfError(
        avio_open(&output->pb, path_.generic_u8string().c_str(),
                  AVIO_FLAG_WRITE),
        "open multi-track fixture output");
    ffmpeg::FfmpegException::throwIfError(
        avformat_write_header(output, nullptr),
        "write multi-track fixture header");
    for (;;) {
      ffmpeg::Packet packet;
      const int status = av_read_frame(input, packet.get());
      if (status == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(status, "read multi-track fixture");
      if (packet->stream_index != video && packet->stream_index != audio)
        continue;
      for (std::size_t index = 0; index < source_indices.size(); ++index) {
        if (source_indices[index] != packet->stream_index) continue;
        auto copied = packet.Ref();
        av_packet_rescale_ts(copied.get(),
                             input->streams[packet->stream_index]->time_base,
                             output->streams[index]->time_base);
        copied->stream_index = static_cast<int>(index);
        ffmpeg::FfmpegException::throwIfError(
            av_interleaved_write_frame(output, copied.get()),
            "write multi-track fixture packet");
      }
    }
    ffmpeg::FfmpegException::throwIfError(av_write_trailer(output),
                                          "finish multi-track fixture");
  }
  ~MultiTrackFixture() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string path() const { return path_.generic_u8string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace mw::streamer::test

#endif  // MW_STREAMER_TESTS_PIPELINE_MULTITRACK_FIXTURE_H_
