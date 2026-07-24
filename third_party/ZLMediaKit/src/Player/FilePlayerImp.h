/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_PLAYER_FILEPLAYERIMP_H
#define SRC_PLAYER_FILEPLAYERIMP_H

#ifdef ENABLE_MP4

#include "PlayerBase.h"
#include "Record/MP4Reader.h"

namespace mediakit {

class FilePlayerImp final : public PlayerImp<PlayerBase, PlayerBase>, public std::enable_shared_from_this<FilePlayerImp> {
public:
    using Ptr = std::shared_ptr<FilePlayerImp>;

    explicit FilePlayerImp(const toolkit::EventPoller::Ptr &poller);

    void play(const std::string &url) override;
    void pause(bool flag) override;
    void speed(float speed) override;
    void teardown() override;

    float getDuration() const override;
    float getProgress() const override;
    uint32_t getProgressPos() const override;
    void seekTo(float progress) override;
    void seekTo(uint32_t pos) override;

    std::vector<Track::Ptr> getTracks(bool ready = true) const override;
    bool isFinite() const override { return true; }

private:
    void onReadComplete(const std::weak_ptr<MP4Reader> &reader, const toolkit::SockException &ex);

private:
    toolkit::EventPoller::Ptr _poller;
    MP4Reader::Ptr _reader;
};

} // namespace mediakit

#endif // ENABLE_MP4
#endif // SRC_PLAYER_FILEPLAYERIMP_H
