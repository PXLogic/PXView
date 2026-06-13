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


#ifndef PXVIEW_PV_DATA_DISK_READ_CACHE_H
#define PXVIEW_PV_DATA_DISK_READ_CACHE_H

#include "disk_buffer_manager.h"
#include <list>
#include <mutex>
#include <cstdint>
#include <functional>

namespace pv {
namespace data {

struct CacheEntry
{
    int channel;
    uint64_t block_index;
    void *data_ptr;
};

class DiskReadCache
{
public:
    DiskReadCache(DiskBufferManager *manager);
    ~DiskReadCache();

    void* lookup(int channel, uint64_t block_index);
    void* load(int channel, uint64_t block_index);

    void set_max_size(uint64_t bytes);
    uint64_t max_size() const { return _max_bytes; }

    void clear();

    void set_evict_callback(std::function<void(int channel, uint64_t block_index, void *ptr)> cb);

private:
    void evict();

private:
    DiskBufferManager *_manager;
    std::list<CacheEntry> _lru_list;
    uint64_t _current_bytes;
    uint64_t _max_bytes;
    std::mutex _mutex;
    std::function<void(int channel, uint64_t block_index, void *ptr)> _evict_callback;
};

} // namespace data
} // namespace pv

#endif
