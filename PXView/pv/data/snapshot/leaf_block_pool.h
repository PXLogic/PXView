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
#include <cassert>

namespace pv {
namespace data {

/// LeafBlockPool — a fixed-size memory pool for LogicSnapshot leaf blocks.
///
/// MS-1 fix: The pool is now strictly single-size. The first acquire() call
/// fixes the block size for the pool's lifetime; all subsequent calls must
/// pass the same size. This prevents the heap buffer overflow that would
/// occur if a caller acquired a block with a different (larger) size than
/// the original — the pool would return a too-small buffer.
///
/// The pool stores `_block_size` so that release() can optionally validate
/// that the returned block matches. Acquire from the free-list returns a
/// block that is guaranteed to be at least `_block_size` bytes.
class LeafBlockPool {
public:
    static LeafBlockPool& instance() {
        static LeafBlockPool pool;
        return pool;
    }

    /// Acquire a memory block of exactly `block_size` bytes.
    /// The first call fixes the pool's block size; subsequent calls with a
    /// different size will trigger an assert (debug) and fall through to
    /// malloc (release) to avoid returning an undersized block.
    void* acquire(size_t block_size) {
        std::lock_guard<std::mutex> lock(_mutex);

        // MS-1 fix: validate block_size against the pool's fixed size.
        if (_block_size == 0) {
            // First call — fix the pool's block size.
            _block_size = block_size;
        } else if (_block_size != block_size) {
            // Size mismatch — the pool was initialized with a different size.
            // This is a programming error. In debug builds, assert; in
            // release, fall through to malloc so the caller gets a valid
            // (correctly-sized) block instead of a recycled too-small one.
            assert(false && "LeafBlockPool::acquire: block_size mismatch — "
                   "pool was initialized with a different size");
            return malloc(block_size);
        }

        if (!_free_blocks.empty()) {
            void* ptr = _free_blocks.back();
            _free_blocks.pop_back();
            return ptr;
        }
        return malloc(block_size);
    }

    /// Return a memory block to the pool (instead of calling free).
    /// The block must have been acquired with the same size as the pool's
    /// fixed block_size.
    void release(void* ptr) {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (_free_blocks.size() < _max_pool_size) {
            _free_blocks.push_back(ptr);
        } else {
            free(ptr);
        }
    }

    /// Set the maximum pool capacity (in number of blocks).
    void set_max_pool_size(size_t max_blocks) {
        std::lock_guard<std::mutex> lock(_mutex);
        _max_pool_size = max_blocks;
    }

    /// Release all idle memory in the pool (call on app exit or memory pressure).
    void drain() {
        std::lock_guard<std::mutex> lock(_mutex);
        for (void* ptr : _free_blocks) {
            free(ptr);
        }
        _free_blocks.clear();
    }

    /// Current number of idle blocks in the pool.
    size_t idle_count() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _free_blocks.size();
    }

    /// The fixed block size for this pool (0 until the first acquire).
    size_t block_size() const { return _block_size; }

private:
    LeafBlockPool() : _max_pool_size(2048), _block_size(0) {}
    ~LeafBlockPool() { drain(); }
    LeafBlockPool(const LeafBlockPool&) = delete;
    LeafBlockPool& operator=(const LeafBlockPool&) = delete;

    std::vector<void*> _free_blocks;
    std::mutex _mutex;
    size_t _max_pool_size;
    // MS-1 fix: fixed block size, set on first acquire(). All subsequent
    // acquire() calls must use the same size.
    size_t _block_size;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_LEAF_BLOCK_POOL_H
