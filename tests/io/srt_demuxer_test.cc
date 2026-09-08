#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "Http/HlsPlayer.h"
#include "Poller/EventPoller.h"
#include "ext-codec/AAC.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

struct AudioListener final : public mediakit::TrackListener {
  bool addTrack(const mediakit::Track::Ptr& track) override {
    track->addDelegate([this](const mediakit::Frame::Ptr& frame) {
      timestamps.push_back(frame->dts());
      return true;
    });
    return true;
  }

  void addTrackCompleted() override { ready = true; }

  bool ready = false;
  std::vector<std::int64_t> timestamps;
};

mediakit::Frame::Ptr AudioFrame(std::uint64_t timestamp) {
  auto frame = mediakit::FrameImp::create();
  frame->_codec_id = mediakit::CodecAAC;
  frame->_dts = timestamp;
  frame->_buffer.assign("\0", 1);
  return frame;
}

TEST_CASE("SRT demuxing delivers live frames without HLS playback buffering") {
  bool buffered = false;
  SECTION("live delivery") {}
  SECTION("default HLS buffering") { buffered = true; }

  auto poller = toolkit::EventPollerPool::Instance().extractPoller();
  AudioListener listener;
  auto demuxer = std::make_shared<mediakit::HlsDemuxer>();
  std::promise<void> completed;
  auto completion = completed.get_future();
  bool accepted = false;
  std::vector<std::int64_t> immediately_delivered;
  poller->sync([&] {
    if (buffered) {
      demuxer->start(poller, &listener);
    } else {
      demuxer->start(poller, &listener, false);
    }
    demuxer->enableMuteAudio(false);
    accepted = demuxer->addTrack(
        std::make_shared<mediakit::AACTrack>(std::string("\x11\x90", 2)));
    demuxer->addTrackCompleted();
    demuxer->inputFrame(AudioFrame(0));
    listener.timestamps.clear();

    // Run both submissions in one poller task so no timer can intervene.
    demuxer->inputFrame(AudioFrame(1000));
    demuxer->inputFrame(AudioFrame(1021));
    demuxer->pushTask([&] {
      listener.timestamps.push_back(-1);
      completed.set_value();
    });
    immediately_delivered = listener.timestamps;
  });
  const auto status = completion.wait_for(std::chrono::seconds(2));
  poller->sync([&] { demuxer.reset(); });

  CHECK(accepted);
  CHECK(listener.ready);
  CHECK(status == std::future_status::ready);
  CHECK(listener.timestamps == std::vector<std::int64_t>{1000, 1021, -1});
  if (buffered) {
    CHECK(immediately_delivered.empty());
  } else {
    CHECK(immediately_delivered == listener.timestamps);
  }
}

}  // namespace
