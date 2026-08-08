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

#ifndef PXVIEW_PV_DATA_LOGICSNAPSHOT_DISKCACHE_WRITER_H
#define PXVIEW_PV_DATA_LOGICSNAPSHOT_DISKCACHE_WRITER_H

#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <atomic>
#include <cstdint>
#include <vector>
#include "pv/data/cache/disk_cache_config.h"

// Forward declarations — do NOT include logicsnapshot.h here (circular).
namespace pv {
namespace data {
class LogicSnapshot;
}  // namespace data
}  // namespace pv

// Extracted disk-cache/async-writer subsystem (cluster D) from LogicSnapshot.
// Owns the async writer thread, its queue/mutex/CV, the per-slot mmap written
// bitmap, and the disk-cache config. LogicSnapshot holds this via unique_ptr
// and forwards the public methods; cluster A (chunk tree) and the mmap
// allocator itself stay in LogicSnapshot and are accessed through the
// back-pointer (friend).
class LogicSnapshotDiskCacheWriter
{
public:
    explicit LogicSnapshotDiskCacheWriter(pv::data::LogicSnapshot *owner);
    ~LogicSnapshotDiskCacheWriter();

    // ---- Configuration & lifecycle ----
    void set_disk_cache_config(const pv::data::DiskCacheConfig &config);
    bool is_disk_cache_active() const;
    void start();           // spawns _async_thread, resets bytes-written counter
    void drain_and_join();  // stop+join+clear-queue (called by clear/free_data/dtor)

    // ---- Stats ----
    double get_disk_write_speed_mbps() const;
    size_t get_disk_write_queue_depth() const;
    uint64_t get_disk_total_blocks_written() const;
    uint64_t get_async_queue_bytes() const;

    // ---- Hot loading (currently a no-op stub) ----
    void ensure_all_blocks_hot();

    // ---- Enqueue (called by LogicSnapshot::append_payload) ----
    // `format` follows libsigrok's LA_DATA_FORMAT enum:
    //   LA_SPLIT_DATA (0): sample-interleaved (upstream sigrok drivers)
    //   LA_CROSS_DATA (1): channel-block raw (PXLogic/DSLogic fork drivers)
    // The worker thread dispatches to append_payload_impl (SPLIT) or
    // append_cross_payload (CROSS) accordingly.
    void enqueue(const uint8_t *data, uint64_t length, int format);

    // ---- mmap slot state ----
    bool is_mmap_slot_fresh(uint16_t channel, uint64_t global_block_seq) const;
    void mark_mmap_slot_written(uint16_t channel, uint64_t global_block_seq);
    void clear_mmap_slot_written(uint16_t channel, uint64_t global_block_seq);
    void clear_mmap_slot_by_abs(uint64_t abs_slot);

    // ---- mmap slot bulk ops (called by first_payload / free_data) ----
    void setup_mmap_slots(size_t count);
    void clear_all_mmap_slots();

    // ---- capture_ended drain (poll + timeout + force-stop) ----
    void drain_queue_for_capture_end();

    // ---- Config access (read-only, for first_payload) ----
    const pv::data::DiskCacheConfig &disk_cache_config() const { return _disk_cache_config; }

private:
    void async_write_worker();  // thread function

    pv::data::LogicSnapshot *_owner;

    // Disk-cache configuration (set externally before first_payload).
    pv::data::DiskCacheConfig _disk_cache_config;

    // per-slot bitmap: marks whether an mmap slot currently holds committed
    // data. Index = abs_slot = channel * _max_blocks_per_channel +
    // (global_block_seq % _max_blocks_per_channel). abs_slot maps directly to
    // the physical slot in the mmap region (matches MmapAllocator addressing).
    // _max_blocks_per_channel itself stays on LogicSnapshot (cluster A).
    std::vector<bool> _mmap_slot_written;
    std::atomic<bool> _async_busy{false}; // true while writer is processing a dequeued payload

    struct AsyncPayload {
        std::vector<uint8_t> data;
        int format;  // LA_SPLIT_DATA or LA_CROSS_DATA
    };

    // Backpressure watermarks for the async write queue (hysteresis):
    // feed thread blocks above HIGH, unblocks below LOW.
    static constexpr uint64_t ASYNC_HIGH_WATERMARK = 1024ULL * 1024 * 1024;
    static constexpr uint64_t ASYNC_LOW_WATERMARK  = 256ULL * 1024 * 1024;

    std::queue<AsyncPayload> _async_queue;
    std::mutex _async_mutex;
    std::condition_variable _async_cv;
    std::condition_variable _async_drain_cv;  // feed waits on this when queue exceeds high watermark
    std::thread _async_thread;
    std::atomic<bool> _async_running;

    std::atomic<uint64_t> _async_bytes_written;
    std::atomic<double>   _async_write_speed_mbps;
    std::atomic<size_t>   _async_queue_depth;
    std::atomic<uint64_t> _async_queue_bytes_size;
};

#endif  // PXVIEW_PV_DATA_LOGICSNAPSHOT_DISKCACHE_WRITER_H
