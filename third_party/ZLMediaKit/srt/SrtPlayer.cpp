/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "SrtPlayer.h"
#include "SrtPlayerImp.h"
#include "Common/config.h"
#include "Http/HlsPlayer.h"

using namespace toolkit;
using namespace std;

namespace mediakit {


SrtPlayer::SrtPlayer(const EventPoller::Ptr &poller) 
    : SrtCaller(poller) {
    (*this)[Client::kSrtTrackReadyTimeoutMS] = 30000;
    DebugL;
}

SrtPlayer::~SrtPlayer(void) {
    DebugL;
}

void SrtPlayer::play(const string &strUrl) {
    DebugL;
    try {
        _url.parse(strUrl);
    } catch (std::exception &ex) {
        onResult(SockException(Err_other, StrPrinter << "illegal srt url:" << ex.what()), false);
        return;
    }

    weak_ptr<SrtPlayer> weak_self = static_pointer_cast<SrtPlayer>(shared_from_this());
    getPoller()->async([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->onConnect();
    });
    return;
}

void SrtPlayer::teardown() {
    if (!getPoller()->isCurrentThread()) {
        getPoller()->sync([this]() { teardown(); });
        return;
    }
    _check_timer.reset();
    teardownSrt();
}

void SrtPlayer::pause(bool bPause) {
    DebugL;
}

void SrtPlayer::speed(float speed) {
    DebugL;
}

void SrtPlayer::onHandShakeFinished() {
    onResult(SockException(Err_success, "srt play success"), false);
}

void SrtPlayer::onResult(const SockException &ex, bool was_connected) {
     if (!ex) {
        // 播放成功
        onPlayResult(ex);
        _benchmark_mode = (*this)[Client::kBenchmarkMode].as<int>();

        // 播放成功，恢复数据包接收超时定时器
        _recv_ticker.resetTime();
        auto timeout = getTimeOutSec();
        //读取配置文件
        weak_ptr<SrtPlayer> weakSelf = static_pointer_cast<SrtPlayer>(shared_from_this());
        // 创建rtp数据接收超时检测定时器
        _check_timer = std::make_shared<Timer>(timeout /2,
            [weakSelf, timeout]() {
                auto strongSelf = weakSelf.lock();
                if (!strongSelf) {
                    return false;
                }
                if (strongSelf->_recv_ticker.elapsedTime() > timeout * 1000) {
                    // 接收媒体数据包超时
                    strongSelf->reportSrtError(
                        SockException(Err_timeout, "receive srt media data timeout:" + strongSelf->_url._full_url));
                    return false;
                }

                return true;
            }, getPoller());
    } else {
        _check_timer.reset();
        WarnL << ex.getErrCode() << " " << ex.what();
        if (ex.getErrCode() == Err_shutdown) {
            // 主动shutdown的，不触发回调
            return;
        }
        if (was_connected) {
            onShutdown(ex);
        } else {
            onPlayResult(ex);
        }
    }
    return;
}


void SrtPlayer::onSRTData(const toolkit::Buffer::Ptr &buffer) {
    _recv_ticker.resetTime();
}

uint16_t SrtPlayer::getLatency() {
    auto latency = (*this)[Client::kLatency].as<uint16_t>();
    return (uint16_t)latency ;
}

float SrtPlayer::getTimeOutSec() {
    auto timeoutMS = (*this)[Client::kTimeoutMS].as<uint64_t>();
    return (float)timeoutMS / (float)1000;
}

std::string SrtPlayer::getPassphrase() {
    auto passPhrase = (*this)[Client::kPassPhrase].as<string>();
    return passPhrase;
}

size_t SrtPlayer::getRecvSpeed() {
    return SrtCaller::getRecvSpeed();
}

size_t SrtPlayer::getRecvTotalBytes() {
    return SrtCaller::getRecvTotalBytes();
}

///////////////////////////////////////////////////
// SrtPlayerImp

void SrtPlayerImp::teardown() {
    if (!getPoller()->isCurrentThread()) {
        getPoller()->sync([this]() { teardown(); });
        return;
    }
    _track_ready_timer.reset();
    _play_result_emitted = true;
    Super::teardown();
}

void SrtPlayerImp::onPlayResult(const toolkit::SockException &ex) {
    if (ex) {
        _track_ready_timer.reset();
        _play_result_emitted = true;
        Super::onPlayResult(ex);
        return;
    }

    auto timeout_ms = (*this)[Client::kSrtTrackReadyTimeoutMS].as<uint64_t>();
    if (timeout_ms) {
        std::weak_ptr<SrtPlayerImp> weak_self =
            std::static_pointer_cast<SrtPlayerImp>(shared_from_this());
        _track_ready_timer = std::make_shared<Timer>(
            timeout_ms / 1000.0f,
            [weak_self, timeout_ms]() {
                auto strong_self = weak_self.lock();
                if (strong_self && !strong_self->_play_result_emitted) {
                    strong_self->failTrackReady(SockException(
                        Err_timeout,
                        StrPrinter << "wait srt tracks ready timeout after " << timeout_ms << "ms"));
                }
                return false;
            },
            getPoller());
    }
    // success result only occurs when addTrackCompleted
}

std::vector<Track::Ptr> SrtPlayerImp::getTracks(bool ready /*= true*/) const {
    return _demuxer ? static_pointer_cast<HlsDemuxer>(_demuxer)->getTracks(ready) : Super::getTracks(ready);
}

void SrtPlayerImp::addTrackCompleted() {
    if (_play_result_emitted) {
        return;
    }
    _play_result_emitted = true;
    _track_ready_timer.reset();
    Super::onPlayResult(toolkit::SockException(toolkit::Err_success, "play success"));
}

void SrtPlayerImp::failTrackReady(const toolkit::SockException &ex) {
    if (_play_result_emitted) {
        return;
    }
    _play_result_emitted = true;
    _track_ready_timer.reset();
    SrtPlayer::teardown();
    _decoder.reset();
    _demuxer.reset();
    Super::onPlayResult(ex);
}

void SrtPlayerImp::onSRTData(const toolkit::Buffer::Ptr &buffer) {
    SrtPlayer::onSRTData(buffer);

    if (_benchmark_mode) {
        return;
    }

    if (!_demuxer) {
        auto demuxer = std::make_shared<HlsDemuxer>();
        // SRT/TS中途加入可能需要等待下一个长GOP关键帧。禁止MediaSink按全局
        // 10秒规则删除未就绪轨道；由SRT专属定时器明确上报播放失败。
        demuxer->setTrackReadyTimeoutMS(0);
        GET_CONFIG(bool, add_mute_audio, Protocol::kAddMuteAudio);
        auto &add_mute_audio_option = (*this)[Protocol::kAddMuteAudio];
        demuxer->enableMuteAudio(add_mute_audio_option.empty() ? add_mute_audio : add_mute_audio_option.as<bool>());
        demuxer->start(getPoller(), this);
        _demuxer = std::move(demuxer);
    }

    if (!_decoder && _demuxer) {
        _decoder = DecoderImp::createDecoder(DecoderImp::decoder_ts, _demuxer.get());
    }

    if (_decoder && _demuxer) {
        _decoder->input(reinterpret_cast<const uint8_t *>(buffer->data()), buffer->size());
    }

    return;
}

} /* namespace mediakit */

