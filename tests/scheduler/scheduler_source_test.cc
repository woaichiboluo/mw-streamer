#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::DecodedInputSource;
using mw::streamer::InputFrameSource;
using mw::streamer::MediaFrame;
using mw::streamer::Scheduler;
using mw::streamer::SchedulerConfig;
using namespace std::chrono_literals;

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct Queue {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Frame> frames;
  std::atomic<std::size_t> available{0};
  std::size_t cursor = 0;
  std::int64_t end = 0;
  bool closed = true;
  std::mutex mutex;

  InputFrameSource Source(const std::shared_ptr<Queue>& owner) {
    return {stream,
            [owner](ffmpeg::Frame& frame) {
              std::lock_guard<std::mutex> lock(owner->mutex);
              if (owner->cursor >= owner->available.load()) return false;
              frame = owner->frames[owner->cursor++].Ref();
              return true;
            },
            [owner] {
              std::lock_guard<std::mutex> lock(owner->mutex);
              return owner->closed && owner->cursor == owner->frames.size();
            },
            [owner] { return owner->end; }};
  }
};

std::shared_ptr<Queue> Video(int count, AVRational rate = {60, 1},
                             std::int64_t origin = 0, int index = 0) {
  auto queue = std::make_shared<Queue>();
  queue->stream.stream_index = index;
  queue->stream.time_base = {1, 90000};
  auto* parameters = queue->stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->width = parameters->height = 16;
  parameters->framerate = rate;
  for (int n = 0; n < count; ++n) {
    ffmpeg::Frame frame;
    frame->width = frame->height = 16;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->time_base = queue->stream.time_base;
    frame->pts = origin + av_rescale_q(n, av_inv_q(rate), frame->time_base);
    frame->best_effort_timestamp = frame->pts;
    frame->duration = av_rescale_q(1, av_inv_q(rate), frame->time_base);
    frame->sample_aspect_ratio = {1, 1};
    ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                          "分配测试视频帧");
    for (int plane = 0; plane < 3; ++plane) {
      const int height = plane ? 8 : 16;
      for (int row = 0; row < height; ++row)
        std::memset(frame->data[plane] + row * frame->linesize[plane],
                    plane ? 128 : 16 + n,
                    static_cast<std::size_t>(plane ? 8 : 16));
    }
    queue->frames.push_back(std::move(frame));
  }
  queue->end =
      origin + av_rescale_q(count, av_inv_q(rate), queue->stream.time_base);
  queue->available = queue->frames.size();
  return queue;
}

std::shared_ptr<Queue> Audio(int samples, int rate = 16000,
                             std::int64_t offset = 0) {
  auto queue = std::make_shared<Queue>();
  queue->stream.stream_index = 1;
  queue->stream.time_base = {1, rate};
  auto* parameters = queue->stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
  parameters->format = AV_SAMPLE_FMT_S16;
  parameters->sample_rate = rate;
  av_channel_layout_default(&parameters->ch_layout, 1);
  for (int n = 0; n < samples; n += 320) {
    ffmpeg::Frame frame;
    frame->format = AV_SAMPLE_FMT_S16;
    frame->sample_rate = rate;
    frame->nb_samples = std::min(320, samples - n);
    frame->pts = offset + n;
    frame->best_effort_timestamp = frame->pts;
    frame->duration = frame->nb_samples;
    frame->time_base = queue->stream.time_base;
    av_channel_layout_default(&frame->ch_layout, 1);
    ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                          "分配测试音频帧");
    auto* data = reinterpret_cast<std::int16_t*>(frame->data[0]);
    for (int i = 0; i < frame->nb_samples; ++i) data[i] = 1000;
    queue->frames.push_back(std::move(frame));
  }
  queue->end = offset + samples;
  queue->available = queue->frames.size();
  return queue;
}

struct Results {
  std::mutex mutex;
  std::vector<ffmpeg::Frame> video;
  std::vector<ffmpeg::Frame> audio;
  std::vector<ffmpeg::StreamInfo> streams;
  std::atomic<int> ended{0};
  std::atomic<int> errors{0};
  std::atomic<int> channel_frames{0};
  std::atomic<int> paired_frames{0};
  std::atomic<bool> frame_delivery_valid{true};
  std::thread::id callback_thread;
  bool same_thread = true;

  void Bind(Scheduler& scheduler) {
    scheduler.SetOnReady([this](const auto& output) { streams = output; });
    scheduler.SetOnFrame([this](const MediaFrame& frame) {
      const int tick = channel_frames++;
      if (frame.video && (*frame.video)->pts != tick)
        frame_delivery_valid = false;
      if (frame.video && frame.audio) ++paired_frames;
      if (frame.video) Collect(*frame.video, true);
      if (frame.audio) Collect(*frame.audio, false);
    });
    scheduler.SetOnEnded([this] { ++ended; });
    scheduler.SetOnError([this](int, std::string_view) { ++errors; });
  }
  void Collect(const ffmpeg::Frame& frame, bool v) {
    std::lock_guard<std::mutex> lock(mutex);
    if (callback_thread == std::thread::id{})
      callback_thread = std::this_thread::get_id();
    same_thread = same_thread && callback_thread == std::this_thread::get_id();
    (v ? video : audio).push_back(frame.Ref());
  }

  // The first output tick may preview the initial picture if fewer than four
  // frames are cached. Only that single initial still may precede the media
  // run. Check the buffer identity, silence and continuous PTS before removing
  // it; later repeats or missing native frames still fail the original
  // assertions.
  int StripInitialStill(std::size_t media_frames) {
    const auto count = std::max(video.size(), audio.size());
    if (count == media_frames) return 0;
    REQUIRE(count == media_frames + 1);
    if (!video.empty()) {
      REQUIRE(video.size() == count);
      REQUIRE(video[0]->buf[0]->buffer == video[1]->buf[0]->buffer);
      for (std::size_t n = 0; n < video.size(); ++n)
        REQUIRE(video[n]->pts == static_cast<std::int64_t>(n));
      video.erase(video.begin());
      for (auto& frame : video) --frame->pts;
    }
    if (!audio.empty()) {
      REQUIRE(audio.size() == count);
      const auto& first = audio.front();
      const auto* samples =
          reinterpret_cast<const std::int32_t*>(first->data[0]);
      REQUIRE(std::all_of(
          samples, samples + first->nb_samples * first->ch_layout.nb_channels,
          [](auto value) { return value == 0; }));
      std::int64_t pts = 0;
      for (const auto& frame : audio) {
        REQUIRE(frame->pts == pts);
        pts += frame->nb_samples;
      }
      const int shift = first->nb_samples;
      audio.erase(audio.begin());
      for (auto& frame : audio) frame->pts -= shift;
    }
    if (channel_frames) --channel_frames;
    if (paired_frames) --paired_frames;
    return 1;
  }
};

struct BufferReleases {
  std::mutex mutex;
  std::thread::id producer;
  std::array<std::thread::id, 16> threads{};
  std::size_t count = 0;
};

struct ObservedBuffer {
  AVBufferRef* reference;
  std::shared_ptr<BufferReleases> releases;
};

void ReleaseObservedBuffer(void* opaque, std::uint8_t*) {
  std::unique_ptr<ObservedBuffer> buffer(static_cast<ObservedBuffer*>(opaque));
  {
    std::lock_guard<std::mutex> lock(buffer->releases->mutex);
    auto& releases = *buffer->releases;
    const auto thread = std::this_thread::get_id();
    if (std::find(releases.threads.begin(),
                  releases.threads.begin() + releases.count,
                  thread) == releases.threads.begin() + releases.count &&
        releases.count < releases.threads.size())
      releases.threads[releases.count++] = thread;
  }
  av_buffer_unref(&buffer->reference);
}

InputFrameSource ObserveNativeReleases(
    const std::shared_ptr<Queue>& owner,
    const std::shared_ptr<BufferReleases>& releases) {
  for (auto& frame : owner->frames) {
    auto* reference = frame->buf[0];
    auto buffer =
        std::make_unique<ObservedBuffer>(ObservedBuffer{reference, releases});
    frame->buf[0] = av_buffer_create(reference->data, reference->size,
                                     ReleaseObservedBuffer, buffer.get(), 0);
    if (!frame->buf[0]) {
      frame->buf[0] = reference;
      throw std::bad_alloc();
    }
    buffer.release();
  }
  auto source = owner->Source(owner);
  source.try_receive = [owner, releases](ffmpeg::Frame& frame) {
    std::lock_guard<std::mutex> lock(owner->mutex);
    {
      std::lock_guard<std::mutex> release_lock(releases->mutex);
      releases->producer = std::this_thread::get_id();
    }
    if (owner->cursor >= owner->available.load()) return false;
    // Move the sole source reference: actual native fps drops / resampling
    // now release these buffers on the graph's execution thread.
    frame = std::move(owner->frames[owner->cursor++]);
    return true;
  };
  return source;
}

TEST_CASE("原始 MOV PTS 微抖动由 native fps 转换处理且不丢重复",
          "[scheduler][source][video]") {
  auto video = Video(60);
  video->frames[10]->pts -= 181;
  video->frames[10]->best_effort_timestamp = video->frames[10]->pts;
  video->frames[20]->pts += 1;
  video->frames[20]->best_effort_timestamp = video->frames[20]->pts;
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(output.ended == 1);
  output.StripInitialStill(60);
  REQUIRE(output.video.size() == 60);
  REQUIRE(output.channel_frames == 60);
  REQUIRE(output.paired_frames == 0);
  REQUIRE(output.frame_delivery_valid);
  for (std::size_t n = 0; n < output.video.size(); ++n) {
    REQUIRE(output.video[n]->pts == static_cast<std::int64_t>(n));
    REQUIRE(output.video[n]->time_base.num == 1);
    REQUIRE(output.video[n]->time_base.den == 60);
    REQUIRE(output.video[n]->data[0][0] == 16 + n);
  }
  REQUIRE(video->frames[10]->pts == 14819);
  REQUIRE(video->frames[10]->time_base.den == 90000);
}

TEST_CASE("native fps 支持降帧和升帧并排空源 EOF 时长",
          "[scheduler][source][conversion]") {
  const bool upsample = GENERATE(false, true);
  auto video = Video(upsample ? 12 : 24,
                     upsample ? AVRational{30, 1} : AVRational{60, 1});
  SchedulerConfig config;
  config.video_frame_rate = upsample ? AVRational{60, 1} : AVRational{30, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(upsample ? 24 : 12);
  REQUIRE(output.video.size() == (upsample ? 24 : 12));
  for (std::size_t n = 1; n < output.video.size(); ++n) {
    REQUIRE(output.video[n]->pts == static_cast<std::int64_t>(n));
    if (upsample && n % 2 == 1)
      REQUIRE(output.video[n]->data[0][0] == output.video[n - 1]->data[0][0]);
    if (!upsample)
      REQUIRE(output.video[n]->data[0][0] - output.video[n - 1]->data[0][0] ==
              2);
  }
}

TEST_CASE("音视频共同源起点保留十八毫秒偏移并输出 S32 cadence",
          "[scheduler][source][audio]") {
  auto video = Video(30, {60, 1}, 90000);
  auto audio = Audio(4000 - 144, 8000, 8000 + 144);
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start(
      {{video->Source(video), audio->Source(audio)}, AV_TIME_BASE}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(output.same_thread);
  output.StripInitialStill(30);
  REQUIRE(output.video.size() == 30);
  REQUIRE(output.audio.size() == 30);
  REQUIRE(output.channel_frames == 30);
  REQUIRE(output.paired_frames == 30);
  REQUIRE(output.frame_delivery_valid);
  REQUIRE(output.streams.size() == 2);
  REQUIRE(output.streams[1].codec_parameters.get()->format ==
          AV_SAMPLE_FMT_S32);
  std::vector<std::int32_t> pcm;
  for (std::size_t n = 0; n < output.audio.size(); ++n) {
    const auto& frame = output.audio[n];
    REQUIRE(frame->format == AV_SAMPLE_FMT_S32);
    REQUIRE(frame->nb_samples == 800);
    REQUIRE(frame->sample_rate == 48000);
    REQUIRE(frame->pts == static_cast<std::int64_t>(n * 800));
    REQUIRE(frame->time_base.den == 48000);
    const auto* data = reinterpret_cast<const std::int32_t*>(frame->data[0]);
    pcm.insert(pcm.end(), data, data + frame->nb_samples);
  }
  REQUIRE(pcm.size() == 24000);
  // FIR pre-ringing is expected around the resampled step. Check the centre.
  for (int n = 0; n < 750; ++n) REQUIRE(pcm[static_cast<std::size_t>(n)] == 0);
  const auto centre = std::find_if(
      pcm.begin(), pcm.end(), [](auto value) { return value >= 32768000; });
  REQUIRE(std::distance(pcm.begin(), centre) >= 858);
  REQUIRE(std::distance(pcm.begin(), centre) <= 870);
  REQUIRE(pcm[950] > 1000000);
}

TEST_CASE("非整数帧率音频样本按周期分配且无累计漂移",
          "[scheduler][source][cadence]") {
  auto video = Video(15, {60000, 1001});
  auto audio = Audio(12012, 48000);
  SchedulerConfig config;
  config.video_frame_rate = {60000, 1001};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  const int initial_still = output.StripInitialStill(15);
  REQUIRE(output.audio.size() == 15);
  const int cycle[]{800, 801, 801, 801, 801};
  std::int64_t samples = 0;
  for (std::size_t n = 0; n < output.audio.size(); ++n) {
    REQUIRE(output.audio[n]->nb_samples ==
            cycle[(n + static_cast<std::size_t>(initial_still)) % 5]);
    REQUIRE(output.audio[n]->pts == samples);
    samples += output.audio[n]->nb_samples;
  }
  REQUIRE(samples == 12012);
}

TEST_CASE("非整数 cadence 单样本起始静音和跨块余量不截断原 PCM 顺序",
          "[scheduler][source][sample_carry]") {
  auto video = Video(15, {60000, 1001});
  auto audio = Audio(12012, 48000);
  int value = 1;
  for (auto& frame : audio->frames) {
    auto* data = reinterpret_cast<std::int16_t*>(frame->data[0]);
    for (int n = 0; n < frame->nb_samples; ++n)
      data[n] = static_cast<std::int16_t>(value++);
  }
  SchedulerConfig config;
  config.video_frame_rate = {60000, 1001};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(15);
  REQUIRE(output.audio.size() == 15);
  int expected = 0;
  for (const auto& frame : output.audio) {
    const auto* data = reinterpret_cast<const std::int32_t*>(frame->data[0]);
    for (int n = 0; n < frame->nb_samples; ++n)
      REQUIRE(data[n] == expected++ * 65536);
  }
  REQUIRE(expected == 12012);
}

TEST_CASE("29.97 帧率按输出相位分配音频样本且跨块余量保留 PCM 顺序",
          "[scheduler][source][cadence][sample_carry]") {
  auto video = Video(10, {30000, 1001});
  auto audio = Audio(16016, 48000);
  int value = 1;
  for (auto& frame : audio->frames) {
    auto* data = reinterpret_cast<std::int16_t*>(frame->data[0]);
    for (int n = 0; n < frame->nb_samples; ++n)
      data[n] = static_cast<std::int16_t>(value++);
  }
  SchedulerConfig config;
  config.video_frame_rate = {30000, 1001};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  const int initial_still = output.StripInitialStill(10);
  REQUIRE(output.audio.size() == 10);
  const int channel_cycle[]{1601, 1602, 1601, 1602, 1602};
  int expected = 0;
  for (std::size_t tick = 0; tick < output.audio.size(); ++tick) {
    const auto& frame = output.audio[tick];
    REQUIRE(
        frame->nb_samples ==
        channel_cycle[(tick + static_cast<std::size_t>(initial_still)) % 5]);
    REQUIRE(frame->pts == expected);
    const auto* data = reinterpret_cast<const std::int32_t*>(frame->data[0]);
    for (int n = 0; n < frame->nb_samples; ++n)
      REQUIRE(data[n] == expected++ * 65536);
  }
  REQUIRE(expected == 16016);
}

TEST_CASE("44100Hz 源保留原始样本属性且 channel 固定输出 48000Hz",
          "[scheduler][source][resampling]") {
  auto video = Video(6);
  auto audio = Audio(4410, 44100);
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(6);
  REQUIRE(output.audio.size() == 6);
  REQUIRE(output.streams.back().codec_parameters.get()->sample_rate == 48000);
  int samples = 0;
  for (const auto& frame : output.audio) {
    REQUIRE(frame->sample_rate == 48000);
    REQUIRE(frame->nb_samples == 800);
    samples += frame->nb_samples;
  }
  REQUIRE(samples == 4800);
  REQUIRE(audio->frames.front()->sample_rate == 44100);
  REQUIRE(audio->frames.front()->time_base.den == 44100);
  REQUIRE(audio->frames.front()->format == AV_SAMPLE_FMT_S16);
}

TEST_CASE("不支持的分数音频节拍在读取输入源之前拒绝启动",
          "[scheduler][source][configuration]") {
  auto video = Video(6);
  SchedulerConfig config;
  config.video_frame_rate = {31, 2};
  Scheduler scheduler(config);
  REQUIRE_FALSE(scheduler.Start({{video->Source(video)}, 0}));
  REQUIRE(video->cursor == 0);
}

TEST_CASE("不同源视频帧率转换后音频与完整 bundle 时长保持一致",
          "[scheduler][source][av_conversion]") {
  const int conversion = GENERATE(0, 1);
  auto video = Video(conversion ? 20 : 10,
                     conversion ? AVRational{50, 1} : AVRational{25, 1});
  auto audio = Audio(6400);
  SchedulerConfig config;
  config.video_frame_rate = conversion ? AVRational{60, 1} : AVRational{30, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(conversion ? 24 : 12);
  REQUIRE(output.video.size() == (conversion ? 24 : 12));
  REQUIRE(output.audio.size() == output.video.size());
  std::int64_t samples = 0;
  for (const auto& frame : output.audio) samples += frame->nb_samples;
  REQUIRE(samples == 19200);
  const auto video_end =
      av_rescale_q(static_cast<std::int64_t>(output.video.size()),
                   av_inv_q(config.video_frame_rate), {1, 48000});
  REQUIRE(video_end == samples);
}

TEST_CASE("native bwdif 将交错帧拆成渐进场帧且源 flags 不被改写",
          "[scheduler][source][interlaced]") {
  auto video = Video(6, {30, 1});
  for (auto& frame : video->frames)
    frame->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(12);
  REQUIRE(output.video.size() == 12);
  REQUIRE(output.streams.front().codec_parameters.get()->field_order ==
          AV_FIELD_PROGRESSIVE);
  for (std::size_t n = 0; n < output.video.size(); ++n) {
    REQUIRE_FALSE(output.video[n]->flags & AV_FRAME_FLAG_INTERLACED);
    REQUIRE(output.video[n]->pts == static_cast<std::int64_t>(n));
  }
  REQUIRE(video->frames.front()->flags & AV_FRAME_FLAG_INTERLACED);
}

TEST_CASE("新的源段原始 PTS 回开头而输出 tick 连续且仅 Drain 结束",
          "[scheduler][source][loop]") {
  auto first = Video(8);
  auto second = Video(8);
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{first->Source(first)}, 0}));
  REQUIRE(scheduler.QueueSource({{second->Source(second)}, 0}));
  scheduler.Drain();
  REQUIRE_FALSE(scheduler.QueueSource({{second->Source(second)}, 0}));
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(output.ended == 1);
  output.StripInitialStill(16);
  REQUIRE(output.video.size() == 16);
  for (std::size_t n = 0; n < output.video.size(); ++n) {
    REQUIRE(output.video[n]->pts == static_cast<std::int64_t>(n));
    REQUIRE(output.video[n]->data[0][0] == 16 + n % 8);
  }
}

TEST_CASE("短于 preroll 的源和空源正常 EOF 不会等待四帧",
          "[scheduler][source][eof]") {
  const int frames = GENERATE(0, 1);
  auto video = Video(frames);
  Scheduler scheduler;
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(output.ended == 1);
  output.StripInitialStill(static_cast<std::size_t>(frames));
  REQUIRE(output.video.size() == static_cast<std::size_t>(frames));
}

TEST_CASE("停止取消等待中的 pull 并释放所有源引用且回调不再运行",
          "[scheduler][source][stop]") {
  auto video = Video(20);
  video->available = 0;
  std::weak_ptr<Queue> lifetime = video;
  Scheduler scheduler;
  Results output;
  output.Bind(scheduler);
  DecodedInputSource source{{video->Source(video)}, 0};
  REQUIRE(scheduler.Start(source));
  source.tracks.clear();
  video.reset();
  const auto started = std::chrono::steady_clock::now();
  scheduler.RequestStop();
  scheduler.Stop();
  REQUIRE(std::chrono::steady_clock::now() - started < 500ms);
  REQUIRE(lifetime.expired());
  REQUIRE(output.ended == 0);
  REQUIRE(output.errors == 0);
  REQUIRE(output.video.empty());
}

TEST_CASE("Scheduler 拒绝同类型多轨且不消费任何输入",
          "[scheduler][source][selection]") {
  const bool video = GENERATE(true, false);
  auto first = video ? Video(8) : Audio(1600);
  auto second = video ? Video(8, {60, 1}, 0, 2) : Audio(1600);
  second->stream.stream_index = 2;
  DecodedInputSource source{{first->Source(first), second->Source(second)}, 0};
  Scheduler scheduler;
  Results output;
  output.Bind(scheduler);
  REQUIRE_FALSE(scheduler.Start(source));
  scheduler.Stop();
  REQUIRE(first->cursor == 0);
  REQUIRE(second->cursor == 0);
  REQUIRE(output.video.empty());
  REQUIRE(output.audio.empty());
  REQUIRE(output.ended == 0);
  REQUIRE(output.errors == 0);
}

TEST_CASE("Scheduler 接受已选轨的非连续索引且与源顺序无关",
          "[scheduler][source][selection]") {
  auto video = Video(8, {60, 1}, 0, 7);
  auto audio = Audio(6400, 48000);
  audio->stream.stream_index = 3;
  const bool audio_first = GENERATE(true, false);
  DecodedInputSource source;
  if (audio_first)
    source.tracks = {audio->Source(audio), video->Source(video)};
  else
    source.tracks = {video->Source(video), audio->Source(audio)};
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start(source));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(video->cursor == video->frames.size());
  REQUIRE(audio->cursor == audio->frames.size());
  output.StripInitialStill(8);
  REQUIRE(output.video.size() == 8);
  REQUIRE(output.audio.size() == 8);
  REQUIRE(output.streams.size() == 2);
  REQUIRE(output.streams[0].stream_index == 7);
  REQUIRE(output.streams[1].stream_index == 3);
}

TEST_CASE("媒体回调异常触发一次 fatal 并不伪装正常 EOF",
          "[scheduler][source][error]") {
  auto video = Video(12);
  Scheduler scheduler;
  Results output;
  output.Bind(scheduler);
  scheduler.SetOnFrame(
      [](const auto&) { throw std::runtime_error("processor"); });
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.errors != 0; }));
  scheduler.Stop();
  REQUIRE(output.errors == 1);
  REQUIRE(output.ended == 0);
}

TEST_CASE("native fps 丢源帧与音频转换在不同持久滤镜线程运行",
          "[scheduler][source][parallel]") {
  auto video = Video(120, {60, 1});
  auto audio = Audio(96000, 48000);
  auto video_releases = std::make_shared<BufferReleases>();
  auto audio_releases = std::make_shared<BufferReleases>();
  auto video_source = ObserveNativeReleases(video, video_releases);
  auto audio_source = ObserveNativeReleases(audio, audio_releases);
  SchedulerConfig config;
  config.video_frame_rate = {30, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video_source, audio_source}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(60);
  REQUIRE(output.video.size() == 60);
  REQUIRE(output.audio.size() == 60);
  std::vector<std::thread::id> video_workers;
  std::vector<std::thread::id> audio_workers;
  for (const auto& item : {std::make_pair(video_releases, &video_workers),
                           std::make_pair(audio_releases, &audio_workers)}) {
    std::lock_guard<std::mutex> lock(item.first->mutex);
    for (std::size_t n = 0; n < item.first->count; ++n)
      if (item.first->threads[n] != item.first->producer)
        item.second->push_back(item.first->threads[n]);
  }
  REQUIRE(video_releases->producer == audio_releases->producer);
  // These are release callbacks of actual native filter work, rather than
  // source callbacks (which correctly remain on the coordinating producer).
  REQUIRE(video_workers.size() == 1);
  REQUIRE(audio_workers.size() == 1);
  REQUIRE(video_workers.front() != audio_workers.front());
}

TEST_CASE("双滤镜运行期间源异常只上报一次且 Stop 释放两个图的缓冲引用",
          "[scheduler][source][parallel][error][stop]") {
  auto video = Video(120);
  auto audio = Audio(96000, 48000);
  std::weak_ptr<Queue> video_lifetime = video;
  std::weak_ptr<Queue> audio_lifetime = audio;
  auto video_source = video->Source(video);
  auto receive = video_source.try_receive;
  video_source.try_receive = [receive, count = 0](auto& frame) mutable {
    if (++count == 10) throw std::runtime_error("native source failed");
    return receive(frame);
  };
  Scheduler scheduler;
  Results output;
  output.Bind(scheduler);
  DecodedInputSource source{{video_source, audio->Source(audio)}, 0};
  REQUIRE(scheduler.Start(source));
  source.tracks.clear();
  video_source = {};
  receive = {};
  video.reset();
  audio.reset();
  REQUIRE(WaitFor([&] { return output.errors != 0; }));
  const auto started = std::chrono::steady_clock::now();
  scheduler.Stop();
  REQUIRE(std::chrono::steady_clock::now() - started < 500ms);
  REQUIRE(video_lifetime.expired());
  REQUIRE(audio_lifetime.expired());
  REQUIRE(output.errors == 1);
  REQUIRE(output.ended == 0);
}

TEST_CASE("源临时 underflow 重复画面但同一 tick 音频为静音且恢复顺序不丢",
          "[scheduler][source][underflow]") {
  auto video = Video(36);
  auto audio = Audio(9600);
  video->available = 8;
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<int> callbacks{0};
  scheduler.SetOnFrame([&](const MediaFrame& frame) {
    if (frame.video) output.Collect(*frame.video, true);
    if (frame.audio) output.Collect(*frame.audio, false);
    if (++callbacks == 12) video->available = video->frames.size();
  });
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(output.video.size() > 36);
  REQUIRE(output.audio.size() == output.video.size());
  int silent = 0;
  std::vector<int> pictures;
  for (std::size_t n = 0; n < output.video.size(); ++n) {
    REQUIRE(output.video[n]->pts == static_cast<std::int64_t>(n));
    REQUIRE(output.audio[n]->pts == static_cast<std::int64_t>(n * 800));
    const int picture = output.video[n]->data[0][0];
    if (pictures.empty() || pictures.back() != picture)
      pictures.push_back(picture);
    const auto* data =
        reinterpret_cast<const std::int32_t*>(output.audio[n]->data[0]);
    if (std::all_of(data, data + 800, [](auto sample) { return sample == 0; }))
      ++silent;
  }
  REQUIRE(silent >= 1);
  REQUIRE(pictures.size() == 36);
  for (std::size_t n = 0; n < pictures.size(); ++n)
    REQUIRE(pictures[n] == 16 + static_cast<int>(n));
}

TEST_CASE("仅音频源按 cadence 排空真实尾样本并补足最后一个 channel 块",
          "[scheduler][source][audio_only]") {
  auto audio = Audio(960, 48000);
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(output.video.empty());
  output.StripInitialStill(2);
  REQUIRE(output.audio.size() == 2);
  REQUIRE(output.audio.back()->nb_samples == 800);
  const auto* tail =
      reinterpret_cast<const std::int32_t*>(output.audio.back()->data[0]);
  REQUIRE(
      std::all_of(tail, tail + 160, [](auto sample) { return sample > 0; }));
  REQUIRE(std::all_of(tail + 160, tail + 800,
                      [&](auto sample) { return sample == tail[159]; }));
}

TEST_CASE("音频先 EOF 后继续视频时复用最后音频块",
          "[scheduler][source][audio_eof]") {
  auto video = Video(8);
  auto audio = Audio(1920, 48000);
  int value = 1;
  for (auto& frame : audio->frames) {
    auto* data = reinterpret_cast<std::int16_t*>(frame->data[0]);
    for (int sample = 0; sample < frame->nb_samples; ++sample)
      data[sample] = static_cast<std::int16_t>(value++);
  }
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(8);
  REQUIRE(output.video.size() == 8);
  REQUIRE(output.audio.size() == 8);
  // After audio EOF, Scheduler retains the final audio block and repeats its
  // last sample to fill a short block. Later video frames reuse the entire
  // retained block.
  for (std::size_t tick = 0; tick < output.audio.size(); ++tick) {
    const auto& frame = output.audio[tick];
    REQUIRE(frame->pts == static_cast<std::int64_t>(tick * 800));
    const auto* data = reinterpret_cast<const std::int32_t*>(frame->data[0]);
    for (int sample = 0; sample < 800; ++sample) {
      const int expected = tick < 2 ? static_cast<int>(tick) * 800 + sample + 1
                                    : 1601 + std::min(sample, 319);
      REQUIRE(data[sample] == expected * 65536);
    }
  }
  REQUIRE(output.ended == 1);
}

TEST_CASE("初始低库存时预览首帧无需等待缓存凑齐四帧",
          "[scheduler][source][preroll]") {
  auto video = Video(20);
  // bwdif/fps needs lookahead. Four native inputs provide initial output but
  // cannot fill four completed output frames. Previewing the first picture
  // clears the initial buffer threshold without consuming that picture.
  video->available = 4;
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<bool> received{false};
  scheduler.SetOnFrame([&](const MediaFrame& frame) {
    if (frame.video) {
      output.Collect(*frame.video, true);
      if (!received.exchange(true)) video->available = video->frames.size();
    }
  });
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  const bool first_received =
      WaitFor([&] { return received || output.errors; });
  if (!first_received) video->available = video->frames.size();
  const bool ended = WaitFor([&] { return output.ended || output.errors; });
  scheduler.Stop();
  REQUIRE(first_received);
  REQUIRE(ended);
  REQUIRE(output.errors == 0);
  REQUIRE(output.video.size() == 21);
  REQUIRE(output.video[0]->buf[0]->buffer == output.video[1]->buf[0]->buffer);
  REQUIRE(output.video.front()->data[0][0] == 16);
  REQUIRE(output.StripInitialStill(20) == 1);
  for (std::size_t n = 0; n < output.video.size(); ++n)
    REQUIRE(output.video[n]->data[0][0] == 16 + n);
  REQUIRE(output.ended == 1);
}

TEST_CASE("首帧立即交付后按输出周期等待再交付后续帧",
          "[scheduler][source][clock]") {
  auto video = Video(4, {10, 1});
  SchedulerConfig config;
  config.video_frame_rate = {10, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::vector<std::chrono::steady_clock::time_point> deliveries;
  scheduler.SetOnFrame([&](const MediaFrame& frame) {
    deliveries.push_back(std::chrono::steady_clock::now());
    if (frame.video) output.Collect(*frame.video, true);
  });
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  const int initial_still = output.StripInitialStill(4);
  if (initial_still) deliveries.erase(deliveries.begin());
  REQUIRE(deliveries.size() == 4);
  // After the first delivery, consecutive media frames must remain separated
  // by the configured output period instead of being sent in an initial burst.
  REQUIRE(deliveries[1] - deliveries[0] >= 80ms);
  REQUIRE(deliveries[2] - deliveries[1] >= 80ms);
  REQUIRE(deliveries[3] - deliveries[2] >= 80ms);
  REQUIRE(output.ended == 1);
}

TEST_CASE("低库存首帧 peek 保留原图音频并只静音初始一拍",
          "[scheduler][source][preroll][still]") {
  auto video = Video(3, {10, 1});
  auto audio = Audio(14400, 48000);
  SchedulerConfig config;
  config.video_frame_rate = {10, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  REQUIRE(scheduler.Start({{video->Source(video), audio->Source(audio)}, 0}));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  // The producer queue holds two frames, so the first output previews the
  // picture without consuming it.
  REQUIRE(output.video.size() == 4);
  REQUIRE(output.audio.size() == 4);
  REQUIRE(output.video[0]->buf[0]->buffer == output.video[1]->buf[0]->buffer);
  for (std::size_t tick = 0; tick < 4; ++tick) {
    REQUIRE(output.video[tick]->pts == static_cast<std::int64_t>(tick));
    REQUIRE(output.video[tick]->data[0][0] ==
            16 + std::max(0, static_cast<int>(tick) - 1));
    REQUIRE(output.audio[tick]->pts == static_cast<std::int64_t>(tick * 4800));
    const auto* samples =
        reinterpret_cast<const std::int32_t*>(output.audio[tick]->data[0]);
    for (int sample = 0; sample < 4800; ++sample)
      REQUIRE(samples[sample] == (tick == 0 ? 0 : 1000 * 65536));
  }
  REQUIRE(output.ended == 1);
}

TEST_CASE("循环回开低库存时预览尚未消费的旧段且保留缓存",
          "[scheduler][source][loop][still]") {
  auto first = Video(4, {10, 1});
  auto second = Video(2, {10, 1});
  for (auto& frame : second->frames) frame->data[0][0] += 100;
  SchedulerConfig config;
  config.video_frame_rate = {10, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<int> completions{0};
  std::atomic<bool> queued{false};
  DecodedInputSource second_source{{second->Source(second)}, 0};
  DecodedInputSource first_source{{first->Source(first)}, 0};
  first_source.loop = true;
  first_source.complete = [&](auto count) {
    if (count == 4) ++completions;
    queued = scheduler.QueueSource(second_source);
    scheduler.Drain();
  };
  REQUIRE(scheduler.Start(first_source));
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(queued);
  REQUIRE(completions == 1);
  const int pictures[]{16, 16, 17, 18, 18, 19, 116, 117};
  REQUIRE(output.video.size() == 8);
  REQUIRE(output.video[3]->buf[0]->buffer == output.video[4]->buf[0]->buffer);
  for (std::size_t tick = 0; tick < 8; ++tick) {
    REQUIRE(output.video[tick]->pts == static_cast<std::int64_t>(tick));
    REQUIRE(output.video[tick]->data[0][0] == pictures[tick]);
  }
  REQUIRE(output.ended == 1);
}

TEST_CASE("显式 Seek flush 清除旧段待交付缓存且不完成被放弃的旧段",
          "[scheduler][source][seek][flush]") {
  auto first = Video(60, {10, 1});
  auto second = Video(4, {10, 1});
  for (auto& frame : second->frames) frame->data[0][0] += 100;
  SchedulerConfig config;
  config.video_frame_rate = {10, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<int> old_completions{0};
  std::atomic<int> new_completions{0};
  std::atomic<bool> queued{false};
  DecodedInputSource first_source{{first->Source(first)}, 0};
  first_source.complete = [&](auto) { ++old_completions; };
  DecodedInputSource second_source{{second->Source(second)}, 0};
  second_source.flush = true;
  second_source.complete = [&](auto count) {
    if (count == 4) ++new_completions;
  };
  scheduler.SetOnFrame([&](const MediaFrame& frame) {
    if (!frame.video) return;
    output.Collect(*frame.video, true);
    if (output.video.size() == 1) {
      queued = scheduler.QueueSource(second_source);
      scheduler.Drain();
    }
  });
  REQUIRE(scheduler.Start(first_source));
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(queued);
  REQUIRE(old_completions == 0);
  REQUIRE(new_completions == 1);
  const int pictures[]{16, 116, 116, 117, 118, 119};
  REQUIRE(output.video.size() == 6);
  for (std::size_t tick = 0; tick < 6; ++tick) {
    REQUIRE(output.video[tick]->pts == static_cast<std::int64_t>(tick));
    REQUIRE(output.video[tick]->data[0][0] == pictures[tick]);
  }
  REQUIRE(output.ended == 1);
}

TEST_CASE("源段按容器时长在滤镜 frame 边界完成且计数为实际频道帧",
          "[scheduler][source][duration][loop]") {
  auto first = Video(12, {30, 1});
  auto second = Video(12, {30, 1});
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<int> completions{0};
  std::int64_t counts[2]{-1, -1};
  DecodedInputSource first_source{{first->Source(first)}, 0};
  first_source.duration_us = 50000;
  first_source.complete = [&](auto frames) {
    counts[0] = frames;
    ++completions;
  };
  DecodedInputSource second_source{{second->Source(second)}, 0};
  second_source.duration_us = 33333;
  second_source.complete = [&](auto frames) {
    counts[1] = frames;
    ++completions;
  };
  REQUIRE(scheduler.Start(first_source));
  REQUIRE(scheduler.QueueSource(second_source));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(5);
  REQUIRE(output.video.size() == 5);
  REQUIRE(completions == 2);
  REQUIRE(counts[0] == 3);
  REQUIRE(counts[1] == 2);
  const int pictures[]{16, 16, 17, 16, 16};
  for (std::size_t tick = 0; tick < output.video.size(); ++tick) {
    REQUIRE(output.video[tick]->pts == static_cast<std::int64_t>(tick));
    REQUIRE(output.video[tick]->data[0][0] == pictures[tick]);
  }
  REQUIRE(first->cursor < first->frames.size());
  REQUIRE(second->cursor < second->frames.size());
}

TEST_CASE("音频先 EOF 时容器边界根据后续新视频更新且仅完成一次",
          "[scheduler][source][duration][audio_eof]") {
  auto video = Video(12);
  auto audio = Audio(1920, 48000);
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<int> completions{0};
  std::int64_t count = -1;
  DecodedInputSource source{{video->Source(video), audio->Source(audio)}, 0};
  source.duration_us = 66667;
  source.complete = [&](auto frames) {
    count = frames;
    ++completions;
  };
  REQUIRE(scheduler.Start(source));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  output.StripInitialStill(4);
  REQUIRE(output.video.size() == 4);
  REQUIRE(output.audio.size() == 4);
  REQUIRE(completions == 1);
  REQUIRE(count == 4);
}

TEST_CASE("非频道整帧 Seek 以格式原点的绝对端点舍入决定时长边界",
          "[scheduler][source][duration][seek]") {
  auto video = Video(70, {30, 1}, 36450);   // 405 ms in 1/90000.
  auto audio = Audio(75936, 48000, 19440);  // Valid tail ends at 1.987 s.
  SchedulerConfig config;
  config.video_frame_rate = {30, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::atomic<int> completions{0};
  std::int64_t count = -1;
  DecodedInputSource source{{video->Source(video), audio->Source(audio)},
                            405000};
  source.media_origin_us = 0;
  source.duration_us = 1595000;  // Original container endpoint is 2 s.
  source.complete = [&](auto frames) {
    count = frames;
    ++completions;
  };
  REQUIRE(scheduler.Start(source));
  scheduler.Drain();
  REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(completions == 1);
  // Scheduler rounds time=1.987 s and end=2 s to the same output tick.
  // Subtracting the arbitrary Seek before rounding would require another
  // picture because round(1.582*30)=47 but round(1.595*30)=48.
  REQUIRE(count == 48);
  output.StripInitialStill(48);
  REQUIRE(output.video.size() == 48);
  REQUIRE(output.audio.size() == 48);
  REQUIRE(video->cursor < video->frames.size());
}

TEST_CASE("空滤镜 EOF 报告零帧而主动停止不报告源段完成",
          "[scheduler][source][complete]") {
  const bool stopped = GENERATE(false, true);
  auto video = Video(stopped ? 12 : 0);
  if (stopped) video->available = 0;
  Scheduler scheduler;
  Results output;
  output.Bind(scheduler);
  std::atomic<int> completions{0};
  std::int64_t count = -1;
  DecodedInputSource source{{video->Source(video)}, 0};
  source.complete = [&](auto frames) {
    count = frames;
    ++completions;
  };
  REQUIRE(scheduler.Start(source));
  if (stopped)
    scheduler.RequestStop();
  else {
    scheduler.Drain();
    REQUIRE(WaitFor([&] { return output.ended || output.errors; }));
  }
  scheduler.Stop();
  REQUIRE(output.errors == 0);
  REQUIRE(completions == (stopped ? 0 : 1));
  REQUIRE(count == (stopped ? -1 : 0));
  REQUIRE(output.ended == (stopped ? 0 : 1));
}

TEST_CASE("完整 bundle 背压限制 source pull 预读且 RequestStop 唤醒生产等待",
          "[scheduler][source][backpressure]") {
  auto video = Video(600);
  SchedulerConfig config;
  config.video_frame_rate = {60, 1};
  Scheduler scheduler(config);
  Results output;
  output.Bind(scheduler);
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> entered{false};
  bool resume = false;
  scheduler.SetOnFrame([&](const MediaFrame& frame) {
    if (frame.video) output.Collect(*frame.video, true);
    if (frame.audio) output.Collect(*frame.audio, false);
    if (!entered.exchange(true)) {
      std::unique_lock<std::mutex> lock(mutex);
      changed.wait_for(lock, 3s, [&] { return resume; });
    }
  });
  REQUIRE(scheduler.Start({{video->Source(video)}, 0}));
  REQUIRE(WaitFor([&] { return entered.load(); }));
  std::this_thread::sleep_for(50ms);
  std::size_t pulled;
  {
    std::lock_guard<std::mutex> lock(video->mutex);
    pulled = video->cursor;
  }
  REQUIRE(pulled >= 4);
  REQUIRE(pulled <= 32);
  scheduler.RequestStop();
  {
    std::lock_guard<std::mutex> lock(mutex);
    resume = true;
  }
  changed.notify_all();
  const auto started = std::chrono::steady_clock::now();
  scheduler.Stop();
  REQUIRE(std::chrono::steady_clock::now() - started < 500ms);
  REQUIRE(output.ended == 0);
  REQUIRE(output.errors == 0);
}

}  // namespace
