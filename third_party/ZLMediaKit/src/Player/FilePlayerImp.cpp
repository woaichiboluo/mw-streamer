/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifdef ENABLE_MP4

#include <algorithm>
#include "FilePlayerImp.h"
#include "Util/File.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

FilePlayerImp::FilePlayerImp(const EventPoller::Ptr &poller)
    : _poller(poller ? poller : EventPollerPool::Instance().getPoller()) {}

void FilePlayerImp::play(const string &url) {
    teardown();

    MP4Reader::Ptr reader;
    try {
        if (!File::fileExist(url) || File::is_dir(url)) {
            throw invalid_argument("mp4文件不存在:" + url);
        }
        reader = make_shared<MP4Reader>(MediaTuple(), url);
    } catch (const exception &ex) {
        onPlayResult(SockException(Err_other, ex.what()));
        return;
    }

    _reader = reader;
    weak_ptr<FilePlayerImp> weak_self = shared_from_this();
    weak_ptr<MP4Reader> weak_reader = reader;
    reader->setOnComplete([weak_self, weak_reader](const SockException &ex) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        strong_self->_poller->async(
            [weak_self, weak_reader, ex]() {
                if (auto strong_self = weak_self.lock()) {
                    strong_self->onReadComplete(weak_reader, ex);
                }
            },
            false);
    });

    onPlayResult(SockException(Err_success, "play success"));
    if (_reader == reader) {
        reader->startReadMP4(0, false, false, false);
    }
}

void FilePlayerImp::pause(bool flag) {
    if (_reader) {
        _reader->pause(flag);
    }
}

void FilePlayerImp::speed(float speed) {
    if (_reader) {
        _reader->speed(speed);
    }
}

void FilePlayerImp::teardown() {
    auto reader = std::move(_reader);
    if (!reader) {
        return;
    }
    reader->stopReadMP4();
    reader->setOnComplete(nullptr);
}

float FilePlayerImp::getDuration() const {
    return _reader ? _reader->getDurationMS() / 1000.0f : 0;
}

float FilePlayerImp::getProgress() const {
    if (!_reader) {
        return 0;
    }
    auto duration = _reader->getDurationMS();
    return duration ? (float)_reader->getProgressMS() / duration : 0;
}

uint32_t FilePlayerImp::getProgressPos() const {
    return _reader ? _reader->getProgressMS() / 1000 : 0;
}

void FilePlayerImp::seekTo(float progress) {
    if (!_reader) {
        return;
    }
    progress = max(0.0f, min(progress, 1.0f));
    auto duration = _reader->getDurationMS();
    if (!duration) {
        return;
    }
    auto stamp = progress >= 1.0f ? duration - 1 : (uint64_t)(progress * duration);
    _reader->seekTo((uint32_t)stamp);
}

void FilePlayerImp::seekTo(uint32_t pos) {
    if (!_reader) {
        return;
    }
    auto duration = _reader->getDurationMS();
    if (!duration) {
        return;
    }
    auto stamp = min<uint64_t>((uint64_t)pos * 1000, duration - 1);
    _reader->seekTo((uint32_t)stamp);
}

vector<Track::Ptr> FilePlayerImp::getTracks(bool ready) const {
    return _reader ? _reader->getDemuxer()->getTracks(ready) : vector<Track::Ptr>();
}

void FilePlayerImp::onReadComplete(const weak_ptr<MP4Reader> &reader, const SockException &ex) {
    if (!_reader || reader.lock() != _reader) {
        return;
    }
    onShutdown(ex);
}

} // namespace mediakit

#endif // ENABLE_MP4
