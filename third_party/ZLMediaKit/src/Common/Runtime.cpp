/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Runtime.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

#include "Common/MediaSource.h"
#include "Http/HttpCookieManager.h"
#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "Util/util.h"
#include "mw/log.h"

#ifdef ENABLE_SRT
#include "srt/SrtEpollReactor.h"
#endif

namespace mediakit {

namespace {

std::atomic<bool> s_runtime_active { false };

} // namespace

class Runtime {
public:
    ~Runtime() {
        // Cookie attachments contain timestamp users and reader references;
        // their expiration timer also retains a poller. Release them before
        // closing the pools, while the timestamp clock is still installed.
        if (_http_cookie_manager) {
            _http_cookie_manager->shutdown();
            HttpCookieManager::setInstance(nullptr);
            _http_cookie_manager.reset();
        }
#ifdef ENABLE_SRT
        if (_srt_reactor) {
            _srt_reactor->shutdown();
            SrtEpollReactor::setInstance(nullptr);
            _srt_reactor.reset();
        }
#endif
        if (_work_pool) {
            _work_pool->close();
            toolkit::WorkThreadPool::setInstance(nullptr);
            _work_pool.reset();
        }
        if (_event_pool) {
            _event_pool->close();
            toolkit::EventPollerPool::setInstance(nullptr);
            _event_pool.reset();
        }
        // NullMediaSource contains timestamp users, so it must be destroyed
        // while the clock remains installed and running.
        if (_null_media_source) {
            MediaSource::setNullMediaSource(nullptr);
            _null_media_source.reset();
        }
        if (_clock) {
            _clock->stop();
            toolkit::setTimestampClock(nullptr);
            _clock.reset();
        }
        if (_network_initialized) {
            const int error = toolkit::SockUtil::release();
            if (error != 0) {
                MW_LOG_ERROR("zlm", "Failed to release network runtime: {}", error);
            }
        }
    }

    void initialize(const RuntimeConfig &config) {
        const int error = toolkit::SockUtil::initialize();
        if (error != 0) {
            throw std::runtime_error("Failed to initialize network runtime: " + std::to_string(error));
        }
        _network_initialized = true;

        _clock = std::make_unique<toolkit::TimestampClock>();
        toolkit::setTimestampClock(_clock.get());
        _null_media_source = MediaSource::createNullMediaSource();
        MediaSource::setNullMediaSource(_null_media_source.get());

        toolkit::EventPollerPool::setPoolSize(config.event_poller_threads);
        toolkit::EventPollerPool::enableCpuAffinity(config.enable_cpu_affinity);
        toolkit::WorkThreadPool::setPoolSize(config.work_threads);
        toolkit::WorkThreadPool::enableCpuAffinity(config.enable_cpu_affinity);
        _event_pool = toolkit::EventPollerPool::createPool();
        toolkit::EventPollerPool::setInstance(_event_pool.get());
        _work_pool = toolkit::WorkThreadPool::createPool();
        toolkit::WorkThreadPool::setInstance(_work_pool.get());
        _http_cookie_manager = HttpCookieManager::createManager();
        HttpCookieManager::setInstance(_http_cookie_manager.get());
#ifdef ENABLE_SRT
        _srt_reactor = SrtEpollReactor::createReactor();
        SrtEpollReactor::setInstance(_srt_reactor.get());
#endif
    }

private:
    bool _network_initialized = false;
    std::unique_ptr<toolkit::TimestampClock> _clock;
    MediaSource::Ptr _null_media_source;
    std::unique_ptr<toolkit::EventPollerPool> _event_pool;
    std::unique_ptr<toolkit::WorkThreadPool> _work_pool;
    HttpCookieManager::Ptr _http_cookie_manager;
#ifdef ENABLE_SRT
    std::unique_ptr<SrtEpollReactor> _srt_reactor;
#endif
};

Runtime *init(const RuntimeConfig &config) {
    if (s_runtime_active.exchange(true, std::memory_order_acq_rel)) {
        throw std::logic_error("ZLM runtime is already initialized");
    }
    try {
        auto runtime = std::make_unique<Runtime>();
        runtime->initialize(config);
        return runtime.release();
    } catch (...) {
        s_runtime_active.store(false, std::memory_order_release);
        throw;
    }
}

void shutdown(Runtime *runtime) {
    if (!runtime) {
        return;
    }
    delete runtime;
    s_runtime_active.store(false, std::memory_order_release);
}

} // namespace mediakit
