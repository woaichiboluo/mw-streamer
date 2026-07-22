/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Device.h"
#include "Extension/Factory.h"
using namespace toolkit;
using namespace std;

namespace mediakit {

bool DevChannel::inputH264(const char *data, int len, uint64_t dts, uint64_t pts) {
    if (dts == 0) {
        dts = _aTicker[0].elapsedTime();
    }
    if (pts == 0) {
        pts = dts;
    }

    return inputFrame(Factory::getFrameFromPtr(CodecH264,data, len, dts, pts));
}

bool DevChannel::inputH265(const char *data, int len, uint64_t dts, uint64_t pts) {
    if (dts == 0) {
        dts = _aTicker[0].elapsedTime();
    }
    if (pts == 0) {
        pts = dts;
    }

    return inputFrame(Factory::getFrameFromPtr(CodecH265, data, len, dts, pts));
}

#define ADTS_HEADER_LEN 7

bool DevChannel::inputAAC(const char *data_without_adts, int len, uint64_t dts, const char *adts_header){
    if (dts == 0) {
        dts = _aTicker[1].elapsedTime();
    }

    if (!adts_header) {
        // 没有adts头  [AUTO-TRANSLATED:b9faaa83]
        // No adts header
        return inputFrame(std::make_shared<FrameFromPtr>(CodecAAC, (char *) data_without_adts, len, dts, 0, 0));
    }

    if (adts_header + ADTS_HEADER_LEN == data_without_adts) {
        // adts头和帧在一起  [AUTO-TRANSLATED:76c4e678]
        // adts header and frame together
        return inputFrame(std::make_shared<FrameFromPtr>(CodecAAC, (char *) data_without_adts - ADTS_HEADER_LEN, len + ADTS_HEADER_LEN, dts, 0, ADTS_HEADER_LEN));
    }

    // adts头和帧不在一起  [AUTO-TRANSLATED:591dd07a]
    // adts header and frame not together
    char *data_with_adts = new char[len + ADTS_HEADER_LEN];
    memcpy(data_with_adts, adts_header, ADTS_HEADER_LEN);
    memcpy(data_with_adts + ADTS_HEADER_LEN, data_without_adts, len);
    return inputFrame(std::make_shared<FrameAutoDelete>(CodecAAC, data_with_adts, len + ADTS_HEADER_LEN, dts, 0, ADTS_HEADER_LEN));
}

bool DevChannel::inputAudio(const char *data, int len, uint64_t dts){
    if (dts == 0) {
        dts = _aTicker[1].elapsedTime();
    }
    return inputFrame(Factory::getFrameFromPtr(_audio->codecId, (char *) data, len, dts, dts));
}

bool DevChannel::initVideo(const VideoInfo &info) {
    _video = std::make_shared<VideoInfo>(info);
    return addTrack(Factory::getTrackByCodecId(info.codecId));
}

bool DevChannel::initAudio(const AudioInfo &info) {
    _audio = std::make_shared<AudioInfo>(info);
    return addTrack(Factory::getTrackByCodecId(info.codecId, info.iSampleRate, info.iChannel, info.iSampleBit));
}

bool DevChannel::inputFrame(const Frame::Ptr &frame) {
    auto cached_frame = Frame::getCacheAbleFrame(frame);
    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
    getOwnerPoller(MediaSource::NullMediaSource())->async([weak_self, cached_frame]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->MultiMediaSourceMuxer::inputFrame(cached_frame);
        }
    });
    return true;
}

bool DevChannel::addTrack(const Track::Ptr &track) {
    bool ret;
    getOwnerPoller(MediaSource::NullMediaSource())->sync([&]() { ret = MultiMediaSourceMuxer::addTrack(track); });
    return ret;
}

void DevChannel::addTrackCompleted() {
    getOwnerPoller(MediaSource::NullMediaSource())->sync([&]() { MultiMediaSourceMuxer::addTrackCompleted(); });
}

MediaOriginType DevChannel::getOriginType(MediaSource &sender) const {
    return MediaOriginType::device_chn;
}

} /* namespace mediakit */
