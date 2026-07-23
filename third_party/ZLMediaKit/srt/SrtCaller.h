/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_SRTCALLER_H
#define ZLMEDIAKIT_SRTCALLER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "Network/Buffer.h"
#include "Network/Socket.h"
#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "SrtEpollReactor.h"
#include "Util/SpeedStatistic.h"

namespace mediakit {

class SrtUrl {
public:
    void parse(const std::string &url);

public:
    std::string _full_url;
    std::string _params;
    std::string _streamid;
    sockaddr_storage _addr {};

private:
    uint16_t _port = 0;
    std::string _host;
};

/**
 * Caller-side SRT transport backed by libsrt.
 *
 * libsrt I/O runs on the shared SrtEpollReactor thread. All derived-class
 * callbacks are marshalled to the owning EventPoller.
 */
class SrtCaller : public std::enable_shared_from_this<SrtCaller> {
public:
    using Ptr = std::shared_ptr<SrtCaller>;

    explicit SrtCaller(const toolkit::EventPoller::Ptr &poller);
    virtual ~SrtCaller();

    const toolkit::EventPoller::Ptr &getPoller() const { return _poller; }

    virtual void onSendTSData(const toolkit::Buffer::Ptr &buffer, bool flush);

    size_t getRecvSpeed() const;
    size_t getRecvTotalBytes() const;
    size_t getSendSpeed() const;
    size_t getSendTotalBytes() const;

protected:
    virtual void onConnect();
    virtual void teardownSrt();
    virtual void onHandShakeFinished();
    virtual void onResult(const toolkit::SockException &ex, bool was_connected) = 0;
    virtual void onSRTData(const toolkit::Buffer::Ptr &buffer);

    void reportSrtError(const toolkit::SockException &ex);

    virtual uint16_t getLatency() = 0;
    virtual float getTimeOutSec() = 0;
    virtual bool isPlayer() = 0;
    virtual std::string getPassphrase() = 0;

protected:
    SrtUrl _url;
    toolkit::EventPoller::Ptr _poller;

private:
    enum class State : uint8_t {
        Idle,
        Connecting,
        Connected,
        Closing,
        Closed
    };

    struct SendPacket {
        toolkit::Buffer::Ptr buffer;
        size_t offset = 0;
        size_t size = 0;
        uint64_t generation = 0;
        uint64_t packet_id = 0;
    };

    struct ReceivePacket {
        toolkit::BufferRaw::Ptr buffer;
        uint64_t generation = 0;
    };

    bool configureSocket(int32_t fd, toolkit::SockException &ex);
    void prepareReceiveSlots();
    void onReactorEvent(int32_t fd,
                        SrtEpollReactor::RegistrationToken token,
                        int events,
                        uint64_t generation);
    void handleConnected(uint64_t generation);
    void handleError(const toolkit::SockException &ex, uint64_t generation);
    void postError(const toolkit::SockException &ex, uint64_t generation);
    void drainReceive(int32_t fd, SrtEpollReactor::RegistrationToken token, uint64_t generation);
    void drainReceiveQueue(uint64_t generation);
    void drainSend(int32_t fd, SrtEpollReactor::RegistrationToken token, uint64_t generation);
    void teardownSrt_l();
    void closeSocket();
    void clearQueues();
    bool updateSocketEvents(int32_t fd,
                            SrtEpollReactor::RegistrationToken token,
                            uint64_t generation,
                            int events);

private:
    static constexpr size_t kMaxQueueBytes = 8 * 1024 * 1024;
    static constexpr size_t kLivePayloadSize = 1316;
    static constexpr size_t kLiveMaxPayloadSize = 1456;
    static constexpr size_t kReceiveSlotCount = 1024;
    static constexpr size_t kMaxPacketsPerEvent = 256;

    mutable std::mutex _socket_mutex;
    int32_t _socket = -1;
    SrtEpollReactor::RegistrationToken _registration_token =
        SrtEpollReactor::kInvalidRegistrationToken;
    std::atomic<State> _state {State::Idle};
    std::atomic<uint64_t> _generation {0};
    std::atomic<bool> _connect_posted {false};
    std::atomic<bool> _terminal_posted {false};

    std::mutex _recv_mutex;
    std::deque<toolkit::BufferRaw::Ptr> _recv_free_slots;
    std::deque<ReceivePacket> _recv_queue;
    size_t _recv_slot_count = 0;
    bool _recv_drain_scheduled = false;
    bool _recv_input_paused = false;

    std::mutex _send_mutex;
    std::deque<SendPacket> _send_queue;
    size_t _send_queue_bytes = 0;
    uint64_t _next_send_packet_id = 1;

    mutable std::mutex _stat_mutex;
    mutable toolkit::BytesSpeed _recv_speed;
    mutable toolkit::BytesSpeed _send_speed;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_SRTCALLER_H
