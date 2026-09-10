#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "mw/decoder/decoder_sink.h"
#include "mw/encoder/encoder_sink.h"
#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/input/zlm_input.h"
#include "mw/output/remux_sink.h"
#include "mw/pipeline/pipeline.h"
#include "mw/processor/transform_processor_sink.h"
#include "mw/synchronizer/synchronizer_sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::DecoderSink;
using mw::streamer::decoder::DecoderSinkConfig;
using mw::streamer::encoder::EncoderSink;
using mw::streamer::encoder::EncoderSinkConfig;
using mw::streamer::encoder::EncoderSinkState;
using mw::streamer::input::InputState;
using mw::streamer::input::ZlmInput;
using mw::streamer::input::ZlmInputConfig;
using mw::streamer::media::FrameReady;
using mw::streamer::media::FrameStreamsReady;
using mw::streamer::media::PacketReady;
using mw::streamer::media::StreamEnded;
using mw::streamer::media::StreamsReady;
using mw::streamer::media::TimelineReset;
using mw::streamer::output::RemuxSink;
using mw::streamer::output::RemuxSinkConfig;
using mw::streamer::processor::TransformProcessorSink;
using mw::streamer::sink::PacketSinkState;
using mw::streamer::sink::Sink;
using mw::streamer::sink::SinkMediaType;
using mw::streamer::synchronizer::SynchronizerSink;
using mw::streamer::synchronizer::SynchronizerSinkConfig;
using mw::streamer::synchronizer::SynchronizerSinkState;
using namespace mw::streamer::pipeline;

class TestDirectory final {
 public:
  TestDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("mw-encoder-pipeline-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

std::unique_ptr<ZlmInput> MakeInput() {
  ZlmInputConfig config;
  config.url = std::string(MW_ENCODER_PIPELINE_TEST_DATA_DIR) + "/h264_aac.mp4";
  config.reconnect_policy.max_retries = 0;
  return std::make_unique<ZlmInput>(std::move(config));
}

std::unique_ptr<DecoderSink> MakeDecoder() {
  DecoderSinkConfig config;
  config.video_decoder.backend =
      mw::streamer::decoder::VideoDecoderBackend::kSoftware;
  return std::make_unique<DecoderSink>("decoder-1", config);
}

EncoderSinkConfig EncoderConfig() {
  EncoderSinkConfig config;
  config.video_encoder.encoder_name = "libx264";
  config.video_encoder.frame_rate = {10, 1};
  config.video_encoder.properties = {{"preset", "ultrafast"}};
  config.audio_encoder.encoder_name = "aac";
  return config;
}

SynchronizerSinkConfig SynchronizerConfig() { return {}; }

std::vector<std::filesystem::path> Recordings(
    const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if ((entry.path().parent_path() == directory &&
         entry.path().extension() == ".mp4") ||
        entry.path().filename() == "index.m3u8") {
      paths.push_back(entry.path());
    }
  }
  return paths;
}

class FatalTarget final : public Sink {
 public:
  FatalTarget() : Sink("fatal-target", SinkMediaType::kPacket) {}
  void OnStreamsReady(const StreamsReady&) noexcept override {}
  void OnPacket(const PacketReady&) noexcept override {
    ReportFatalError("encoder downstream fatal");
  }
  void OnTimelineReset(const TimelineReset&) noexcept override {}
  void OnInputEnded(const StreamEnded&) noexcept override {}
  void Stop() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    condition_.notify_all();
  }
  PacketSinkState state() const noexcept { return PacketSinkState::kFailed; }
  bool WaitStopped() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 10s, [this] { return stopped_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopped_ = false;
};

class PacketTrace final : public Sink {
 public:
  PacketTrace() : Sink("packet-trace", SinkMediaType::kPacket) {}
  void OnStreamsReady(const StreamsReady& ready) noexcept override {
    streams = ready.streams;
  }
  void OnPacket(const PacketReady& ready) noexcept override {
    packets.push_back(ready.packet.Ref());
  }
  void OnTimelineReset(const TimelineReset&) noexcept override {}
  void OnInputEnded(const StreamEnded&) noexcept override {}
  void Stop() noexcept override {}
  PacketSinkState state() const noexcept { return PacketSinkState::kRunning; }

  std::vector<mw::streamer::ffmpeg::StreamInfo> streams;
  std::vector<mw::streamer::ffmpeg::Packet> packets;
};

struct SlowProcessorState {
  int video_calls = 0;
  std::atomic<bool> blocked{false};
  std::atomic<bool> recovered{false};
  std::mutex mutex;
  std::condition_variable condition;
  bool standby_observed = false;
};

MwStreamerStreamingProcessorCallbacks SlowProcessorCallbacks(
    SlowProcessorState& state) {
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start =
      [](const MwStreamerStreamingProcessorStartRequest* request, void*) {
        if (!request->video_output_size) {
          return kMwStreamerProcessorStartFailed;
        }
        request->video_output_size->width = request->source_info->video.width;
        request->video_output_size->height = request->source_info->video.height;
        return kMwStreamerProcessorStartSuccess;
      };
  callbacks.process_video =
      [](const MwStreamerStreamingVideoProcessRequest* request, void* context) {
        auto& state = *static_cast<SlowProcessorState*>(context);
        if (++state.video_calls == 4) {
          std::unique_lock<std::mutex> lock(state.mutex);
          state.blocked = true;
          // Buffered business frames can outlast a fixed pause. Resume only
          // after standby is observed, with a deadline if standby is broken.
          const bool observed = state.condition.wait_for(
              lock, 3s, [&state] { return state.standby_observed; });
          state.blocked = false;
          state.recovered = observed;
        }
        for (std::uint32_t plane = 0;
             plane < request->output->storage.linear.plane_count; ++plane) {
          const auto& view = request->output->storage.linear.planes[plane];
          auto* data = reinterpret_cast<std::uint8_t*>(view.address);
          for (std::uint32_t row = 0; row < view.row_count; ++row) {
            std::memset(data + row * view.stride_bytes,
                        plane == 0 ? 0x21 : 0x80, view.row_bytes);
          }
        }
      };
  return callbacks;
}

struct FrameStamp {
  std::int64_t pts;
  AVRational time_base;
  int samples = 0;
};

class ScheduledFrameTrace final : public Sink {
 public:
  explicit ScheduledFrameTrace(SlowProcessorState* processor = nullptr)
      : Sink("scheduled-frame-trace", SinkMediaType::kFrame),
        processor_(processor) {}

  void OnStreamsReady(const FrameStreamsReady&) override {}
  void OnAudioFrame(const FrameReady& ready) override {
    const auto& frame = ready.frame;
    audio.push_back({frame->pts, frame->time_base, frame->nb_samples});
    if (processor_ && processor_->blocked) ++audio_during_pause;
  }
  void OnVideoFrame(const FrameReady& ready) override {
    const auto& frame = ready.frame;
    video.push_back({frame->pts, frame->time_base});
    if (!processor_) return;
    bool processed = true;
    for (int row = 0; row < frame->height && processed; ++row) {
      for (int column = 0; column < frame->width; ++column) {
        if (frame->data[0][row * frame->linesize[0] + column] != 0x21) {
          processed = false;
          break;
        }
      }
    }
    if (processor_->blocked) {
      ++video_during_pause;
      if (!processed) {
        ++standby_during_pause;
        {
          std::lock_guard<std::mutex> lock(processor_->mutex);
          processor_->standby_observed = true;
        }
        processor_->condition.notify_one();
      }
    }
    if (processor_->recovered && processed) ++processed_after_pause;
  }
  void OnTimelineReset(const TimelineReset&) override {}
  void OnInputEnded(const StreamEnded&) override { ended = true; }
  void Stop() noexcept override {}

  std::vector<FrameStamp> audio;
  std::vector<FrameStamp> video;
  int audio_during_pause = 0;
  int video_during_pause = 0;
  int standby_during_pause = 0;
  int processed_after_pause = 0;
  bool ended = false;

 private:
  SlowProcessorState* processor_;
};

int CheckDecodableRecording(const std::filesystem::path& path) {
  INFO(path.string());
  mw::streamer::ffmpeg::InputFormatContext input(path.string());
  input.FindStreamInfo();
  REQUIRE(input->nb_streams == 2);
  std::vector<std::unique_ptr<mw::streamer::ffmpeg::CodecContext>> decoders;
  for (unsigned int index = 0; index < input->nb_streams; ++index) {
    const auto* parameters = input->streams[index]->codecpar;
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    REQUIRE(codec);
    auto decoder = std::make_unique<mw::streamer::ffmpeg::CodecContext>(codec);
    REQUIRE(avcodec_parameters_to_context(decoder->get(), parameters) == 0);
    REQUIRE(avcodec_open2(decoder->get(), codec, nullptr) == 0);
    decoders.push_back(std::move(decoder));
  }
  int videos = 0;
  int audios = 0;
  const auto receive = [&](AVCodecContext* decoder) {
    mw::streamer::ffmpeg::Frame frame;
    for (;;) {
      const int result = avcodec_receive_frame(decoder, frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result == 0);
      if (decoder->codec_type == AVMEDIA_TYPE_VIDEO) {
        CHECK(frame->width == 64);
        CHECK(frame->height == 64);
        ++videos;
      } else {
        CHECK(frame->nb_samples > 0);
        ++audios;
      }
      frame.Unref();
    }
  };
  mw::streamer::ffmpeg::Packet packet;
  while (input.ReadPacket(packet)) {
    auto* decoder = decoders.at(packet->stream_index)->get();
    REQUIRE(avcodec_send_packet(decoder, packet.get()) == 0);
    receive(decoder);
    packet.Unref();
  }
  for (const auto& decoder : decoders) {
    REQUIRE(avcodec_send_packet(decoder->get(), nullptr) == 0);
    receive(decoder->get());
  }
  CHECK(videos > 0);
  CHECK(audios > 0);
  return videos;
}

}  // namespace

TEST_CASE("新Pipeline通过Processor同步和Encoder一次编码输出两个独立录像") {
  TestDirectory directory;
  Pipeline pipeline(MakeInput());
  auto decoder = MakeDecoder();
  auto processor = std::make_unique<TransformProcessorSink>(
      "processor", mw::streamer::processor::StreamingProcessorConfig{""},
      MwStreamerStreamingProcessorCallbacks{});
  auto encoder = std::make_unique<EncoderSink>("encoder", EncoderConfig());
  auto* encoding = encoder.get();
  std::vector<RemuxSink*> outputs;
  for (const auto* name : {"first.mp4", "second.m3u8"}) {
    RemuxSinkConfig config;
    config.target = (directory.path() / name).string();
    auto output = std::make_unique<RemuxSink>(name, config);
    outputs.push_back(output.get());
    encoder->AddSink(std::move(output));
  }
  auto trace = std::make_unique<PacketTrace>();
  const auto* encoded = trace.get();
  encoder->AddSink(std::move(trace));
  auto synchronizer =
      std::make_unique<SynchronizerSink>("synchronizer", SynchronizerConfig());
  auto frame_trace = std::make_unique<ScheduledFrameTrace>();
  const auto* scheduled = frame_trace.get();
  synchronizer->AddSink(std::move(frame_trace));
  synchronizer->AddSink(std::move(encoder));
  processor->AddSink(std::move(synchronizer));
  decoder->AddSink(std::move(processor));
  pipeline.AddSink(std::move(decoder));
  const auto baseline = pipeline.GetPerformance();
  pipeline.Start();
  REQUIRE(WaitUntil([&] {
    return encoding->state() == EncoderSinkState::kEnded ||
           encoding->state() == EncoderSinkState::kFailed;
  }));
  INFO(encoding->error());
  REQUIRE(encoding->state() == EncoderSinkState::kEnded);
  for (const auto* output : outputs) {
    REQUIRE(WaitUntil([&] {
      return output->state() == PacketSinkState::kEnded ||
             output->state() == PacketSinkState::kFailed;
    }));
    INFO(output->error());
    REQUIRE(output->state() == PacketSinkState::kEnded);
  }
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
  int audio_index = -1;
  int video_index = -1;
  AVRational video_time_base{0, 1};
  for (const auto& stream : encoded->streams) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_index = stream.stream_index;
    } else {
      video_index = stream.stream_index;
      video_time_base = stream.time_base;
    }
  }
  int encoded_audios = 0;
  int encoded_videos = 0;
  std::vector<std::string> audio_payloads;
  int audios_before_key = 0;
  bool first_key = false;
  bool negative_audio_timestamp = false;
  std::vector<std::int64_t> encoded_video_pts;
  for (const auto& packet : encoded->packets) {
    if (packet->stream_index == video_index) {
      ++encoded_videos;
      encoded_video_pts.push_back(packet->pts);
      first_key |= (packet->flags & AV_PKT_FLAG_KEY) != 0;
    } else if (packet->stream_index == audio_index) {
      ++encoded_audios;
      audio_payloads.emplace_back(reinterpret_cast<const char*>(packet->data),
                                  packet->size);
      if (!first_key) ++audios_before_key;
      negative_audio_timestamp |= packet->pts < 0 || packet->dts < 0;
    }
  }
  CHECK(scheduled->ended);
  using mw::streamer::performance::PerformanceType;
  const auto performance = pipeline.GetPerformance().WithRatesSince(baseline);
  const auto input_stats = performance.Find(PerformanceType::kInput);
  const auto decoder_stats = performance.Find(PerformanceType::kVideoDecoder);
  const auto processor_stats =
      performance.Find(PerformanceType::kVideoProcessor);
  const auto encoder_stats = performance.Find(PerformanceType::kVideoEncoder);
  const auto remux_stats = performance.Find(PerformanceType::kRemux);
  REQUIRE(input_stats.size() == 1);
  REQUIRE(decoder_stats.size() == 1);
  REQUIRE(processor_stats.size() == 1);
  REQUIRE(encoder_stats.size() == 1);
  REQUIRE(remux_stats.size() == 2);
  CHECK(input_stats[0].operation->output_count > 0);
  CHECK(decoder_stats[0].operation->output_count ==
        processor_stats[0].operation->input_count);
  CHECK(processor_stats[0].operation->completed_calls == 0);
  CHECK(encoder_stats[0].operation->input_count == scheduled->video.size());
  CHECK(encoder_stats[0].operation->output_count == encoded_videos);
  CHECK(encoder_stats[0].operation->input_per_second > 0);
  CHECK(encoder_stats[0].operation->in_flight == 0);
  CHECK(encoder_stats[0].node->id == "encoder");
  CHECK(remux_stats[0].node->id == "first.mp4");
  CHECK(remux_stats[1].node->id == "second.m3u8");
  for (const auto& match : remux_stats) {
    CHECK(match.operation->input_count == encoded->packets.size());
    CHECK(match.operation->output_count == encoded->packets.size());
    CHECK(match.operation->failed_calls == 0);
  }
  REQUIRE(scheduled->video.size() >= 2);
  REQUIRE(encoded_video_pts.size() == scheduled->video.size());
  const auto& first_video = scheduled->video.front();
  const auto& last_video = scheduled->video.back();
  // EOF delivery races the real-time output clock; a final repeat before the
  // EOF notification is valid, but output must remain near this 2-second clip.
  const auto video_duration_ms =
      av_rescale_q(last_video.pts, last_video.time_base, AVRational{1, 1000}) -
      av_rescale_q(first_video.pts, first_video.time_base,
                   AVRational{1, 1000}) +
      100;
  CHECK(video_duration_ms >= 1900);
  CHECK(video_duration_ms <= 2300);
  for (std::size_t index = 0; index < scheduled->video.size(); ++index) {
    INFO(index);
    const auto& frame = scheduled->video[index];
    CHECK(av_compare_ts(encoded_video_pts[index], video_time_base, frame.pts,
                        frame.time_base) == 0);
    CHECK(av_compare_ts(frame.pts - first_video.pts, frame.time_base,
                        static_cast<std::int64_t>(index),
                        AVRational{1, 10}) == 0);
  }
  CHECK(encoded_audios >= 94);
  REQUIRE(audios_before_key > 0);
  REQUIRE(negative_audio_timestamp);
  const auto files = Recordings(directory.path());
  REQUIRE(files.size() == 2);
  for (const auto& file : files) {
    INFO(file.string());
    mw::streamer::ffmpeg::InputFormatContext input(file.string());
    input.FindStreamInfo();
    REQUIRE(input->nb_streams == 2);
    CHECK(input->duration >= 1700000);
    CHECK(input->duration <= 2300000);
    int videos = 0;
    std::vector<FrameStamp> recorded_video;
    std::vector<std::string> recorded_audio_payloads;
    mw::streamer::ffmpeg::Packet packet;
    while (input.ReadPacket(packet)) {
      const auto& codec = *input->streams[packet->stream_index]->codecpar;
      if (codec.codec_type == AVMEDIA_TYPE_VIDEO) {
        CHECK(codec.codec_id == AV_CODEC_ID_H264);
        CHECK(codec.width == 64);
        CHECK(codec.height == 64);
        recorded_video.push_back(
            {packet->pts, input->streams[packet->stream_index]->time_base});
        ++videos;
      } else if (codec.codec_type == AVMEDIA_TYPE_AUDIO) {
        CHECK(codec.codec_id == AV_CODEC_ID_AAC);
        recorded_audio_payloads.emplace_back(
            reinterpret_cast<const char*>(packet->data), packet->size);
      }
      packet.Unref();
    }
    CHECK(videos == encoded_videos);
    REQUIRE(recorded_video.size() == scheduled->video.size());
    for (std::size_t index = 0; index < recorded_video.size(); ++index) {
      INFO(index);
      const auto& packet = recorded_video[index];
      const auto& frame = scheduled->video[index];
      CHECK(av_compare_ts(packet.pts - recorded_video.front().pts,
                          packet.time_base, frame.pts - first_video.pts,
                          frame.time_base) == 0);
    }
    CHECK(recorded_audio_payloads.size() == audio_payloads.size());
    CHECK((recorded_audio_payloads == audio_payloads));
  }
}

TEST_CASE("慢Processor期间同步独立备播并持续音频且恢复后两路录像可解码") {
  TestDirectory directory;
  SlowProcessorState slow_processor;
  Pipeline pipeline(MakeInput());
  auto decoder = MakeDecoder();
  auto processor = std::make_unique<TransformProcessorSink>(
      "processor", mw::streamer::processor::StreamingProcessorConfig{""},
      SlowProcessorCallbacks(slow_processor));
  auto sync_config = SynchronizerConfig();
  sync_config.standby_timeout = 100ms;
  auto synchronizer =
      std::make_unique<SynchronizerSink>("synchronizer", sync_config);
  const auto* synchronizing = synchronizer.get();
  auto frame_trace = std::make_unique<ScheduledFrameTrace>(&slow_processor);
  const auto* scheduled = frame_trace.get();
  synchronizer->AddSink(std::move(frame_trace));

  auto encoder = std::make_unique<EncoderSink>("encoder", EncoderConfig());
  const auto* encoding = encoder.get();
  RemuxSinkConfig encoded_config;
  encoded_config.target = (directory.path() / "encoded.mp4").string();
  auto output = std::make_unique<RemuxSink>("output", encoded_config);
  const auto* recording = output.get();
  encoder->AddSink(std::move(output));
  synchronizer->AddSink(std::move(encoder));
  processor->AddSink(std::move(synchronizer));
  decoder->AddSink(std::move(processor));
  pipeline.AddSink(std::move(decoder));

  RemuxSinkConfig raw_config;
  raw_config.target = (directory.path() / "original.mp4").string();
  auto raw = std::make_unique<RemuxSink>("raw", raw_config);
  const auto* raw_recording = raw.get();
  pipeline.AddSink(std::move(raw));
  pipeline.Start();
  REQUIRE(WaitUntil([&] {
    return (recording->state() == PacketSinkState::kEnded &&
            synchronizing->state() == SynchronizerSinkState::kEnded &&
            encoding->state() == EncoderSinkState::kEnded) ||
           recording->state() == PacketSinkState::kFailed ||
           synchronizing->state() == SynchronizerSinkState::kFailed ||
           encoding->state() == EncoderSinkState::kFailed;
  }));
  INFO(synchronizing->error());
  INFO(encoding->error());
  INFO(recording->error());
  REQUIRE(recording->state() == PacketSinkState::kEnded);
  REQUIRE(WaitUntil([&] {
    return raw_recording->state() == PacketSinkState::kEnded ||
           raw_recording->state() == PacketSinkState::kFailed;
  }));
  INFO(raw_recording->error());
  REQUIRE(raw_recording->state() == PacketSinkState::kEnded);
  REQUIRE(synchronizing->state() == SynchronizerSinkState::kEnded);
  REQUIRE(encoding->state() == EncoderSinkState::kEnded);
  pipeline.Stop();

  INFO("Processor等待备播帧的超时为3秒");
  CHECK(slow_processor.recovered.load());
  CHECK(scheduled->ended);
  CHECK(scheduled->video_during_pause >= 2);
  CHECK(scheduled->audio_during_pause >= 5);
  CHECK(scheduled->standby_during_pause >= 1);
  CHECK(scheduled->processed_after_pause >= 1);
  REQUIRE(scheduled->video.size() >= 10);
  REQUIRE(scheduled->audio.size() >= 50);
  for (std::size_t index = 1; index < scheduled->video.size(); ++index) {
    const auto& previous = scheduled->video[index - 1];
    const auto& current = scheduled->video[index];
    CHECK(av_compare_ts(current.pts, current.time_base,
                        previous.pts + av_rescale_q(1, AVRational{1, 10},
                                                    previous.time_base),
                        previous.time_base) == 0);
  }
  for (std::size_t index = 1; index < scheduled->audio.size(); ++index) {
    const auto& previous = scheduled->audio[index - 1];
    const auto& current = scheduled->audio[index];
    CHECK(av_compare_ts(current.pts, current.time_base,
                        previous.pts + av_rescale_q(previous.samples,
                                                    AVRational{1, 48000},
                                                    previous.time_base),
                        previous.time_base) == 0);
  }
  const auto files = Recordings(directory.path());
  REQUIRE(files.size() == 2);
  for (const auto& file : files) {
    const int videos = CheckDecodableRecording(file);
    if (file.filename().string().find("original_") == 0) {
      CHECK(videos == 20);
    } else {
      CHECK(videos == static_cast<int>(scheduled->video.size()));
    }
  }
}

TEST_CASE("Encoder下游fatal穿过同步层Processor和Decoder自动停止Pipeline") {
  auto input = MakeInput();
  const auto* source = input.get();
  Pipeline pipeline(std::move(input));
  auto decoder = MakeDecoder();
  const auto* decoding = decoder.get();
  auto processor = std::make_unique<TransformProcessorSink>(
      "processor", mw::streamer::processor::StreamingProcessorConfig{""},
      MwStreamerStreamingProcessorCallbacks{});
  auto encoder = std::make_unique<EncoderSink>("encoder", EncoderConfig());
  const auto* encoding = encoder.get();
  auto target = std::make_unique<FatalTarget>();
  auto* failed_output = target.get();
  encoder->AddSink(std::move(target));
  auto synchronizer =
      std::make_unique<SynchronizerSink>("synchronizer", SynchronizerConfig());
  synchronizer->AddSink(std::move(encoder));
  processor->AddSink(std::move(synchronizer));
  decoder->AddSink(std::move(processor));
  pipeline.AddSink(std::move(decoder));
  pipeline.Start();
  // Wait for automatic cleanup before invoking Stop, otherwise a missing
  // fatal route or a self-join deadlock could be hidden by manual shutdown.
  REQUIRE(failed_output->WaitStopped());
  CHECK(source->state() == InputState::kStopped);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.error() == "encoder downstream fatal");
  CHECK(encoding->state() == EncoderSinkState::kFailed);
  CHECK(decoding->state() == PacketSinkState::kFailed);
  pipeline.Stop();
}

TEST_CASE("Encoder异步配置失败不影响Pipeline原始录像旁路") {
  TestDirectory directory;
  Pipeline pipeline(MakeInput());
  auto decoder = MakeDecoder();
  auto config = EncoderConfig();
  config.video_encoder.encoder_name = "mw_missing_encoder";
  auto encoder = std::make_unique<EncoderSink>("encoder", config);
  const auto* encoding = encoder.get();
  RemuxSinkConfig failed_config;
  failed_config.target = (directory.path() / "encoded.mp4").string();
  encoder->AddSink(std::make_unique<RemuxSink>("remux-4", failed_config));
  decoder->AddSink(std::move(encoder));
  pipeline.AddSink(std::move(decoder));
  RemuxSinkConfig raw_config;
  raw_config.target = (directory.path() / "original.mp4").string();
  auto raw = std::make_unique<RemuxSink>("raw", raw_config);
  const auto* recording = raw.get();
  pipeline.AddSink(std::move(raw));
  pipeline.Start();
  REQUIRE(WaitUntil(
      [&] { return encoding->state() == EncoderSinkState::kFailed; }));
  CHECK_FALSE(encoding->error().empty());
  REQUIRE(
      WaitUntil([&] { return recording->state() == PacketSinkState::kEnded; }));
  CHECK(pipeline.state() != PipelineState::kFailed);
  pipeline.Stop();
  const auto files = Recordings(directory.path());
  REQUIRE(files.size() == 1);
  CHECK(files.front().filename().string().find("original_") == 0);
}
