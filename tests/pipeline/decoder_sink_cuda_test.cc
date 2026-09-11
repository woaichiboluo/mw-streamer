#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/decoder/decoder_sink.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

#include "mw/ffmpeg/input_format_context.h"
#include "mw/processor/transform_processor_sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::DecoderSink;
using mw::streamer::decoder::DecoderSinkConfig;
using mw::streamer::media::FrameReady;
using mw::streamer::media::FrameStreamsReady;
using mw::streamer::media::StreamEnded;
using mw::streamer::media::StreamEndReason;
using mw::streamer::media::TimelineReset;
using mw::streamer::processor::TransformProcessorSink;
using mw::streamer::sink::PacketSinkState;
using mw::streamer::sink::Sink;
using mw::streamer::sink::SinkMediaType;
namespace ffmpeg = mw::streamer::ffmpeg;

struct Recording {
  std::vector<ffmpeg::Frame> frames;
  int sources = 0;
  int ends = 0;
  int stops = 0;
};

class CudaRecorder final : public Sink {
 public:
  explicit CudaRecorder(std::string id, Recording& recording)
      : Sink(std::move(id), SinkMediaType::kFrame), recording_(recording) {}

  void OnStreamsReady(const FrameStreamsReady& streams) override {
    if (!streams.hardware_context ||
        streams.hardware_context->type() != AV_HWDEVICE_TYPE_CUDA) {
      throw std::runtime_error("CUDA输入没有硬件上下文");
    }
    hardware_context_ = streams.hardware_context;
    ++recording_.sources;
  }

  void OnAudioFrame(const FrameReady&) override {
    throw std::runtime_error("纯视频测试收到音频帧");
  }

  void OnVideoFrame(const FrameReady& frame) override {
    if (frame.generation != 1 || frame.frame->format != AV_PIX_FMT_CUDA ||
        !hardware_context_->IsCompatible(*frame.frame.get())) {
      throw std::runtime_error("解码输出帧与输入CUDA上下文不兼容");
    }
    recording_.frames.push_back(frame.frame.Ref());
  }

  void OnTimelineReset(const TimelineReset&) override {
    throw std::runtime_error("单代次测试收到时间线重置");
  }

  void OnInputEnded(const StreamEnded& end) override {
    if (end.generation != 1 || end.reason != StreamEndReason::kEof) {
      throw std::runtime_error("CUDA输入未正常结束");
    }
    ++recording_.ends;
  }

  void Stop() noexcept override {
    if (!stopped_) {
      stopped_ = true;
      hardware_context_ = nullptr;
      ++recording_.stops;
    }
  }

 private:
  Recording& recording_;
  const ffmpeg::HardwareContext* hardware_context_ = nullptr;
  bool stopped_ = false;
};

}  // namespace

TEST_CASE("DecoderSink CUDA frames outlive the decoder context") {
  ffmpeg::InputFormatContext input(std::string(MW_DECODER_SINK_TEST_DATA_DIR) +
                                   "/h264_aac.mp4");
  input.FindStreamInfo();
  std::vector<ffmpeg::StreamInfo> streams;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    const auto* stream = input->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      streams.push_back({stream->index,
                         ffmpeg::CodecParameters(*stream->codecpar),
                         stream->time_base});
    }
  }
  REQUIRE(streams.size() == 1);
  const auto* parameters = streams.front().codec_parameters.get();
  const int source_width = parameters->width;
  const int source_height = parameters->height;
  Recording recording;
  auto sink = std::make_unique<DecoderSink>("sink", DecoderSinkConfig{});
  SECTION("direct decoder output") {
    sink->AddSink(std::make_unique<CudaRecorder>("recording", recording));
  }
  SECTION("processor without a callback preserves CUDA storage") {
    auto processor = std::make_unique<TransformProcessorSink>(
        "processor", MwStreamerStreamingProcessorCallbacks{});
    processor->AddSink(std::make_unique<CudaRecorder>("recording", recording));
    sink->AddSink(std::move(processor));
  }
  sink->OnStreamsReady({1, streams});
  ffmpeg::Packet packet;
  while (input.ReadPacket(packet)) {
    if (packet->stream_index == streams.front().stream_index) {
      sink->OnPacket({1, packet.Ref()});
    }
    packet.Unref();
  }
  sink->OnInputEnded({1, StreamEndReason::kEof});
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (sink->state() != PacketSinkState::kEnded &&
         sink->state() != PacketSinkState::kFailed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  const auto state = sink->state();
  const auto error = sink->error();
  sink->Stop();
  sink.reset();

  INFO(error);
  REQUIRE(state == PacketSinkState::kEnded);
  CHECK(recording.sources == 1);
  CHECK(recording.ends == 1);
  CHECK(recording.stops == 1);
  REQUIRE(recording.frames.size() == 20);
  for (const auto& frame : recording.frames) {
    ffmpeg::Frame downloaded;
    downloaded->format = AV_PIX_FMT_NV12;
    REQUIRE(av_hwframe_transfer_data(downloaded.get(), frame.get(), 0) >= 0);
    CHECK(downloaded->width == source_width);
    CHECK(downloaded->height == source_height);
    CHECK(downloaded->format == AV_PIX_FMT_NV12);
    CHECK(downloaded->data[0] != nullptr);
    CHECK(downloaded->data[1] != nullptr);
  }
}
