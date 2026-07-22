/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "HttpRequester.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

void HttpRequester::onResponseHeader(const string &status, const HttpHeader &headers) {
    _res_body.clear();
}

void HttpRequester::onResponseBody(const char *buf, size_t size) {
    _res_body.append(buf, size);
}

void HttpRequester::onResponseCompleted(const SockException &ex) {
    if (ex && _retry++ < _max_retry) {
        std::weak_ptr<HttpRequester> weak_self = std::static_pointer_cast<HttpRequester>(shared_from_this());
        getPoller()->doDelayTask(_retry_delay, [weak_self]() {
            if (auto self = weak_self.lock()) {
                InfoL << "resend request " << self->getUrl() << " with retry " << self->getRetry();
                self->sendRequest(self->getUrl());
            }
            return 0;
        });
        return;
    }
    const_cast<Parser &>(response()).setContent(std::move(_res_body));
    if (_on_result) {
        _on_result(ex, response());
        _on_result = nullptr;
    }
}

void HttpRequester::setRetry(size_t count, size_t delay) {
    InfoL << "setRetry max=" << count << ", delay=" << delay;
    _max_retry = count;
    _retry_delay = delay;
}

void HttpRequester::startRequester(const string &url, const HttpRequesterResult &on_result, float timeout_sec) {
    _on_result = on_result;
    _retry = 0;
    setCompleteTimeout(timeout_sec * 1000);
    sendRequest(url);
}

void HttpRequester::clear() {
    HttpClientImp::clear();
    _res_body.clear();
    _on_result = nullptr;
}

void HttpRequester::setOnResult(const HttpRequesterResult &on_result) {
    _on_result = on_result;
}

} // namespace mediakit
