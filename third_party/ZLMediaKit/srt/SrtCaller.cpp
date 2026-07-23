/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "SrtCaller.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <srt/srt.h>

#include "Common/Parser.h"
#include "SrtEpollReactor.h"
#include "Util/util.h"

using namespace toolkit;
using namespace std;

namespace mediakit {

constexpr size_t SrtCaller::kMaxQueueBytes;
constexpr size_t SrtCaller::kLivePayloadSize;
constexpr size_t SrtCaller::kLiveMaxPayloadSize;
constexpr size_t SrtCaller::kReceiveSlotCount;
constexpr size_t SrtCaller::kMaxPacketsPerEvent;

namespace {

SockException makeSrtException(const string &operation, int srt_error) {
    ErrCode code = Err_other;
    switch (srt_error) {
        case SRT_ETIMEOUT:
            code = Err_timeout;
            break;
        case SRT_ECONNREJ:
            code = Err_refused;
            break;
        case SRT_ECONNLOST:
        case SRT_ENOCONN:
            code = Err_reset;
            break;
        default:
            break;
    }
    const char *detail = srt_getlasterror_str();
    return SockException(code, operation + ": " + (detail ? detail : "unknown libsrt error"), srt_error);
}

SockException makeSocketStateException(SRTSOCKET fd, bool was_connected) {
    if (!was_connected) {
        auto reason = srt_getrejectreason(fd);
        const char *detail = srt_rejectreason_str(reason);
        return SockException(Err_refused,
                             string("srt connection rejected: ") + (detail ? detail : "unknown reason"),
                             reason);
    }
    return SockException(Err_reset, "srt connection closed");
}

template <typename T>
bool setSocketOption(SRTSOCKET fd, SRT_SOCKOPT option, const T &value, const char *name, SockException &ex) {
    if (srt_setsockflag(fd, option, &value, sizeof(value)) != SRT_ERROR) {
        return true;
    }
    auto error = srt_getlasterror(nullptr);
    ex = makeSrtException(string("set ") + name, error);
    return false;
}

} // namespace

// srt://127.0.0.1:9000?streamid=#!::r=live/test
// srt://127.0.0.1:9000?streamid=#!::r=live/test,m=publish
void SrtUrl::parse(const string &url) {
    _full_url = url;
    _params.clear();
    _streamid.clear();
    _host.clear();
    _port = 0;
    memset(&_addr, 0, sizeof(_addr));

    auto address = findSubString(url.data(), "://", "?");
    splitUrl(address, _host, _port);
    if (_host.empty() || !_port) {
        throw invalid_argument("missing host or port");
    }

    if (!SockUtil::getDomainIP(_host.c_str(), _port, _addr, AF_UNSPEC, SOCK_DGRAM, IPPROTO_UDP)) {
        throw invalid_argument("invalid host: " + _host);
    }

    _params = findSubString(url.data(), "?", nullptr);
    auto args = Parser::parseArgs(_params);
    auto it = args.find("streamid");
    if (it != args.end()) {
        _streamid = it->second;
    }
}

SrtCaller::SrtCaller(const EventPoller::Ptr &poller)
    : _poller(poller ? poller : EventPollerPool::Instance().getPoller()) {}

SrtCaller::~SrtCaller() {
    teardownSrt();
}

void SrtCaller::prepareReceiveSlots() {
    if (!isPlayer()) {
        return;
    }

    lock_guard<mutex> lock(_recv_mutex);
    while (_recv_slot_count < kReceiveSlotCount) {
        _recv_free_slots.emplace_back(BufferRaw::create(kLiveMaxPayloadSize));
        ++_recv_slot_count;
    }
}

bool SrtCaller::configureSocket(int32_t socket, SockException &ex) {
    auto fd = static_cast<SRTSOCKET>(socket);
    auto transport = SRTT_LIVE;
    bool async = false;
    bool sender = !isPlayer();
    auto timeout_ms = max(1, static_cast<int>(getTimeOutSec() * 1000));
    auto payload_size = static_cast<int>(kLivePayloadSize);

    if (!setSocketOption(fd, SRTO_TRANSTYPE, transport, "SRTO_TRANSTYPE", ex)
        || !setSocketOption(fd, SRTO_RCVSYN, async, "SRTO_RCVSYN", ex)
        || !setSocketOption(fd, SRTO_SNDSYN, async, "SRTO_SNDSYN", ex)
        || !setSocketOption(fd, SRTO_SENDER, sender, "SRTO_SENDER", ex)
        || !setSocketOption(fd, SRTO_CONNTIMEO, timeout_ms, "SRTO_CONNTIMEO", ex)) {
        return false;
    }

    if (sender && !setSocketOption(fd, SRTO_PAYLOADSIZE, payload_size, "SRTO_PAYLOADSIZE", ex)) {
        return false;
    }

    auto latency = static_cast<int>(getLatency());
    if (latency > 0 && !setSocketOption(fd, SRTO_LATENCY, latency, "SRTO_LATENCY", ex)) {
        return false;
    }

    if (!_url._streamid.empty()
        && srt_setsockflag(fd, SRTO_STREAMID, _url._streamid.data(), static_cast<int>(_url._streamid.size())) == SRT_ERROR) {
        auto error = srt_getlasterror(nullptr);
        ex = makeSrtException("set SRTO_STREAMID", error);
        return false;
    }

    auto passphrase = getPassphrase();
    if (!passphrase.empty()
        && srt_setsockflag(fd, SRTO_PASSPHRASE, passphrase.data(), static_cast<int>(passphrase.size())) == SRT_ERROR) {
        auto error = srt_getlasterror(nullptr);
        ex = makeSrtException("set SRTO_PASSPHRASE", error);
        return false;
    }
    return true;
}

void SrtCaller::onConnect() {
    teardownSrt();

    auto &reactor = SrtEpollReactor::Instance();
    if (!reactor.available()) {
        onResult(SockException(Err_other, "libsrt reactor is unavailable"), false);
        return;
    }

    try {
        prepareReceiveSlots();
    } catch (const exception &ex) {
        onResult(SockException(Err_other, string("allocate srt receive slots failed: ") + ex.what()), false);
        return;
    }

    auto fd = srt_create_socket();
    if (fd == SRT_INVALID_SOCK) {
        auto error = srt_getlasterror(nullptr);
        onResult(makeSrtException("create srt socket", error), false);
        return;
    }

    SockException configure_error;
    if (!configureSocket(fd, configure_error)) {
        srt_close(fd);
        onResult(configure_error, false);
        return;
    }

    const auto generation = _generation.fetch_add(1) + 1;
    _connect_posted = false;
    _terminal_posted = false;
    _state = State::Connecting;
    {
        lock_guard<mutex> lock(_socket_mutex);
        _socket = fd;
        _registration_token = SrtEpollReactor::kInvalidRegistrationToken;
    }

    weak_ptr<SrtCaller> weak_self = shared_from_this();
    auto token = reactor.registerSocket(fd, SRT_EPOLL_OUT | SRT_EPOLL_ERR,
                                        [weak_self, generation](SRTSOCKET socket,
                                                                SrtEpollReactor::RegistrationToken token,
                                                                int events) {
                                            auto strong_self = weak_self.lock();
                                            if (strong_self) {
                                                strong_self->onReactorEvent(socket, token, events, generation);
                                            }
                                        });
    if (token == SrtEpollReactor::kInvalidRegistrationToken) {
        {
            lock_guard<mutex> lock(_socket_mutex);
            if (_socket == fd) {
                _socket = SRT_INVALID_SOCK;
            }
        }
        srt_close(fd);
        auto ex = SockException(Err_other, "register srt socket to reactor failed");
        _terminal_posted = true;
        handleError(ex, generation);
        return;
    }
    {
        lock_guard<mutex> lock(_socket_mutex);
        _socket = fd;
        _registration_token = token;
    }

    auto addr = reinterpret_cast<const sockaddr *>(&_url._addr);
    auto result = srt_connect(fd, addr, SockUtil::get_sock_len(addr));
    if (result == SRT_ERROR) {
        auto error = srt_getlasterror(nullptr);
        if (error != SRT_EASYNCSND && error != SRT_EASYNCRCV) {
            _terminal_posted = true;
            handleError(makeSrtException("connect srt socket", error), generation);
            return;
        }
    } else {
        onReactorEvent(fd, token, SRT_EPOLL_OUT, generation);
    }
}

void SrtCaller::onReactorEvent(int32_t fd,
                               SrtEpollReactor::RegistrationToken token,
                               int events,
                               uint64_t generation) {
    if (_generation != generation || _terminal_posted) {
        return;
    }

    auto state = _state.load();
    if (events & SRT_EPOLL_ERR) {
        postError(makeSocketStateException(fd, state == State::Connected), generation);
        return;
    }

    if (state == State::Connecting && (events & SRT_EPOLL_OUT)) {
        bool connected = false;
        {
            lock_guard<mutex> lock(_socket_mutex);
            connected = _socket == fd && _generation == generation
                        && srt_getsockstate(fd) == SRTS_CONNECTED;
        }
        if (!connected || _connect_posted.exchange(true)) {
            return;
        }

        auto interest = isPlayer() ? (SRT_EPOLL_IN | SRT_EPOLL_ERR) : SRT_EPOLL_ERR;
        if (!updateSocketEvents(fd, token, generation, interest)) {
            postError(SockException(Err_other, "update connected srt socket events failed"), generation);
            return;
        }

        weak_ptr<SrtCaller> weak_self = shared_from_this();
        _poller->async([weak_self, generation]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->handleConnected(generation);
            }
        });
        return;
    }

    if (state != State::Connected) {
        return;
    }
    if (isPlayer() && (events & SRT_EPOLL_IN)) {
        drainReceive(fd, token, generation);
    }
    if (!isPlayer() && (events & SRT_EPOLL_OUT)) {
        drainSend(fd, token, generation);
    }
}

void SrtCaller::handleConnected(uint64_t generation) {
    if (_generation != generation || _terminal_posted) {
        return;
    }
    auto expected = State::Connecting;
    if (!_state.compare_exchange_strong(expected, State::Connected)) {
        return;
    }
    onHandShakeFinished();
}

void SrtCaller::onHandShakeFinished() {}

void SrtCaller::reportSrtError(const SockException &ex) {
    postError(ex, _generation.load());
}

void SrtCaller::postError(const SockException &ex, uint64_t generation) {
    if (_generation != generation || _terminal_posted.exchange(true)) {
        return;
    }
    weak_ptr<SrtCaller> weak_self = shared_from_this();
    _poller->async([weak_self, ex, generation]() {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->handleError(ex, generation);
        }
    });
}

void SrtCaller::handleError(const SockException &ex, uint64_t generation) {
    if (_generation != generation) {
        return;
    }
    auto state = _state.load();
    if (state == State::Closing || state == State::Closed || state == State::Idle) {
        return;
    }
    auto was_connected = state == State::Connected;
    _state = State::Closing;
    closeSocket();
    clearQueues();
    _state = State::Closed;
    onResult(ex, was_connected);
}

void SrtCaller::teardownSrt() {
    if (_poller && !_poller->isCurrentThread()) {
        _poller->sync([this]() { teardownSrt_l(); });
        return;
    }
    teardownSrt_l();
}

void SrtCaller::teardownSrt_l() {
    _terminal_posted = true;
    _generation.fetch_add(1);
    _state = State::Closing;
    closeSocket();
    clearQueues();
    _state = State::Closed;
}

void SrtCaller::closeSocket() {
    SRTSOCKET fd = SRT_INVALID_SOCK;
    auto token = SrtEpollReactor::kInvalidRegistrationToken;
    {
        lock_guard<mutex> lock(_socket_mutex);
        fd = static_cast<SRTSOCKET>(_socket);
        token = _registration_token;
        _socket = SRT_INVALID_SOCK;
        _registration_token = SrtEpollReactor::kInvalidRegistrationToken;
    }
    if (fd == SRT_INVALID_SOCK) {
        return;
    }
    SrtEpollReactor::Instance().unregisterSocket(fd, token);
    srt_close(fd);
}

void SrtCaller::clearQueues() {
    {
        lock_guard<mutex> lock(_recv_mutex);
        while (!_recv_queue.empty()) {
            auto packet = std::move(_recv_queue.front());
            _recv_queue.pop_front();
            packet.buffer->setSize(0);
            _recv_free_slots.emplace_back(std::move(packet.buffer));
        }
        _recv_drain_scheduled = false;
        _recv_input_paused = false;
    }
    {
        lock_guard<mutex> lock(_send_mutex);
        _send_queue.clear();
        _send_queue_bytes = 0;
    }
}

bool SrtCaller::updateSocketEvents(int32_t fd,
                                   SrtEpollReactor::RegistrationToken token,
                                   uint64_t generation,
                                   int events) {
    {
        lock_guard<mutex> lock(_socket_mutex);
        if (_socket != fd || _generation != generation || _terminal_posted) {
            return false;
        }
        if (_registration_token != SrtEpollReactor::kInvalidRegistrationToken
            && _registration_token != token) {
            return false;
        }
    }
    return SrtEpollReactor::Instance().updateSocket(fd, token, events);
}

void SrtCaller::drainReceive(int32_t fd,
                             SrtEpollReactor::RegistrationToken token,
                             uint64_t generation) {
    for (size_t count = 0; count < kMaxPacketsPerEvent; ++count) {
        BufferRaw::Ptr buffer;
        bool pause_failed = false;
        {
            lock_guard<mutex> lock(_recv_mutex);
            if (_generation != generation || _terminal_posted || _state != State::Connected) {
                return;
            }
            if (_recv_free_slots.empty()) {
                if (!_recv_input_paused) {
                    _recv_input_paused = true;
                    pause_failed = !updateSocketEvents(fd, token, generation, SRT_EPOLL_ERR);
                }
            } else {
                buffer = std::move(_recv_free_slots.front());
                _recv_free_slots.pop_front();
            }
        }
        if (!buffer) {
            if (pause_failed && _generation == generation && !_terminal_posted) {
                postError(SockException(Err_other, "pause srt readable event failed"), generation);
            }
            return;
        }

        buffer->setSize(0);
        SRT_MSGCTRL control = srt_msgctrl_default;
        int received = SRT_ERROR;
        int error = SRT_SUCCESS;
        bool transport_stale = false;
        {
            lock_guard<mutex> lock(_socket_mutex);
            if (_socket != fd || _generation != generation || _state != State::Connected) {
                transport_stale = true;
            } else {
                received = srt_recvmsg2(fd, buffer->data(), static_cast<int>(buffer->getCapacity()), &control);
                if (received == SRT_ERROR) {
                    error = srt_getlasterror(nullptr);
                }
            }
        }
        if (transport_stale) {
            lock_guard<mutex> lock(_recv_mutex);
            _recv_free_slots.emplace_back(std::move(buffer));
            return;
        }

        if (received == SRT_ERROR) {
            {
                lock_guard<mutex> lock(_recv_mutex);
                _recv_free_slots.emplace_back(std::move(buffer));
            }
            if (error != SRT_EASYNCRCV) {
                postError(makeSrtException("receive srt data", error), generation);
            }
            return;
        }
        if (received == 0) {
            {
                lock_guard<mutex> lock(_recv_mutex);
                _recv_free_slots.emplace_back(std::move(buffer));
            }
            postError(SockException(Err_eof, "srt peer closed the connection"), generation);
            return;
        }

        buffer->setSize(static_cast<size_t>(received));
        bool schedule = false;
        bool stale = false;
        pause_failed = false;
        {
            lock_guard<mutex> lock(_recv_mutex);
            stale = _generation != generation || _terminal_posted || _state != State::Connected;
            if (stale) {
                buffer->setSize(0);
                _recv_free_slots.emplace_back(std::move(buffer));
            } else {
                ReceivePacket packet;
                packet.buffer = std::move(buffer);
                packet.generation = generation;
                _recv_queue.emplace_back(std::move(packet));
                if (!_recv_drain_scheduled) {
                    _recv_drain_scheduled = true;
                    schedule = true;
                }
                if (_recv_free_slots.empty() && !_recv_input_paused) {
                    _recv_input_paused = true;
                    pause_failed = !updateSocketEvents(fd, token, generation, SRT_EPOLL_ERR);
                }
            }
        }
        if (stale) {
            return;
        }
        if (pause_failed) {
            postError(SockException(Err_other, "pause srt readable event failed"), generation);
            return;
        }

        {
            lock_guard<mutex> lock(_stat_mutex);
            _recv_speed += static_cast<size_t>(received);
        }

        if (schedule) {
            weak_ptr<SrtCaller> weak_self = shared_from_this();
            _poller->async([weak_self, generation]() {
                auto strong_self = weak_self.lock();
                if (strong_self) {
                    strong_self->drainReceiveQueue(generation);
                }
            });
        }
    }
}

void SrtCaller::drainReceiveQueue(uint64_t generation) {
    deque<ReceivePacket> pending;
    {
        lock_guard<mutex> lock(_recv_mutex);
        if (_generation != generation || _terminal_posted || _state != State::Connected) {
            return;
        }
        pending.swap(_recv_queue);
        _recv_drain_scheduled = false;
    }

    bool resume_failed = false;
    for (auto &packet : pending) {
        if (packet.generation == generation
            && _generation == generation
            && _state == State::Connected
            && !_terminal_posted) {
            onSRTData(packet.buffer);
        }

        packet.buffer->setSize(0);
        lock_guard<mutex> lock(_recv_mutex);
        _recv_free_slots.emplace_back(std::move(packet.buffer));
        if (_recv_input_paused
            && _generation == generation
            && _state == State::Connected
            && !_terminal_posted) {
            SRTSOCKET fd;
            SrtEpollReactor::RegistrationToken token;
            {
                lock_guard<mutex> socket_lock(_socket_mutex);
                fd = static_cast<SRTSOCKET>(_socket);
                token = _registration_token;
            }
            if (fd != SRT_INVALID_SOCK
                && token != SrtEpollReactor::kInvalidRegistrationToken) {
                _recv_input_paused = false;
                resume_failed = !updateSocketEvents(
                    fd, token, generation, SRT_EPOLL_IN | SRT_EPOLL_ERR);
            }
        }
    }

    if (resume_failed && _generation == generation && !_terminal_posted) {
        postError(SockException(Err_other, "resume srt readable event failed"), generation);
    }
}

void SrtCaller::onSRTData(const Buffer::Ptr &) {
    if (!isPlayer()) {
        WarnL << "ignore received SRT data on pusher";
    }
}

void SrtCaller::onSendTSData(const Buffer::Ptr &buffer, bool) {
    const auto generation = _generation.load();
    if (!buffer || !buffer->size() || _state != State::Connected || _terminal_posted) {
        return;
    }

    bool overflow = false;
    bool update_failed = false;
    {
        lock_guard<mutex> lock(_send_mutex);
        if (_generation != generation || _state != State::Connected || _terminal_posted) {
            return;
        }
        overflow = buffer->size() > kMaxQueueBytes - min(_send_queue_bytes, kMaxQueueBytes);
        if (!overflow) {
            for (size_t offset = 0; offset < buffer->size(); offset += kLivePayloadSize) {
                SendPacket packet;
                packet.buffer = buffer;
                packet.offset = offset;
                packet.size = min(kLivePayloadSize, buffer->size() - offset);
                packet.generation = generation;
                packet.packet_id = _next_send_packet_id++;
                _send_queue.emplace_back(std::move(packet));
            }
            _send_queue_bytes += buffer->size();

            int32_t fd;
            SrtEpollReactor::RegistrationToken token;
            {
                lock_guard<mutex> socket_lock(_socket_mutex);
                fd = _socket;
                token = _registration_token;
            }
            if (fd != SRT_INVALID_SOCK
                && token != SrtEpollReactor::kInvalidRegistrationToken) {
                update_failed =
                    !updateSocketEvents(fd, token, generation, SRT_EPOLL_OUT | SRT_EPOLL_ERR);
            }
        }
    }

    if (overflow) {
        postError(SockException(Err_other, "srt send queue exceeded 8 MiB"), generation);
        return;
    }
    if (update_failed
        && _generation == generation
        && _state == State::Connected
        && !_terminal_posted) {
        postError(SockException(Err_other, "enable srt writable event failed"), generation);
    }
}

void SrtCaller::drainSend(int32_t fd,
                          SrtEpollReactor::RegistrationToken token,
                          uint64_t generation) {
    for (size_t count = 0; count < kMaxPacketsPerEvent; ++count) {
        SendPacket packet;
        {
            lock_guard<mutex> lock(_send_mutex);
            if (_send_queue.empty() || _send_queue.front().generation != generation) {
                break;
            }
            packet = _send_queue.front();
        }

        SRT_MSGCTRL control = srt_msgctrl_default;
        int sent = SRT_ERROR;
        int error = SRT_SUCCESS;
        {
            lock_guard<mutex> lock(_socket_mutex);
            if (_socket != fd || _generation != generation || _state != State::Connected) {
                return;
            }
            sent = srt_sendmsg2(fd, packet.buffer->data() + packet.offset, static_cast<int>(packet.size), &control);
            if (sent == SRT_ERROR) {
                error = srt_getlasterror(nullptr);
            }
        }

        if (sent == SRT_ERROR) {
            if (error != SRT_EASYNCSND) {
                postError(makeSrtException("send srt data", error), generation);
            }
            return;
        }
        if (static_cast<size_t>(sent) != packet.size) {
            postError(SockException(Err_other, "libsrt returned a partial live message"), generation);
            return;
        }

        {
            lock_guard<mutex> lock(_send_mutex);
            if (!_send_queue.empty()
                && _send_queue.front().generation == packet.generation
                && _send_queue.front().packet_id == packet.packet_id) {
                _send_queue_bytes -= _send_queue.front().size;
                _send_queue.pop_front();
            } else {
                return;
            }
        }
        {
            lock_guard<mutex> lock(_stat_mutex);
            _send_speed += static_cast<size_t>(sent);
        }
    }

    bool update_failed = false;
    {
        lock_guard<mutex> lock(_send_mutex);
        auto interest = _send_queue.empty() ? SRT_EPOLL_ERR : (SRT_EPOLL_OUT | SRT_EPOLL_ERR);
        update_failed = !updateSocketEvents(fd, token, generation, interest);
    }
    if (update_failed
        && _generation == generation
        && _state == State::Connected
        && !_terminal_posted) {
        postError(SockException(Err_other, "update srt writable event failed"), generation);
    }
}

size_t SrtCaller::getRecvSpeed() const {
    lock_guard<mutex> lock(_stat_mutex);
    return _recv_speed.getSpeed();
}

size_t SrtCaller::getRecvTotalBytes() const {
    lock_guard<mutex> lock(_stat_mutex);
    return _recv_speed.getTotalBytes();
}

size_t SrtCaller::getSendSpeed() const {
    lock_guard<mutex> lock(_stat_mutex);
    return _send_speed.getSpeed();
}

size_t SrtCaller::getSendTotalBytes() const {
    lock_guard<mutex> lock(_stat_mutex);
    return _send_speed.getTotalBytes();
}

} // namespace mediakit
