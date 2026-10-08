#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "scheduler_test_support.h"

namespace {
using namespace mw::streamer::testing;

TEST_CASE("Scheduler音频输出线程切块后调用Processor并保留EOS尾样本",
          "[scheduler][audio]") {
  int calls = 0;
  bool normalized = true;
  const auto caller = std::this_thread::get_id();
  Rig rig;
  rig.processor.SetOnAudio([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      ++calls;
      normalized &= std::this_thread::get_id() != caller &&
                    frame->sample_rate == 48000 &&
                    frame->format == AV_SAMPLE_FMT_FLTP &&
                    frame->ch_layout.nb_channels == 2;
      return frame.Ref();
    });
  });
  REQUIRE(rig.Initialize(false, true));
  auto audio = Audio(2050, 9'000'000'000);
  REQUIRE(rig.scheduler.SubmitAudio(audio));
  audio.Unref();
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(ended);
  CHECK(calls == static_cast<int>(rig.audios.size()));
  CHECK(normalized);
  REQUIRE(rig.audios.size() == 3);
  CHECK(rig.audios[0].samples == 1024);
  CHECK(rig.audios[1].samples == 1024);
  CHECK(rig.audios[2].samples < 1024);
  int index = 1;
  bool content_started = false;
  for (const auto& output : rig.audios) {
    CHECK(output.sample_rate == 48000);
    CHECK(output.format == AV_SAMPLE_FMT_FLTP);
    REQUIRE(output.left.size() == static_cast<size_t>(output.samples));
    for (size_t i = 0; i < output.left.size(); ++i) {
      if (!content_started && output.left[i] == 0.0f) continue;
      content_started = true;
      CHECK(output.left[i] == static_cast<float>(index) / 4096.0f);
      CHECK(output.right[i] == -static_cast<float>(index) / 4096.0f);
      ++index;
    }
  }
  CHECK(index == 2050);
  CHECK(rig.audios[1].pts > rig.audios[0].pts);
  CHECK(rig.audios[2].pts > rig.audios[1].pts);
}

TEST_CASE("Scheduler音频回退后恢复且输出PTS继续按样本时钟推进",
          "[scheduler][audio][timing]") {
  Rig rig;
  REQUIRE(rig.Initialize(false, true));
  REQUIRE(rig.scheduler.SubmitAudio(Audio(2048, 9'000'000'000)));
  const bool started = rig.Wait([&] { return rig.audios.size() >= 2; });
  auto backward = Audio(1024, 6'000'000'000);
  for (int channel = 0; channel < 2; ++channel) {
    auto* samples = reinterpret_cast<float*>(backward->extended_data[channel]);
    std::fill(samples, samples + backward->nb_samples, 0.75f);
  }
  const bool accepted = rig.scheduler.SubmitAudio(backward);
  const bool resumed = rig.Wait(
      [&] {
        return std::any_of(
            rig.audios.begin(), rig.audios.end(), [](const auto& block) {
              return std::find(block.left.begin(), block.left.end(), 0.75f) !=
                     block.left.end();
            });
      },
      200ms);
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(started);
  CHECK(accepted);
  CHECK(resumed);
  CHECK(ended);
  for (size_t index = 1; index < rig.audios.size(); ++index) {
    const auto delta = rig.audios[index].pts - rig.audios[index - 1].pts;
    CHECK(delta >= 21'333'333);
    CHECK(delta <= 21'333'334);
  }
}

TEST_CASE("Scheduler重采样后调用音频滤镜并排空SWR延迟", "[scheduler][audio]") {
  bool normalized = true;
  Rig rig;
  rig.processor.SetOnAudio([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      normalized &= frame->format == AV_SAMPLE_FMT_FLTP &&
                    frame->sample_rate == 48000 &&
                    frame->ch_layout.nb_channels == 2;
      return frame.Ref();
    });
  });
  REQUIRE(rig.Initialize(false, true));
  REQUIRE(rig.scheduler.SubmitAudio(Audio(4410, 0, 44100, AV_SAMPLE_FMT_S16)));
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(ended);
  CHECK(normalized);
  int samples = 0;
  int leading_silence = 0;
  bool started = false;
  for (const auto& output : rig.audios) {
    samples += output.samples;
    for (float sample : output.left) {
      if (sample != 0.0f) started = true;
      if (!started) ++leading_silence;
    }
    REQUIRE_FALSE(output.left.empty());
    for (size_t i = 64; i + 64 < output.left.size(); ++i) {
      if (output.left[i] == 0.0f) continue;
      CHECK(output.left[i] > 0.249f);
      CHECK(output.left[i] < 0.251f);
      CHECK(output.right[i] < -0.249f);
      CHECK(output.right[i] > -0.251f);
    }
  }
  CHECK(samples - leading_silence == 4800);
}

TEST_CASE("Scheduler切块后的Processor输出不再改格式或样本数",
          "[scheduler][audio]") {
  constexpr int64_t kReturnedPts = -123'456;
  bool correct_data = true;
  bool content_started = false;
  int content_samples = 0;
  Rig rig;
  rig.processor.SetOnAudio([&](const ffmpeg::Frame&) {
    return rig.errors.Filter(
        [&] { return Audio(1500, kReturnedPts, 48000, AV_SAMPLE_FMT_S16); });
  });
  rig.SetAudioSink([&](const ffmpeg::Frame& frame) {
    rig.errors.Run([&] {
      correct_data &= frame->format == AV_SAMPLE_FMT_S16 &&
                      frame->ch_layout.nb_channels == 2;
      if (frame->format == AV_SAMPLE_FMT_S16) {
        const auto* data = reinterpret_cast<const int16_t*>(frame->data[0]);
        for (int i = 0; i < frame->nb_samples; ++i) {
          if (!content_started && !data[2 * i] && !data[2 * i + 1]) continue;
          content_started = true;
          ++content_samples;
          correct_data &= data[2 * i] == 8192 && data[2 * i + 1] == -8192;
        }
      }
      std::lock_guard<std::mutex> lock(rig.mutex);
      rig.audios.push_back({frame->nb_samples,
                            frame->sample_rate,
                            frame->format,
                            frame->ch_layout.nb_channels,
                            frame->pts,
                            {},
                            {}});
    });
  });
  REQUIRE(rig.Initialize(false, true));
  REQUIRE(rig.scheduler.SubmitAudio(Audio(64)));
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(ended);
  CHECK(correct_data);
  REQUIRE(rig.audios.size() == 1);
  CHECK(rig.audios[0].samples == 1500);
  CHECK(rig.audios[0].pts == kReturnedPts);
  CHECK(content_samples == 1500);
}

}  // namespace
