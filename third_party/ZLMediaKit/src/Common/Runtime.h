/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RUNTIME_H
#define ZLMEDIAKIT_RUNTIME_H

#include <cstddef>

namespace mediakit {

class Runtime;

struct RuntimeConfig {
    std::size_t event_poller_threads = 0;
    std::size_t work_threads = 0;
    bool enable_cpu_affinity = true;
};

// Creates and installs the SDK runtime before constructing any ZLM users.
// Logging uses mw::log, which supplies a default logger when uninitialized.
// Zero thread counts select the toolkit defaults. Throws std::logic_error if a runtime is already active;
// other initialization failures leave the SDK uninitialized.
Runtime *init(const RuntimeConfig &config = {});

// Destroys the runtime in dependency order. The caller must first destroy all
// ZLM users and invoke this once per handle, outside SDK callbacks and worker
// threads. A null handle is a no-op. A new init() is allowed after this returns.
// Keep logging available until shutdown completes to receive cleanup diagnostics.
void shutdown(Runtime *runtime);

} // namespace mediakit

#endif // ZLMEDIAKIT_RUNTIME_H
