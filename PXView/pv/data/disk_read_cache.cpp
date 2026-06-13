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


#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "disk_read_cache.h"
#include "../log.h"

using namespace std;

namespace pv {
namespace data {

DiskReadCache::DiskReadCache(DiskBufferManager *manager) :
    _manager(manager),
    _current_bytes(0),
    _max_bytes(256 * 1024 * 1024)
{
}

DiskReadCache::~DiskReadCache()
{
    clear();
}

void* DiskReadCache::lookup(int channel, uint64_t block_index)
{
    lock_guard<mutex> lock(_mutex);

    for (auto it = _lru_list.begin(); it != _lru_list.end(); ++it) {
        if (it->channel == channel && it->block_index == block_index) {
            if (it != _lru_list.begin()) {
                _lru_list.splice(_lru_list.begin(), _lru_list, it);
            }
            return it->data_ptr;
        }
    }

    return nullptr;
}

void* DiskReadCache::load(int channel, uint64_t block_index)
{
    {
        lock_guard<mutex> lock(_mutex);

        for (auto it = _lru_list.begin(); it != _lru_list.end(); ++it) {
            if (it->channel == channel && it->block_index == block_index) {
                if (it != _lru_list.begin()) {
                    _lru_list.splice(_lru_list.begin(), _lru_list, it);
                }
                return it->data_ptr;
            }
        }
    }

    void *buf = malloc((size_t)LeafBlockSpace);
    if (!buf) {
        pxv_err("DiskReadCache: malloc failed for block load");
        return nullptr;
    }

    bool ok = _manager->read_block(channel, block_index, buf, LeafBlockSpace);
    if (!ok) {
        free(buf);
        return nullptr;
    }

    {
        lock_guard<mutex> lock(_mutex);

        for (auto it = _lru_list.begin(); it != _lru_list.end(); ++it) {
            if (it->channel == channel && it->block_index == block_index) {
                if (it != _lru_list.begin()) {
                    _lru_list.splice(_lru_list.begin(), _lru_list, it);
                }
                free(buf);
                return it->data_ptr;
            }
        }

        CacheEntry entry;
        entry.channel = channel;
        entry.block_index = block_index;
        entry.data_ptr = buf;

        _lru_list.push_front(entry);
        _current_bytes += LeafBlockSpace;

        while (_current_bytes > _max_bytes && _lru_list.size() > 1)
            evict();

        return buf;
    }
}

void DiskReadCache::set_max_size(uint64_t bytes)
{
    lock_guard<mutex> lock(_mutex);
    _max_bytes = bytes;

    while (_current_bytes > _max_bytes && !_lru_list.empty())
        evict();
}

void DiskReadCache::set_evict_callback(std::function<void(int, uint64_t, void*)> cb)
{
    lock_guard<mutex> lock(_mutex);
    _evict_callback = cb;
}

void DiskReadCache::clear()
{
    lock_guard<mutex> lock(_mutex);

    for (auto &entry : _lru_list) {
        if (entry.data_ptr) {
            if (_evict_callback)
                _evict_callback(entry.channel, entry.block_index, entry.data_ptr);
            free(entry.data_ptr);
        }
    }
    _lru_list.clear();
    _current_bytes = 0;
}

void DiskReadCache::evict()
{
    if (_lru_list.empty())
        return;

    CacheEntry &entry = _lru_list.back();
    if (entry.data_ptr) {
        if (_evict_callback)
            _evict_callback(entry.channel, entry.block_index, entry.data_ptr);
        free(entry.data_ptr);
    }

    _current_bytes -= LeafBlockSpace;
    _lru_list.pop_back();
}

} // namespace data
} // namespace pv
