/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_SRtPLAYERIMP_H
#define ZLMEDIAKIT_SRtPLAYERIMP_H

#include "SrtPlayer.h"
#include "Rtp/Decoder.h"
#include "TS/TSMediaSource.h"

namespace mediakit {

class SrtPlayerImp
    : public PlayerImp<SrtPlayer, PlayerBase>
    , private TrackListener {
public:
    using Ptr = std::shared_ptr<SrtPlayerImp>;
    using Super = PlayerImp<SrtPlayer, PlayerBase>;

    SrtPlayerImp(const toolkit::EventPoller::Ptr &poller) : Super(poller) {}
    ~SrtPlayerImp() override { DebugL; }

    void teardown() override;

private:
    //// SrtPlayer override////
    void onSRTData(const toolkit::Buffer::Ptr &buffer) override;

    //// PlayerBase override////
    void onPlayResult(const toolkit::SockException &ex) override;
    std::vector<Track::Ptr> getTracks(bool ready = true) const override;

private:
    //// TrackListener override////
    bool addTrack(const Track::Ptr &track) override { return true; }
    void addTrackCompleted() override;
    void failTrackReady(const toolkit::SockException &ex);

private:
    // for player
    DecoderImp::Ptr _decoder;
    MediaSinkInterface::Ptr _demuxer;
    toolkit::Timer::Ptr _track_ready_timer;
    bool _play_result_emitted = false;

    // for pusher
    TSMediaSource::RingType::RingReader::Ptr _ts_reader;
};

} /* namespace mediakit */
#endif /* ZLMEDIAKIT_SRtPLAYERIMP_H */
