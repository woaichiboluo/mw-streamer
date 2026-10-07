#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {
namespace ffmpeg = mw::streamer::ffmpeg;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr AVRational kNanoseconds{1, 1000000000};

struct Errors {
  std::mutex mutex;
  std::exception_ptr error;
  std::atomic<bool> failed{false};

  template <typename F>
  void Capture(F&& action) noexcept {
    if (failed.load()) return;
    try {
      action();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!error) error = std::current_exception();
      failed.store(true);
    }
  }

  void Rethrow() {
    std::lock_guard<std::mutex> lock(mutex);
    if (error) std::rethrow_exception(error);
  }
};

struct FormatCloser {
  void operator()(AVFormatContext* format) const noexcept {
    if (format->pb) avio_closep(&format->pb);
    avformat_free_context(format);
  }
};

struct Options {
  AVDictionary* value = nullptr;
  ~Options() { av_dict_free(&value); }
};

// Manual real-time experiment: callbacks never wait for an encoder. The bounded
// queue discards its oldest pending frame if encoding cannot keep up, and every
// such discard is reported separately from output scheduler misses.
class AsyncEncoder {
 public:
  AsyncEncoder(std::string name, std::size_t capacity, Errors& errors,
               std::function<void(const ffmpeg::Frame&)> encode)
      : name_(std::move(name)),
        capacity_(capacity),
        errors_(errors),
        encode_(std::move(encode)) {
    thread_ = std::thread([this] { Run(); });
  }

  ~AsyncEncoder() { Close(); }

  void Submit(const ffmpeg::Frame& frame) {
    auto retained = frame.Ref();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closing_) throw std::logic_error("Encoder queue is already closed");
      if (frames_.size() == capacity_) {
        frames_.pop_front();
        ++dropped_;
      }
      frames_.push_back(std::move(retained));
      ++submitted_;
      peak_ = std::max(peak_, frames_.size());
    }
    wake_.notify_one();
  }

  void Close() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  void Report() const {
    // Called after producers stop and Close joins the worker.
    fmt::print(
        "ASYNC {}: submitted={}, encoded={}, queue_dropped={}, peak_queue={}, "
        "capacity={}, encode_work_ms={:.3f}, max_encode_ms={:.3f}\n",
        name_, submitted_, encoded_, dropped_, peak_, capacity_,
        work_ns_ / 1000000.0, max_work_ns_ / 1000000.0);
    if (submitted_ != encoded_ + dropped_ && !errors_.failed.load())
      throw std::runtime_error("Async encoder lost unaccounted frames");
  }

 private:
  void Run() noexcept {
    for (;;) {
      std::optional<ffmpeg::Frame> frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return closing_ || !frames_.empty(); });
        if (frames_.empty()) return;
        frame.emplace(std::move(frames_.front()));
        frames_.pop_front();
      }
      errors_.Capture([&] {
        const auto started = Clock::now();
        encode_(*frame);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                 started)
                .count();
        work_ns_ += elapsed;
        max_work_ns_ = std::max(max_work_ns_, elapsed);
        ++encoded_;
      });
    }
  }

  std::string name_;
  std::size_t capacity_;
  Errors& errors_;
  std::function<void(const ffmpeg::Frame&)> encode_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<ffmpeg::Frame> frames_;
  bool closing_ = false;
  std::size_t submitted_ = 0;
  std::size_t dropped_ = 0;
  std::size_t peak_ = 0;
  std::size_t encoded_ = 0;
  std::int64_t work_ns_ = 0;
  std::int64_t max_work_ns_ = 0;
  std::thread thread_;
};

class Recording {
 public:
  Recording(std::string path, const ffmpeg::HwDeviceContext& device, int fps)
      : path_(std::move(path)), device_(device.Ref()), fps_(fps) {}

  ~Recording() {
    // Processor and Input must have stopped before this object is destroyed.
    // Preserve a readable partial recording if an encoder callback failed.
    if (header_written_ && !trailer_attempted_) av_write_trailer(format_.get());
  }

  void Open(const std::vector<ffmpeg::StreamInfo>& streams) {
    const AVCodecParameters* video = nullptr;
    const AVCodecParameters* audio = nullptr;
    for (const auto& stream : streams) {
      const auto* parameters = stream.codec_parameters.get();
      if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) video = parameters;
      if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) audio = parameters;
    }
    if (!video || !audio)
      throw std::runtime_error("Record probe requires video and audio tracks");
    if (!device_.get() && video->format != AV_PIX_FMT_YUV420P)
      throw std::runtime_error("Record probe requires CPU YUV420P video");
    if (audio->ch_layout.nb_channels <= 0)
      throw std::runtime_error("Record probe requires a known audio layout");

    AVFormatContext* raw = nullptr;
    const int allocated =
        avformat_alloc_output_context2(&raw, nullptr, "mp4", path_.c_str());
    format_.reset(raw);
    ffmpeg::FfmpegException::throwIfError(allocated, "Allocate MP4 output");
    if (!format_) throw std::runtime_error("Cannot allocate MP4 output");
    // AAC priming or a later audio callback can yield leading negative PTS.
    // The muxer applies one common shift to all tracks, preserving A/V offset.
    format_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;

    const bool cuda = device_.get() != nullptr;
    const auto* video_codec =
        avcodec_find_encoder_by_name(cuda ? "hevc_nvenc" : "libx264");
    const auto* audio_codec = avcodec_find_encoder_by_name("aac");
    if (!video_codec || !audio_codec)
      throw std::runtime_error(
          "Record probe requires the selected video encoder and AAC");
    video_ = std::make_unique<ffmpeg::CodecContext>(video_codec);
    audio_ = std::make_unique<ffmpeg::CodecContext>(audio_codec);
    auto* v = video_->get();
    v->width = video->width;
    v->height = video->height;
    v->pix_fmt = cuda ? AV_PIX_FMT_CUDA : AV_PIX_FMT_YUV420P;
    v->time_base = {1, 90000};
    v->framerate = {fps_, 1};
    v->gop_size = fps_ * 2;
    v->max_b_frames = 0;
    v->sample_aspect_ratio = video->sample_aspect_ratio;
    v->color_range = video->color_range;
    v->colorspace = video->color_space;
    v->color_primaries = video->color_primaries;
    v->color_trc = video->color_trc;
    if (cuda) {
      if (video->format != AV_PIX_FMT_YUV420P &&
          video->format != AV_PIX_FMT_YUVJ420P)
        throw std::runtime_error(
            "CUDA record probe requires 8-bit 4:2:0 video");
      v->hw_device_ctx = av_buffer_ref(device_.get());
      if (!v->hw_device_ctx) throw std::bad_alloc();
      // NVENC needs a frames description when opened. Actual decoder frames
      // remain in their original pool on this same device; no copy is made.
      v->hw_frames_ctx = av_hwframe_ctx_alloc(device_.get());
      if (!v->hw_frames_ctx) throw std::bad_alloc();
      auto* frames =
          reinterpret_cast<AVHWFramesContext*>(v->hw_frames_ctx->data);
      frames->format = AV_PIX_FMT_CUDA;
      frames->sw_format = AV_PIX_FMT_NV12;
      frames->width = v->width;
      frames->height = v->height;
      ffmpeg::FfmpegException::throwIfError(
          av_hwframe_ctx_init(v->hw_frames_ctx),
          "Describe CUDA encoder frames");
      v->bit_rate = 30000000;
    }
    auto* a = audio_->get();
    a->sample_rate = 48000;
    a->sample_fmt = AV_SAMPLE_FMT_FLTP;
    a->time_base = {1, 48000};
    a->bit_rate = 128000;
    ffmpeg::FfmpegException::throwIfError(
        av_channel_layout_copy(&a->ch_layout, &audio->ch_layout),
        "Copy encoder audio layout");
    if (format_->oformat->flags & AVFMT_GLOBALHEADER) {
      v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ffmpeg::FfmpegException::throwIfError(
        av_opt_set(v->priv_data, "preset", cuda ? "p2" : "veryfast", 0),
        "Set video encoder preset");
    // Keep NVENC's default HQ tuning. P2 + LL measured substantially slower
    // than P2 + HQ on the same cached 7680x2160 NV12 input on this device.
    if (!cuda) {
      ffmpeg::FfmpegException::throwIfError(
          av_opt_set(v->priv_data, "tune", "zerolatency", 0),
          "Set x264 encoder tune");
    }
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(v, video_codec, nullptr), "Open video encoder");
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(a, audio_codec, nullptr), "Open AAC encoder");
    if (a->frame_size != 1024 ||
        !(audio_codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME))
      throw std::runtime_error(
          "AAC encoder must support 1024 and short EOS frames");
    video_stream_ = NewStream(v);
    if (cuda) video_stream_->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    audio_stream_ = NewStream(a);
    ffmpeg::FfmpegException::throwIfError(
        avio_open(&format_->pb, path_.c_str(), AVIO_FLAG_WRITE),
        "Open MP4 file");
    Options options;
    ffmpeg::FfmpegException::throwIfError(
        av_dict_set(&options.value, "movflags", "+faststart", 0),
        "Enable MP4 faststart");
    ffmpeg::FfmpegException::throwIfError(
        avformat_write_header(format_.get(), &options.value),
        "Write MP4 header");
    header_written_ = true;
    fmt::print(
        "Recording {} {}x{} @{}fps, AAC 48000Hz FLTP {}ch; "
        "one shared PTS origin, no per-track reset or manual A/V offset.\n",
        video_codec->name, v->width, v->height, fps_, a->ch_layout.nb_channels);
  }

  void Video(const ffmpeg::Frame& source) {
    const auto* context = video_->get();
    if (source->format != context->pix_fmt || source->width != context->width ||
        source->height != context->height)
      throw std::runtime_error("Output video must retain its format and size");
    if (device_.get()) {
      if (!source->hw_frames_ctx)
        throw std::runtime_error(
            "CUDA frame needs its original frames context");
      const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
          source->hw_frames_ctx->data);
      if (frames->sw_format != AV_PIX_FMT_NV12 ||
          frames->device_ref->data != device_.get()->data)
        throw std::runtime_error("CUDA frame format or shared device changed");
    }
    auto frame = Prepare(source, context->time_base);
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    Encode(video_->get(), video_stream_, frame.get());
    ++video_frames_;
  }

  void Audio(const ffmpeg::Frame& source) {
    const auto* context = audio_->get();
    if (source->format != AV_SAMPLE_FMT_FLTP || source->sample_rate != 48000 ||
        av_channel_layout_compare(&source->ch_layout, &context->ch_layout) !=
            0 ||
        source->nb_samples <= 0 || source->nb_samples > context->frame_size)
      throw std::runtime_error(
          "Output audio must be 48000Hz FLTP, <=1024 samples");
    auto frame = Prepare(source, context->time_base);
    Encode(audio_->get(), audio_stream_, frame.get());
    audio_samples_ += source->nb_samples;
  }

  std::optional<Clock::time_point> StartedAt() {
    std::lock_guard<std::mutex> lock(origin_mutex_);
    return started_;
  }

  void Observe(const ffmpeg::Frame& source) {
    if (source->pts == AV_NOPTS_VALUE || source->time_base.num <= 0 ||
        source->time_base.den <= 0)
      throw std::runtime_error("Output frame needs a valid PTS and time base");
    std::lock_guard<std::mutex> lock(origin_mutex_);
    if (!origin_) {
      origin_ = av_rescale_q(source->pts, source->time_base, kNanoseconds);
      started_ = Clock::now();
    }
  }

  void Finish() {
    if (!header_written_) return;
    Encode(video_->get(), video_stream_, nullptr);
    Encode(audio_->get(), audio_stream_, nullptr);
    trailer_attempted_ = true;
    ffmpeg::FfmpegException::throwIfError(av_write_trailer(format_.get()),
                                          "Write MP4 trailer and faststart");
    ffmpeg::FfmpegException::throwIfError(avio_closep(&format_->pb),
                                          "Close MP4 file");
    fmt::print(
        "Saved {}: video_frames={}, audio_samples={}, shared_origin_ns={}\n",
        path_, video_frames_, audio_samples_, origin_.value_or(0));
  }

 private:
  AVStream* NewStream(const AVCodecContext* context) {
    auto* stream = avformat_new_stream(format_.get(), nullptr);
    if (!stream) throw std::bad_alloc();
    stream->time_base = context->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_from_context(stream->codecpar, context),
        "Copy encoder stream parameters");
    return stream;
  }

  ffmpeg::Frame Prepare(const ffmpeg::Frame& source, AVRational time_base) {
    Observe(source);
    const auto timestamp =
        av_rescale_q(source->pts, source->time_base, kNanoseconds);
    std::int64_t origin;
    {
      std::lock_guard<std::mutex> lock(origin_mutex_);
      origin = *origin_;
    }
    auto frame = source.Ref();
    frame->pts = av_rescale_q(timestamp - origin, kNanoseconds, time_base);
    frame->duration =
        av_rescale_q(source->duration, source->time_base, time_base);
    frame->time_base = time_base;
    frame->pkt_dts = AV_NOPTS_VALUE;
    return frame;
  }

  bool Receive(AVCodecContext* context, AVStream* stream) {
    ffmpeg::Packet packet;
    for (;;) {
      packet.Unref();
      const int result = avcodec_receive_packet(context, packet.get());
      if (result == AVERROR(EAGAIN)) return false;
      if (result == AVERROR_EOF) return true;
      ffmpeg::FfmpegException::throwIfError(result, "Receive encoded packet");
      // write_header may have changed stream time_base. Rescale both DTS and
      // PTS together; never infer timestamps from encoded frame counts.
      av_packet_rescale_ts(packet.get(), context->time_base, stream->time_base);
      packet->stream_index = stream->index;
      std::lock_guard<std::mutex> lock(mux_mutex_);
      ffmpeg::FfmpegException::throwIfError(
          av_interleaved_write_frame(format_.get(), packet.get()),
          "Mux encoded packet");
    }
  }

  void Encode(AVCodecContext* context, AVStream* stream, const AVFrame* frame) {
    int result = avcodec_send_frame(context, frame);
    if (result == AVERROR(EAGAIN)) {
      Receive(context, stream);
      result = avcodec_send_frame(context, frame);
    }
    ffmpeg::FfmpegException::throwIfError(
        result, frame ? "Send encoder frame" : "Flush encoder");
    const bool drained = Receive(context, stream);
    if (!frame && !drained)
      throw std::runtime_error("Encoder drain did not reach EOF");
  }

  std::string path_;
  ffmpeg::HwDeviceContext device_;
  int fps_;
  std::unique_ptr<AVFormatContext, FormatCloser> format_;
  std::unique_ptr<ffmpeg::CodecContext> video_;
  std::unique_ptr<ffmpeg::CodecContext> audio_;
  AVStream* video_stream_ = nullptr;
  AVStream* audio_stream_ = nullptr;
  std::mutex origin_mutex_;
  std::mutex mux_mutex_;
  std::optional<std::int64_t> origin_;
  std::optional<Clock::time_point> started_;
  bool header_written_ = false;
  bool trailer_attempted_ = false;
  std::int64_t video_frames_ = 0;
  std::int64_t audio_samples_ = 0;
};
}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 3 || argc > 7) {
    fmt::print(stderr,
               "Usage: {} URL output.mp4 [seconds=60] [cpu|cuda] [fps=30] "
               "[sync|async=async]\n",
               argv[0]);
    return 2;
  }
  try {
    const int seconds = argc > 3 ? std::stoi(argv[3]) : 60;
    if (seconds <= 0) throw std::invalid_argument("Duration must be positive");
    const std::string mode = argc > 4 ? argv[4] : "cpu";
    if (mode != "cpu" && mode != "cuda")
      throw std::invalid_argument("Device must be cpu or cuda");
    const int fps = argc > 5 ? std::stoi(argv[5]) : 30;
    if (fps <= 0 || fps > 240)
      throw std::invalid_argument("Frame rate must be between 1 and 240");
    const std::string encoding = argc > 6 ? argv[6] : "async";
    if (encoding != "sync" && encoding != "async")
      throw std::invalid_argument("Encoding must be sync or async");
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
    Errors errors;
    ffmpeg::HwDeviceContext device(mode == "cuda" ? ffmpeg::HwDeviceType::kCuda
                                                  : ffmpeg::HwDeviceType::kCpu);
    Recording recording(argv[2], device, fps);
    std::unique_ptr<AsyncEncoder> video_encoder;
    std::unique_ptr<AsyncEncoder> audio_encoder;
    if (encoding == "async") {
      video_encoder = std::make_unique<AsyncEncoder>(
          "video", 64, errors,
          [&](const ffmpeg::Frame& frame) { recording.Video(frame); });
      audio_encoder = std::make_unique<AsyncEncoder>(
          "audio", 64, errors,
          [&](const ffmpeg::Frame& frame) { recording.Audio(frame); });
    }
    std::int64_t video_callbacks = 0;
    std::int64_t callback_ns = 0;
    std::int64_t max_callback_ns = 0;
    std::int64_t late_callbacks = 0;
    std::atomic<bool> ready{false}, input_ended{false}, output_ended{false};
    mw::streamer::SchedulerConfig output_config;
    output_config.video_frame_rate = {fps, 1};
    mw::streamer::Processor processor;
    mw::streamer::Scheduler output(output_config);
    output.SetOnVideo([&](const ffmpeg::Frame& frame) noexcept {
      const auto started = Clock::now();
      errors.Capture([&] {
        auto processed = processor.ProcessVideo(frame);
        recording.Observe(processed);
        if (video_encoder)
          video_encoder->Submit(processed);
        else
          recording.Video(processed);
      });
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               Clock::now() - started)
                               .count();
      ++video_callbacks;
      callback_ns += elapsed;
      max_callback_ns = std::max(max_callback_ns, elapsed);
      if (elapsed > 1000000000 / fps) ++late_callbacks;
    });
    output.SetOnAudio([&](const ffmpeg::Frame& frame) noexcept {
      errors.Capture([&] {
        auto processed = processor.ProcessAudio(frame);
        recording.Observe(processed);
        if (audio_encoder)
          audio_encoder->Submit(processed);
        else
          recording.Audio(processed);
      });
    });
    // Processor callbacks stay unset: both tracks use default Frame::Ref().
    processor.SetOnEnded([&]() noexcept { output_ended.store(true); });
    output.SetOnEnded([&]() noexcept { processor.End(); });
    mw::streamer::FfmpegInputConfig input_config;
    input_config.auto_reconnect = false;
    mw::streamer::FfmpegInput input(device, input_config);
    input.SetOnReady([&](const auto& streams) noexcept {
      errors.Capture([&] {
        recording.Open(streams);
        ready.store(processor.Start(streams, device) && output.Start(streams));
        if (!ready.load())
          throw std::runtime_error("Processor or Scheduler rejected Ready");
      });
    });
    input.SetOnFrame([&](int, const ffmpeg::Frame& frame) noexcept {
      errors.Capture([&] {
        const bool accepted = frame->width > 0 ? output.SubmitVideo(frame)
                                               : output.SubmitAudio(frame);
        if (!accepted)
          throw std::runtime_error("Scheduler rejected input frame");
      });
    });
    input.SetOnStateChanged([&](mw::streamer::InputState state, int error,
                                std::string_view message) noexcept {
      errors.Capture([&] {
        if (state == mw::streamer::InputState::kFailed)
          throw ffmpeg::FfmpegException(error, message);
        if (state == mw::streamer::InputState::kEnded) input_ended.store(true);
      });
    });
    input.Start(argv[1]);
    while (!errors.failed.load() && !input_ended.load()) {
      const auto started = recording.StartedAt();
      if (started && Clock::now() - *started >= std::chrono::seconds(seconds))
        break;
      std::this_thread::sleep_for(10ms);
    }
    input.Stop();
    if (ready.load()) output.Drain();
    const auto drain_deadline = Clock::now() + 15s;
    while (ready.load() && !output_ended.load() &&
           Clock::now() < drain_deadline)
      std::this_thread::sleep_for(10ms);
    const bool drained = ready.load() && output_ended.load();
    output.Stop();
    processor.Stop();
    const auto encoding_drain_started = Clock::now();
    if (video_encoder) video_encoder->Close();
    if (audio_encoder) audio_encoder->Close();
    recording.Finish();
    fmt::print(
        "OUTPUT VIDEO: callbacks={}, average_callback_ms={:.3f}, "
        "max_callback_ms={:.3f}, over_frame_period={}, encoding_drain_ms={}\n",
        video_callbacks,
        video_callbacks ? callback_ns / 1000000.0 / video_callbacks : 0.0,
        max_callback_ns / 1000000.0, late_callbacks,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - encoding_drain_started)
            .count());
    if (video_encoder) video_encoder->Report();
    if (audio_encoder) audio_encoder->Report();
    errors.Rethrow();
    if (!drained)
      throw std::runtime_error("Scheduler Drain timed out or was not ready");
    fmt::print(
        "RECORDING COMPLETE: default Processor output, drained and "
        "encoder-flushed.\n");
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "Recording failed: {}\n", error.what());
    return 1;
  }
}
