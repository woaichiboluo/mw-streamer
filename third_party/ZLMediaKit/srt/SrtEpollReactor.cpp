/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "SrtEpollReactor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace mediakit {

namespace {

std::atomic<SrtEpollReactor *> g_reactor_instance { nullptr };

} // namespace

class SrtEpollReactor::Impl {
public:
    using EventCallback = SrtEpollReactor::EventCallback;
    using RegistrationToken = SrtEpollReactor::RegistrationToken;

    struct Registration {
        RegistrationToken token;
        EventCallback callback;
    };

    Impl() {
        if (srt_startup() == SRT_ERROR) {
            return;
        }
        _srt_started = true;

        _eid = srt_epoll_create();
        if (_eid < 0) {
            srt_cleanup();
            _srt_started = false;
            return;
        }

        if (srt_epoll_set(_eid, SRT_EPOLL_ENABLE_EMPTY) == SRT_ERROR) {
            srt_epoll_release(_eid);
            _eid = -1;
            srt_cleanup();
            _srt_started = false;
            return;
        }

        _running = true;
        try {
            _thread = std::thread(&Impl::run, this);
            _thread_id = _thread.get_id();
        } catch (...) {
            _running = false;
            srt_epoll_release(_eid);
            _eid = -1;
            srt_cleanup();
            _srt_started = false;
        }
    }

    ~Impl() { shutdown(); }

    RegistrationToken registerSocket(SRTSOCKET fd, int events, EventCallback callback) {
        if (fd == SRT_INVALID_SOCK || !callback || !_running) {
            return SrtEpollReactor::kInvalidRegistrationToken;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || _eid < 0 || _callbacks.count(fd)) {
            return SrtEpollReactor::kInvalidRegistrationToken;
        }

        auto token = nextRegistrationToken();
        _callbacks.emplace(fd, Registration { token, std::move(callback) });
        if (srt_epoll_add_usock(_eid, fd, &events) == SRT_ERROR) {
            _callbacks.erase(fd);
            return SrtEpollReactor::kInvalidRegistrationToken;
        }
        ++_ownership_epoch;
        return token;
    }

    bool updateSocket(SRTSOCKET fd, RegistrationToken token, int events) {
        if (fd == SRT_INVALID_SOCK || token == SrtEpollReactor::kInvalidRegistrationToken || !_running) {
            return false;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _callbacks.find(fd);
        if (!_running || _eid < 0 || it == _callbacks.end() || it->second.token != token) {
            return false;
        }
        return srt_epoll_update_usock(_eid, fd, &events) != SRT_ERROR;
    }

    bool unregisterSocket(SRTSOCKET fd, RegistrationToken token) {
        if (fd == SRT_INVALID_SOCK || token == SrtEpollReactor::kInvalidRegistrationToken) {
            return false;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _callbacks.find(fd);
        if (it == _callbacks.end() || it->second.token != token) {
            return false;
        }

        _callbacks.erase(it);
        ++_ownership_epoch;
        if (_eid < 0) {
            return true;
        }
        return srt_epoll_remove_usock(_eid, fd) != SRT_ERROR;
    }

    bool available() const { return _running; }

    void shutdown() {
        _running = false;

        if (std::this_thread::get_id() == _thread_id) {
            return;
        }

        std::unique_lock<std::mutex> lock(_shutdown_mutex);
        if (_shutdown_complete) {
            return;
        }
        if (_join_in_progress) {
            _shutdown_cv.wait(lock, [this]() { return _shutdown_complete; });
            return;
        }

        _join_in_progress = true;
        lock.unlock();
        if (_thread.joinable()) {
            _thread.join();
        }
        lock.lock();
        _shutdown_complete = true;
        _join_in_progress = false;
        lock.unlock();
        _shutdown_cv.notify_all();
    }

private:
    RegistrationToken nextRegistrationToken() {
        auto token = _next_registration_token++;
        if (token == SrtEpollReactor::kInvalidRegistrationToken) {
            token = _next_registration_token++;
        }
        return token;
    }

    void run() {
        std::array<SRT_EPOLL_EVENT, 64> ready_events;
        while (_running) {
            const auto ownership_epoch = _ownership_epoch.load();
            auto count = srt_epoll_uwait(_eid, ready_events.data(), static_cast<int>(ready_events.size()), 100);
            if (!_running) {
                break;
            }
            if (count <= 0) {
                continue;
            }

            count = std::min<int>(count, ready_events.size());
            for (int i = 0; i < count; ++i) {
                if (!_running) {
                    break;
                }
                EventCallback callback;
                RegistrationToken token = SrtEpollReactor::kInvalidRegistrationToken;
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    // A descriptor may be reused after uwait returned. Discard
                    // that batch if ownership changed; level-triggered state is
                    // reported again with the new registration on the next wait.
                    if (ownership_epoch != _ownership_epoch.load()) {
                        break;
                    }
                    auto it = _callbacks.find(ready_events[i].fd);
                    if (it != _callbacks.end()) {
                        token = it->second.token;
                        callback = it->second.callback;
                    }
                }

                if (callback) {
                    try {
                        callback(ready_events[i].fd, token, ready_events[i].events);
                    } catch (...) {
                        // Keep the shared reactor alive if an owner callback fails.
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _callbacks.clear();
            if (_eid >= 0) {
                srt_epoll_clear_usocks(_eid);
                srt_epoll_release(_eid);
                _eid = -1;
            }
        }
        if (_srt_started) {
            srt_cleanup();
            _srt_started = false;
        }
    }

private:
    mutable std::mutex _mutex;
    std::map<SRTSOCKET, Registration> _callbacks;
    std::mutex _shutdown_mutex;
    std::condition_variable _shutdown_cv;
    std::atomic<bool> _running { false };
    std::thread _thread;
    std::thread::id _thread_id;
    int _eid = -1;
    bool _srt_started = false;
    bool _join_in_progress = false;
    bool _shutdown_complete = false;
    RegistrationToken _next_registration_token = 1;
    std::atomic<uint64_t> _ownership_epoch { 0 };
};

constexpr SrtEpollReactor::RegistrationToken SrtEpollReactor::kInvalidRegistrationToken;

SrtEpollReactor &SrtEpollReactor::Instance() {
    static SrtEpollReactor instance;
    return instance;
}

bool SrtEpollReactor::isCreated() noexcept {
    return g_reactor_instance.load(std::memory_order_acquire) != nullptr;
}

void SrtEpollReactor::shutdownIfCreated() {
    auto *instance = g_reactor_instance.load(std::memory_order_acquire);
    if (instance) {
        instance->shutdown();
    }
}

SrtEpollReactor::SrtEpollReactor()
    : _impl(new Impl()) {
    g_reactor_instance.store(this, std::memory_order_release);
}

SrtEpollReactor::~SrtEpollReactor() {
    g_reactor_instance.store(nullptr, std::memory_order_release);
}

SrtEpollReactor::RegistrationToken SrtEpollReactor::registerSocket(SRTSOCKET fd, int events, EventCallback callback) {
    return _impl->registerSocket(fd, events, std::move(callback));
}

bool SrtEpollReactor::updateSocket(SRTSOCKET fd, RegistrationToken token, int events) {
    return _impl->updateSocket(fd, token, events);
}

bool SrtEpollReactor::unregisterSocket(SRTSOCKET fd, RegistrationToken token) {
    return _impl->unregisterSocket(fd, token);
}

bool SrtEpollReactor::available() const {
    return _impl->available();
}

void SrtEpollReactor::shutdown() {
    _impl->shutdown();
}

} // namespace mediakit
