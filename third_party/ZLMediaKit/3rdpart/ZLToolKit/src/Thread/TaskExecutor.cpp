/*
 * Copyright (c) 2016 The ZLToolKit project authors. All Rights Reserved.
 *
 * This file is part of ZLToolKit(https://github.com/ZLMediaKit/ZLToolKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <atomic>
#include "TaskExecutor.h"
#include "Poller/EventPoller.h"
#include "Util/onceToken.h"
#include "Util/TimeTicker.h"

using namespace std;

namespace toolkit {

ThreadLoadCounter::ThreadLoadCounter(uint64_t max_size, uint64_t max_usec) {
    _last_sleep_time = _last_wake_time = getCurrentMicrosecond();
    _max_size = max_size;
    _max_usec = max_usec;
}

void ThreadLoadCounter::startSleep() {
    lock_guard<mutex> lck(_mtx);
    _sleeping = true;
    auto current_time = getCurrentMicrosecond();
    auto run_time = current_time - _last_wake_time;
    _last_sleep_time = current_time;
    _time_list.emplace_back(run_time, false);
    if (_time_list.size() > _max_size) {
        _time_list.pop_front();
    }
}

void ThreadLoadCounter::sleepWakeUp() {
    lock_guard<mutex> lck(_mtx);
    _sleeping = false;
    auto current_time = getCurrentMicrosecond();
    auto sleep_time = current_time - _last_sleep_time;
    _last_wake_time = current_time;
    _time_list.emplace_back(sleep_time, true);
    if (_time_list.size() > _max_size) {
        _time_list.pop_front();
    }
}

int ThreadLoadCounter::load() {
    lock_guard<mutex> lck(_mtx);
    uint64_t totalSleepTime = 0;
    uint64_t totalRunTime = 0;
    _time_list.for_each([&](const TimeRecord &rcd) {
        if (rcd._sleep) {
            totalSleepTime += rcd._time;
        } else {
            totalRunTime += rcd._time;
        }
    });

    if (_sleeping) {
        totalSleepTime += (getCurrentMicrosecond() - _last_sleep_time);
    } else {
        totalRunTime += (getCurrentMicrosecond() - _last_wake_time);
    }

    uint64_t totalTime = totalRunTime + totalSleepTime;
    while ((_time_list.size() != 0) && (totalTime > _max_usec || _time_list.size() > _max_size)) {
        TimeRecord &rcd = _time_list.front();
        if (rcd._sleep) {
            totalSleepTime -= rcd._time;
        } else {
            totalRunTime -= rcd._time;
        }
        totalTime -= rcd._time;
        _time_list.pop_front();
    }
    if (totalTime == 0) {
        return 0;
    }
    return (int) (totalRunTime * 100 / totalTime);
}

////////////////////////////////////////////////////////////////////////////

Task::Ptr TaskExecutorInterface::async_first(TaskIn task, bool may_sync) {
    return async(std::move(task), may_sync);
}

void TaskExecutorInterface::sync(const TaskIn &task) {
    semaphore sem;
    auto ret = async([&]() {
        onceToken token(nullptr, [&]() {
            //通过RAII原理防止抛异常导致不执行这句代码  [AUTO-TRANSLATED:206bd80e]
            //Prevent this code from not being executed due to an exception being thrown through RAII principle
            sem.post();
        });
        task();
    });
    if (ret && *ret) {
        sem.wait();
    }
}

void TaskExecutorInterface::sync_first(const TaskIn &task) {
    semaphore sem;
    auto ret = async_first([&]() {
        onceToken token(nullptr, [&]() {
            //通过RAII原理防止抛异常导致不执行这句代码  [AUTO-TRANSLATED:206bd80e]
            //Prevent this code from not being executed due to an exception being thrown through RAII principle
            sem.post();
        });
        task();
    });
    if (ret && *ret) {
        sem.wait();
    }
}

//////////////////////////////////////////////////////////////////

TaskExecutor::TaskExecutor(uint64_t max_size, uint64_t max_usec) : ThreadLoadCounter(max_size, max_usec) {}

//////////////////////////////////////////////////////////////////

TaskExecutor::Ptr TaskExecutorGetterImp::getExecutor() {
    lock_guard<mutex> lock(_executor_mutex);
    if (_threads.empty()) {
        throw logic_error("Executor pool is empty");
    }
    auto thread_pos = _thread_pos;
    if (thread_pos >= _threads.size()) {
        thread_pos = 0;
    }

    TaskExecutor::Ptr executor_min_load = _threads[thread_pos];
    auto min_load = executor_min_load->load();

    for (size_t i = 0; i < _threads.size(); ++i) {
        ++thread_pos;
        if (thread_pos >= _threads.size()) {
            thread_pos = 0;
        }

        auto th = _threads[thread_pos];
        auto load = th->load();

        if (load < min_load) {
            min_load = load;
            executor_min_load = th;
        }
        if (min_load == 0) {
            break;
        }
    }
    _thread_pos = thread_pos;
    _issued_executors.emplace(executor_min_load.get());
    return executor_min_load;
}

vector<int> TaskExecutorGetterImp::getExecutorLoad() {
    auto threads = snapshotExecutors();
    vector<int> vec(threads.size());
    int i = 0;
    for (auto &executor : threads) {
        vec[i++] = executor->load();
    }
    return vec;
}

void TaskExecutorGetterImp::getExecutorDelay(const function<void(const vector<int> &)> &callback) {
    auto threads = snapshotExecutors();
    std::shared_ptr<vector<int> > delay_vec = std::make_shared<vector<int>>(threads.size());
    shared_ptr<void> finished(nullptr, [callback, delay_vec](void *) {
        //此析构回调触发时，说明已执行完毕所有async任务  [AUTO-TRANSLATED:8adf8212]
        //When this destructor callback is triggered, it means all async tasks have been executed
        callback((*delay_vec));
    });
    int index = 0;
    for (auto &th : threads) {
        std::shared_ptr<Ticker> delay_ticker = std::make_shared<Ticker>();
        th->async([finished, delay_vec, index, delay_ticker]() {
            (*delay_vec)[index] = (int) delay_ticker->elapsedTime();
        }, false);
        ++index;
    }
}

using onGetExecutor = std::function<void(const TaskExecutor::Ptr &)>;
class onGetExecutorCB {
public:
    onGetExecutorCB(onGetExecutor cb): _cb(std::move(cb)) {}

    void operator()(const TaskExecutor::Ptr &exe) {
        bool expected = false;
        if (_done.compare_exchange_strong(expected, true)) {
            _cb(exe);
            _cb = nullptr;
        }
    }

private:
    std::atomic<bool> _done { false };
    std::function<void(const TaskExecutor::Ptr &)> _cb;
};

void TaskExecutorGetterImp::getExecutor(const onGetExecutor &cb) {
    auto callback = std::make_shared<onGetExecutorCB>(cb);
    auto threads = snapshotExecutors();
    for (auto &th : threads) {
        th->async([th, callback]() mutable { (*callback)(th); }, false);
    }
}

void TaskExecutorGetterImp::for_each(const function<void(const TaskExecutor::Ptr &)> &cb) {
    auto threads = snapshotExecutors();
    for (auto &th : threads) {
        cb(th);
    }
}

size_t TaskExecutorGetterImp::getExecutorSize() const {
    lock_guard<mutex> lock(_executor_mutex);
    return _threads.size();
}

TaskExecutor::Ptr TaskExecutorGetterImp::getFirstExecutor() {
    lock_guard<mutex> lock(_executor_mutex);
    if (_threads.empty()) {
        throw logic_error("Executor pool is empty");
    }
    _issued_executors.emplace(_threads.front().get());
    return _threads.front();
}

TaskExecutor::Ptr TaskExecutorGetterImp::getSharedExecutor(const TaskExecutor::Ptr &executor) {
    lock_guard<mutex> lock(_executor_mutex);
    auto it = find(_threads.begin(), _threads.end(), executor);
    if (it == _threads.end()) {
        return nullptr;
    }
    _issued_executors.emplace(executor.get());
    return *it;
}

vector<TaskExecutor::Ptr> TaskExecutorGetterImp::snapshotExecutors() {
    lock_guard<mutex> lock(_executor_mutex);
    for (const auto &executor : _threads) {
        _issued_executors.emplace(executor.get());
    }
    return _threads;
}

TaskExecutor::Ptr TaskExecutorGetterImp::extractUnusedExecutor() {
    lock_guard<mutex> lock(_executor_mutex);
    if (_threads.size() <= 1) {
        return nullptr;
    }
    for (auto it = _threads.begin(); it != _threads.end(); ++it) {
        if (_issued_executors.count(it->get()) == 0) {
            auto executor = std::move(*it);
            _threads.erase(it);
            _thread_pos = 0;
            return executor;
        }
    }
    return nullptr;
}

TaskExecutor::Ptr TaskExecutorGetterImp::createPoller(const string &name, int priority, bool register_thread, bool enable_cpu_affinity, size_t cpu_index) {
    EventPoller::Ptr poller(new EventPoller(name), [](EventPoller *poller) {
        // runLoop borrows this. A last reference released by its own callback
        // must be reclaimed elsewhere, after shutdown has joined the loop.
        if (poller->isCurrentThread()) {
            thread([poller]() { delete poller; }).detach();
        } else {
            delete poller;
        }
    });
    poller->runLoop(false, register_thread);
    poller->async([cpu_index, name, priority, enable_cpu_affinity]() {
        ThreadPool::setPriority((ThreadPool::Priority)priority);
        setThreadName(name.data());
        if (enable_cpu_affinity) {
            setThreadAffinity(cpu_index);
        }
    });
    return poller;
}

size_t TaskExecutorGetterImp::addPoller(const string &name, size_t size, int priority, bool register_thread, bool enable_cpu_affinity) {
    auto cpus = max<size_t>(1, thread::hardware_concurrency());
    size = size > 0 ? size : cpus;
    lock_guard<mutex> lock(_executor_mutex);
    for (size_t i = 0; i < size; ++i) {
        _threads.emplace_back(createPoller(name + " " + to_string(i), priority, register_thread, enable_cpu_affinity, i % cpus));
    }
    return size;
}

}//toolkit
