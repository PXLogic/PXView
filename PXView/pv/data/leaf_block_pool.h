/*
 * This file is part of the PXView project.
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
 */

#ifndef PXVIEW_PV_DATA_LEAF_BLOCK_POOL_H
#define PXVIEW_PV_DATA_LEAF_BLOCK_POOL_H

#include <vector>
#include <mutex>
#include <cstdlib>
#include <cstdint>

namespace pv {
namespace data {

class LeafBlockPool {
public:
    static LeafBlockPool& instance() {
        static LeafBlockPool pool;
        return pool;
    }

    // Acquire a memory block of the given size from the pool
    void* acquire(size_t block_size) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_free_blocks.empty()) {
            void* ptr = _free_blocks.back();
            _free_blocks.pop_back();
            return ptr;
        }
        return malloc(block_size);
    }

    // Return a memory block to the pool (instead of calling free)
    void release(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (_free_blocks.size() < _max_pool_size) {
            _free_blocks.push_back(ptr);
        } else {
            free(ptr);
        }
    }

    // Set the maximum pool capacity (in number of blocks)
    void set_max_pool_size(size_t max_blocks) {
        std::lock_guard<std::mutex> lock(_mutex);
        _max_pool_size = max_blocks;
    }

    // Release all idle memory in the pool (call on app exit or memory pressure)
    void drain() {
        std::lock_guard<std::mutex> lock(_mutex);
        for (void* ptr : _free_blocks) {
            free(ptr);
        }
        _free_blocks.clear();
    }

    // Current number of idle blocks in the pool
    size_t idle_count() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _free_blocks.size();
    }

private:
    LeafBlockPool() : _max_pool_size(2048) {}
    ~LeafBlockPool() { drain(); }
    LeafBlockPool(const LeafBlockPool&) = delete;
    LeafBlockPool& operator=(const LeafBlockPool&) = delete;

    std::vector<void*> _free_blocks;
    std::mutex _mutex;
    size_t _max_pool_size;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_LEAF_BLOCK_POOL_H
