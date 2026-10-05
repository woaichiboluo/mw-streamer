/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_SRTEPOLLREACTOR_H
#define ZLMEDIAKIT_SRTEPOLLREACTOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include <srt/srt.h>

namespace mediakit {

/**
 * Process-wide reactor for caller-side SRT sockets.
 *
 * Callbacks are invoked on the reactor thread. Owners should marshal callbacks
 * to their own EventPoller before touching player or pusher state.
 */
class SrtEpollReactor {
public:
    using RegistrationToken = uint64_t;
    using EventCallback = std::function<void(SRTSOCKET fd, RegistrationToken token, int events)>;

    static constexpr RegistrationToken kInvalidRegistrationToken = 0;

    // Returns the installed instance and initializes SRT on first use.
    static SrtEpollReactor &Instance();

    // Creates an owned, inactive reactor without installing it globally.
    static std::unique_ptr<SrtEpollReactor> createReactor();

    // Installs a non-owning instance; the owner serializes initialization and
    // shutdown and clears this pointer before destroying the reactor.
    static void setInstance(SrtEpollReactor *instance);

    // True only after the installed reactor has initialized its backend.
    static bool isCreated() noexcept;
    static void release();

    ~SrtEpollReactor();

    /**
     * Register a socket and its interested epoll events.
     * Returns a non-zero registration token on success. The token must be
     * supplied when updating or unregistering the socket, preventing an old
     * owner from mutating a reused SRT socket descriptor. An event may be
     * dispatched before this method returns, so the callback receives the same
     * token directly.
     */
    RegistrationToken registerSocket(SRTSOCKET fd, int events, EventCallback callback);

    /**
     * Replace the interested epoll events of a registered socket.
     */
    bool updateSocket(SRTSOCKET fd, RegistrationToken token, int events);

    /**
     * Remove a registered socket. The socket itself remains owned by the caller.
     */
    bool unregisterSocket(SRTSOCKET fd, RegistrationToken token);

    bool available() const;

    /**
     * Stop the reactor and wait for its thread when called from another thread.
     * This operation is idempotent. libsrt cleanup is performed by the reactor
     * thread before it exits.
     */
    void shutdown();

private:
    SrtEpollReactor();
    void ensureInitialized();

    SrtEpollReactor(const SrtEpollReactor &) = delete;
    SrtEpollReactor &operator=(const SrtEpollReactor &) = delete;

private:
    class Impl;
    std::mutex _init_mutex;
    std::atomic<bool> _initialized { false };
    std::unique_ptr<Impl> _impl;
};

} // namespace mediakit

#endif // ZLMEDIAKIT_SRTEPOLLREACTOR_H
