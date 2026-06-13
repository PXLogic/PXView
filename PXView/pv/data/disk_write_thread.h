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


#ifndef PXVIEW_PV_DATA_DISK_WRITE_THREAD_H
#define PXVIEW_PV_DATA_DISK_WRITE_THREAD_H

#include "disk_buffer_manager.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <string>
#include <cstdint>
#include <chrono>
#include <vector>

namespace pv {
namespace data {

struct WriteTask
{
    int channel;
    uint64_t block_index;
    void *data_ptr;
    uint64_t size;
    std::function<void()> on_complete;
};

class DiskWriteThread
{
public:
    DiskWriteThread(DiskBufferManager *manager);
    ~DiskWriteThread();

    bool start();
    void stop();
    void flush();

    void submit(WriteTask task);

    size_t queue_depth();
    double write_speed_mbps();
    bool is_disk_full();
    void set_cache_size(uint64_t bytes);

    std::function<void(const std::string&)> on_warning;

private:
    void thread_func();

private:
    DiskBufferManager *_manager;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::condition_variable _cv_full;
    std::queue<WriteTask> _queue;
    bool _running;
    bool _stopping;

    std::chrono::steady_clock::time_point _speed_start;
    uint64_t _speed_bytes;
    double _speed_mbps;

    std::vector<std::pair<std::chrono::steady_clock::time_point, uint64_t>> _speed_samples;
    bool _disk_full;
    uint64_t _cache_size_bytes;
};

} // namespace data
} // namespace pv

#endif
