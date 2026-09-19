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
        // P2 池化：slot 指向固定槽位池中的一块，长度由 length 给出。
        // nullptr 表示走 heap 兜底（payload 大于槽位尺寸时的罕见路径）。
        uint8_t *slot = nullptr;
        std::vector<uint8_t> heap;
        uint64_t length = 0;
        int format = 0;  // LA_SPLIT_DATA or LA_CROSS_DATA

        const uint8_t *data() const { return slot ? slot : heap.data(); }
        uint8_t *data() { return slot ? slot : heap.data(); }
        uint64_t size() const { return length; }
    };

    // P2（消除 staging 拷贝的分配/回收成本）：固定槽位池。
    //
    // 背景：实测（tests/qtest/data/test_feed_staging_copy_cost.cpp）显示
    // 原实现 `payload.data = std::vector<uint8_t>(data, data + length)` 的代价
    // 里 memcpy 只占 ~9%，**分配 + 归还 OS 占 ~91%**（4MB = 1024 页，
    // 827us/1024 ≈ 808ns/页，与内核首次触碰新映射页的缺页+清零成本吻合）。
    // 槽位复用把这 91% 消掉，同时把峰值额外常驻从 1073MB 降到 slot_bytes*max_slots。
    //
    // **调用方必须持有 _async_mutex**（不设独立互斥量，避免锁序问题）。
    class SlotPool
    {
    public:
        void configure(uint64_t slot_bytes, uint32_t max_slots);
        void reset();                       // 仅在无在飞槽位时调用

        uint8_t *acquire();                 // nullptr = 池耗尽（调用方应等待）
        void release(uint8_t *slot);

        uint64_t slot_bytes() const { return _slot_bytes; }
        uint32_t capacity() const { return static_cast<uint32_t>(_storage.size()); }
        uint32_t in_use() const { return _in_use; }
        uint32_t peak_in_use() const { return _peak_in_use; }
        uint32_t max_slots() const { return _max_slots; }
        bool configured() const { return _slot_bytes > 0 && _max_slots > 0; }

    private:
        uint64_t _slot_bytes = 0;
        uint32_t _max_slots = 0;
        std::vector<std::unique_ptr<uint8_t[]>> _storage;  // 已分配槽位（懒增长）
        std::vector<uint8_t *> _free;                      // 空闲槽位
        uint32_t _in_use = 0;
        uint32_t _peak_in_use = 0;
    };

    // P2 槽位参数：4MB 与驱动 payload 同量级；64 槽 → 上限 256MB
    // （原 ASYNC_HIGH_WATERMARK 是 1073MB，纯粹为吸收分配抖动而留的冗余）。
    static constexpr uint64_t P2_SLOT_BYTES = 4ULL * 1024 * 1024;
    static constexpr uint32_t P2_MAX_SLOTS = 64;
    static constexpr uint64_t ASYNC_HIGH_WATERMARK = P2_SLOT_BYTES * P2_MAX_SLOTS;
    static constexpr uint64_t ASYNC_LOW_WATERMARK  = ASYNC_HIGH_WATERMARK / 4;

    std::queue<AsyncPayload> _async_queue;
    std::mutex _async_mutex;
    std::condition_variable _async_cv;
    std::condition_variable _async_drain_cv;  // feed 等待槽位归还 / 队列降到低水位
    std::thread _async_thread;
    std::atomic<bool> _async_running;

    SlotPool _slot_pool;  // 受 _async_mutex 保护

    std::atomic<uint64_t> _async_bytes_written;
    std::atomic<double>   _async_write_speed_mbps;
    std::atomic<size_t>   _async_queue_depth;
    std::atomic<uint64_t> _async_queue_bytes_size;
};

#endif  // PXVIEW_PV_DATA_LOGICSNAPSHOT_DISKCACHE_WRITER_H
