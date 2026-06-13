/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#include <string.h>
#include <stdlib.h>
#include <algorithm>

#include "disk_write_thread.h"
#include "../log.h"

using namespace std;
using namespace std::chrono;

namespace pv {
namespace data {

DiskWriteThread::DiskWriteThread(DiskBufferManager *manager) :
    _manager(manager),
    _running(false),
    _stopping(false),
    _speed_bytes(0),
    _speed_mbps(0.0),
    _disk_full(false),
    _cache_size_bytes(0)
{
    pxv_info("DiskWriteThread: Created new write thread, manager=%p", manager);
}

DiskWriteThread::~DiskWriteThread()
{
    stop();
}

bool DiskWriteThread::start()
{
    lock_guard<mutex> lock(_mutex);

    if (_running)
        return true;

    _stopping = false;
    _running = true;
    _speed_bytes = 0;
    _speed_mbps = 0.0;
    _speed_start = steady_clock::now();
    _speed_samples.clear();
    _disk_full = false;

    while (!_queue.empty()) {
        WriteTask &t = _queue.front();
        if (t.on_complete)
            t.on_complete();
        _queue.pop();
    }

    _thread = thread(&DiskWriteThread::thread_func, this);
    pxv_info("DiskWriteThread: started");
    return true;
}

void DiskWriteThread::stop()
{
    {
        lock_guard<mutex> lock(_mutex);
        if (!_running)
            return;
        _stopping = true;
        _cv.notify_one();
    }

    if (_thread.joinable())
        _thread.join();

    {
        lock_guard<mutex> lock(_mutex);
        while (!_queue.empty()) {
            WriteTask &t = _queue.front();
            if (t.on_complete)
                t.on_complete();
            _queue.pop();
        }
        _running = false;
    }

    pxv_info("DiskWriteThread: stopped");
}

void DiskWriteThread::flush()
{
    unique_lock<mutex> lock(_mutex);
    _cv.wait(lock, [this]() { return _queue.empty() || _stopping; });
}

void DiskWriteThread::submit(WriteTask task)
{
    {
        unique_lock<mutex> lock(_mutex);
        if (!_running)
            return;

        _cv_full.wait(lock, [this]() { return _queue.size() < 50 || !_running; });

        if (!_running)
            return;

        // No malloc/memcpy here. We take ownership of the task and will call on_complete.
        _queue.push(task);
        static int _last_log_depth = -1;
        if ((_queue.size() % 10 == 0 || task.block_index < 5) && _queue.size() != (size_t)_last_log_depth) {
            pxv_info("DiskWriteThread: submit task ch=%d blk=%llu (queue depth %zu)", task.channel, (unsigned long long)task.block_index, _queue.size());
            _last_log_depth = _queue.size();
        }
    }
    _cv.notify_one();
}

size_t DiskWriteThread::queue_depth()
{
    lock_guard<mutex> lock(_mutex);
    return _queue.size();
}

double DiskWriteThread::write_speed_mbps()
{
    lock_guard<mutex> lock(_mutex);
    return _speed_mbps;
}

void DiskWriteThread::thread_func()
{
    pxv_info("DiskWriteThread: thread started");

    while (true) {
        WriteTask task;
        {
            unique_lock<mutex> lock(_mutex);
            _cv.wait(lock, [this]() { return !_queue.empty() || _stopping; });

            if (_stopping && _queue.empty())
                break;

            if (_queue.empty())
                continue;

            task = _queue.front();
            _queue.pop();
            _cv_full.notify_one();
        }

        if (_manager && task.data_ptr) {
            bool do_write = true;
            bool warn_disk_full = false;

            {
                lock_guard<mutex> lock(_mutex);
                if (_disk_full)
                    do_write = false;
            }

            if (do_write && _cache_size_bytes > 0 &&
                !_manager->check_disk_space(_cache_size_bytes / 10)) {
                lock_guard<mutex> lock(_mutex);
                _disk_full = true;
                do_write = false;
                warn_disk_full = true;
                pxv_err("DiskWriteThread: disk space low, stopping writes");
            }

            if (warn_disk_full && on_warning)
                on_warning("Disk space is low, stopping disk cache writes");

            if (do_write) {
                bool ok = _manager->write_block(task.channel, task.block_index,
                    task.data_ptr, task.size);

                if (ok) {
                    static int _last_written = -1;
                    if ((task.block_index % 10 == 0 || task.block_index < 5) && task.block_index != (uint64_t)_last_written) {
                        pxv_info("DiskWriteThread: successfully wrote ch=%d blk=%llu", task.channel, (unsigned long long)task.block_index);
                        _last_written = task.block_index;
                    }
                    auto now = steady_clock::now();
                lock_guard<mutex> lock(_mutex);
                _speed_bytes += task.size;
                _speed_samples.push_back({now, task.size});

                while (_speed_samples.size() > 2) {
                    auto front_time = _speed_samples.front().first;
                    auto dur = duration_cast<milliseconds>(now - front_time).count();
                    if (dur > 3000)
                        _speed_samples.erase(_speed_samples.begin());
                    else
                        break;
                }

                if (_speed_samples.size() >= 2) {
                    auto &first = _speed_samples.front();
                    auto &last = _speed_samples.back();
                    double secs = duration<double>(last.first - first.first).count();
                    if (secs > 0.001) {
                        uint64_t total = 0;
                        for (auto &s : _speed_samples)
                            total += s.second;
                        _speed_mbps = (total / (1024.0 * 1024.0)) / secs;
                    }
                }
            } else {
                pxv_err("DiskWriteThread: write failed ch=%d blk=%llu",
                    task.channel, (unsigned long long)task.block_index);
                if (on_warning) {
                    on_warning("Disk write failed for channel " +
                        to_string(task.channel) + " block " +
                        to_string(task.block_index));
                }
            }
            }

            if (task.on_complete)
                task.on_complete();
        }

        {
            lock_guard<mutex> lock(_mutex);
            if (_queue.empty())
                _cv.notify_one();
        }
    }

    pxv_info("DiskWriteThread: thread exiting");
}

bool DiskWriteThread::is_disk_full()
{
    lock_guard<mutex> lock(_mutex);
    return _disk_full;
}

void DiskWriteThread::set_cache_size(uint64_t bytes)
{
    lock_guard<mutex> lock(_mutex);
    _cache_size_bytes = bytes;
}

} // namespace data
} // namespace pv
