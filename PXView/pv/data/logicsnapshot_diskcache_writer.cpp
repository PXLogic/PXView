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

// One-way dependency: this TU may include logicsnapshot.h; logicsnapshot.h
// must NOT include this header (would be circular). LogicSnapshot holds the
// writer via unique_ptr<forward-declared type> and forwarders in the .cpp.

#include <algorithm>
#include <chrono>
#include <thread>

#include "../pxvdef.h"
#include "../log.h"
#include "logicsnapshot.h"
#include "logicsnapshot_diskcache_writer.h"

using namespace std;

// ----------------------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------------------

LogicSnapshotDiskCacheWriter::LogicSnapshotDiskCacheWriter(pv::data::LogicSnapshot *owner)
    : _owner(owner)
    , _async_running(false)
    , _async_bytes_written(0)
    , _async_write_speed_mbps(0.0)
    , _async_queue_depth(0)
    , _async_queue_bytes_size(0)
{
}

LogicSnapshotDiskCacheWriter::~LogicSnapshotDiskCacheWriter()
{
    drain_and_join();
}

// ----------------------------------------------------------------------------
// Configuration & lifecycle
// ----------------------------------------------------------------------------

void LogicSnapshotDiskCacheWriter::set_disk_cache_config(const pv::data::DiskCacheConfig &config)
{
    pxv_info("LogicSnapshotDiskCacheWriter::set_disk_cache_config: enabled=%d, path=%s",
             config.enabled, config.cache_path.c_str());
    _disk_cache_config = config;
}

bool LogicSnapshotDiskCacheWriter::is_disk_cache_active() const
{
    // _mmap_alloc stays on LogicSnapshot (cluster A) — access via back-pointer.
    return _owner->_mmap_alloc && _disk_cache_config.enabled;
}

void LogicSnapshotDiskCacheWriter::start()
{
    if (!_async_running) {
        _async_running = true;
        _async_bytes_written = 0;
        _async_thread = std::thread(&LogicSnapshotDiskCacheWriter::async_write_worker, this);
    }
}

void LogicSnapshotDiskCacheWriter::drain_and_join()
{
    // CRITICAL (Task 1): stop the async writer and drain its queue BEFORE
    // free_data() resets the mmap allocator. Otherwise the async worker could
    // still be calling allocate_block()/_mmap_alloc->get_block_data() against
    // an allocator we are about to destroy, causing a use-after-free / crash.
    if (_async_running) {
        _async_running = false;
        _async_cv.notify_all();        // wake worker so it observes !_async_running
        _async_drain_cv.notify_all();  // unblock any feed thread waiting on backpressure
        if (_async_thread.joinable()) {
            _async_thread.join();
        }
    }
    {
        std::lock_guard<std::mutex> q_lock(_async_mutex);
        std::queue<AsyncPayload> empty;
        _async_queue.swap(empty);
        _async_queue_depth = 0;
        _async_queue_bytes_size = 0;
        _async_drain_cv.notify_all();  // final release for any straggler feed thread
    }
}

// ----------------------------------------------------------------------------
// Stats
// ----------------------------------------------------------------------------

double LogicSnapshotDiskCacheWriter::get_disk_write_speed_mbps() const
{
    return _async_write_speed_mbps.load();
}

size_t LogicSnapshotDiskCacheWriter::get_disk_write_queue_depth() const
{
    return _async_queue_depth.load();
}

uint64_t LogicSnapshotDiskCacheWriter::get_disk_total_blocks_written() const
{
    // LeafBlockSpace is a private static const on LogicSnapshot — friend access.
    return _async_bytes_written.load() / pv::data::LogicSnapshot::LeafBlockSpace;
}

uint64_t LogicSnapshotDiskCacheWriter::get_async_queue_bytes() const
{
    return _async_queue_bytes_size.load();
}

// ----------------------------------------------------------------------------
// Hot loading (stub)
// ----------------------------------------------------------------------------

void LogicSnapshotDiskCacheWriter::ensure_all_blocks_hot()
{
    // No-op stub (preserved from LogicSnapshot::ensure_all_blocks_hot).
}

// ----------------------------------------------------------------------------
// Enqueue (called by LogicSnapshot::append_payload)
// ----------------------------------------------------------------------------

void LogicSnapshotDiskCacheWriter::enqueue(const uint8_t *data, uint64_t length, int format)
{
    AsyncPayload payload;
    payload.data = std::vector<uint8_t>(data, data + length);
    payload.format = format;
    size_t v_size = payload.data.size();

    {
        std::unique_lock<std::mutex> lock(_async_mutex);
        // Backpressure (Task 3): if the async write queue is larger than the high
        // watermark, block the feed thread here until the async worker drains it
        // below the low watermark. This backs pressure up to the device driver
        // instead of letting the queue grow unbounded and OOM under fast capture +
        // slow disk. The `|| !_async_running` clause guarantees we never block
        // forever when the async writer is being shut down (stop paths notify_all
        // on _async_drain_cv). Hysteresis: block above HIGH, release below LOW.
        if (_async_queue_bytes_size.load() > ASYNC_HIGH_WATERMARK) {
            _async_drain_cv.wait(lock, [this] {
                return _async_queue_bytes_size.load() <= ASYNC_LOW_WATERMARK
                    || !_async_running.load();
            });
            // If the writer was stopped while we were blocked, drop this payload:
            // the stop path is about to (or already has) cleared the queue, so any
            // push here would become an orphan entry referencing freed mmap state.
            if (!_async_running.load()) {
                pxv_warn("append_payload: async writer stopped during backpressure wait, "
                         "dropping %llu bytes", (unsigned long long)v_size);
                return;
            }
        }
        _async_queue.push(std::move(payload));
        _async_queue_depth = _async_queue.size();
        _async_queue_bytes_size += v_size;
    }
    _async_cv.notify_one();
}

// ----------------------------------------------------------------------------
// mmap slot state
// ----------------------------------------------------------------------------

bool LogicSnapshotDiskCacheWriter::is_mmap_slot_fresh(uint16_t channel, uint64_t global_block_seq) const
{
    if (_mmap_slot_written.empty()) return false;
    // _max_blocks_per_channel stays on LogicSnapshot (cluster A) — friend access.
    uint64_t abs_slot = (uint64_t)channel * _owner->_max_blocks_per_channel
                      + (global_block_seq % _owner->_max_blocks_per_channel);
    if (abs_slot >= _mmap_slot_written.size()) return false;
    return !_mmap_slot_written[abs_slot];
}

void LogicSnapshotDiskCacheWriter::mark_mmap_slot_written(uint16_t channel, uint64_t global_block_seq)
{
    if (_mmap_slot_written.empty()) return;
    uint64_t abs_slot = (uint64_t)channel * _owner->_max_blocks_per_channel
                      + (global_block_seq % _owner->_max_blocks_per_channel);
    if (abs_slot >= _mmap_slot_written.size()) return;
    _mmap_slot_written[abs_slot] = true;
}

void LogicSnapshotDiskCacheWriter::clear_mmap_slot_written(uint16_t channel, uint64_t global_block_seq)
{
    if (_mmap_slot_written.empty()) return;
    uint64_t abs_slot = (uint64_t)channel * _owner->_max_blocks_per_channel
                      + (global_block_seq % _owner->_max_blocks_per_channel);
    if (abs_slot >= _mmap_slot_written.size()) return;
    _mmap_slot_written[abs_slot] = false;
}

void LogicSnapshotDiskCacheWriter::clear_mmap_slot_by_abs(uint64_t abs_slot)
{
    if (_mmap_slot_written.empty()) return;
    if (abs_slot >= _mmap_slot_written.size()) return;
    _mmap_slot_written[abs_slot] = false;
}

void LogicSnapshotDiskCacheWriter::setup_mmap_slots(size_t count)
{
    _mmap_slot_written.assign(count, false);
}

void LogicSnapshotDiskCacheWriter::clear_all_mmap_slots()
{
    _mmap_slot_written.clear();
}

// ----------------------------------------------------------------------------
// capture_ended drain (poll + timeout + force-stop)
// ----------------------------------------------------------------------------

void LogicSnapshotDiskCacheWriter::drain_queue_for_capture_end()
{
    // CRITICAL FIX: Drain the async write queue BEFORE acquiring _mutex.
    // Without this, _ring_sample_count may be stale (the async writer hasn't
    // finished writing all pending data), and the memset below would zero out
    // valid data that was still waiting in the queue.
    // We must NOT hold _mutex while waiting, because the async worker needs
    // _mutex to call append_payload_impl().
    int drain_loops = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(_async_mutex);
            if (_async_queue.empty() && !_async_busy.load()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        drain_loops++;
        if (drain_loops > 10000) {  // 10s safety timeout
            pxv_err("capture_ended: async queue drain timeout!");
            // Task 5: the async worker is stuck (e.g., disk stalled) and the
            // queue is not draining. If we leave _async_running true and proceed,
            // the subsequent first_payload()/re-capture races the still-running
            // worker against the new mmap allocator. Force-stop the worker and
            // drop the residual queue so capture_ended can complete safely.
            // first_payload's `if (!_async_running)` restart will spin a fresh
            // worker on the next capture.
            _async_running = false;
            _async_cv.notify_one();        // wake worker so it exits its wait_for
            _async_drain_cv.notify_all();  // unblock any feed thread in backpressure wait
            if (_async_thread.joinable()) {
                _async_thread.join();
            }
            size_t dropped = 0;
            {
                std::lock_guard<std::mutex> q_lock(_async_mutex);
                while (!_async_queue.empty()) {
                    dropped += _async_queue.front().data.size();
                    _async_queue.pop();
                }
                _async_queue_depth = 0;
                _async_queue_bytes_size = 0;
                _async_drain_cv.notify_all();  // final release for any straggler feed thread
            }
            pxv_warn("capture_ended: dropped %llu bytes from async queue after timeout",
                     (unsigned long long)dropped);
            break;
        }
    }
    if (drain_loops > 0) {
        pxv_info("capture_ended: drained async queue in %d ms", drain_loops);
    }
}

// ----------------------------------------------------------------------------
// Async writer thread function
// ----------------------------------------------------------------------------

void LogicSnapshotDiskCacheWriter::async_write_worker()
{
    while (_async_running) {
        AsyncPayload payload;
        {
            std::unique_lock<std::mutex> lock(_async_mutex);
            _async_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !_async_running || !_async_queue.empty();
            });

            if (!_async_running && _async_queue.empty()) break;
            if (_async_queue.empty()) continue;

            payload = std::move(_async_queue.front());
            _async_queue.pop();
            _async_queue_depth = _async_queue.size();
            _async_queue_bytes_size -= payload.data.size();
            // Backpressure (Task 3): if the queue has drained back below the low
            // watermark, release one feed thread that is blocked in enqueue().
            // notify_one (not notify_all) per spec to avoid thundering herd.
            if (_async_queue_bytes_size.load() <= ASYNC_LOW_WATERMARK) {
                _async_drain_cv.notify_one();
            }
        }

        _async_busy.store(true);
        sr_datafeed_logic logic;
        logic.length = payload.data.size();
        logic.data = payload.data.data();
        logic.format = payload.format;

        // LA_CROSS_DATA: raw channel-block — forward to append_cross_payload
        //   (v1.49 bit-copy algorithm, no deinterleave).
        // LA_SPLIT_DATA (default): sample-interleaved — forward to
        //   append_payload_impl which deinterleaves into the chunk tree.
        // unitsize is only used by the SPLIT path; CROSS ignores it.
        if (logic.format == LA_CROSS_DATA) {
            // Cross data length must be a multiple of (channel_num * 8) bytes
            // (each channel gets 8 bytes = 64 samples per chunk). Truncate
            // incomplete chunks to prevent channel desync.
            uint64_t chunk_size = (uint64_t)_owner->_channel_num * 8;
            if (chunk_size > 0)
                logic.length -= logic.length % chunk_size;
            // unitsize unused for CROSS path; set to 1 to avoid div-by-zero.
            logic.unitsize = 1;
        } else {
            // Sample-interleaved: unitsize = bytes per sample group, derived
            // from the snapshot's channel count. _channel_num stays on
            // Snapshot (base) — friend access through _owner.
            logic.unitsize = (uint16_t)((_owner->_channel_num + 7) / 8);
        }

        auto start = std::chrono::steady_clock::now();

        // _mutex is now managed INSIDE append_payload_impl/append_cross_payload
        // (segmented locking: metadata ops hold _mutex, mmap data writes don't).
        // This prevents the async writer from holding _mutex during page faults
        // (which block UI's get_samples).
        if (logic.format == LA_CROSS_DATA)
            _owner->append_cross_payload(logic);
        else
            _owner->append_payload_impl(logic);

        auto end = std::chrono::steady_clock::now();
        _async_bytes_written += payload.data.size();

        double elapsed_s = std::chrono::duration<double>(end - start).count();
        if (elapsed_s > 0) {
            double mbps = (payload.data.size() / (1024.0 * 1024.0)) / elapsed_s;
            // Exponential moving average for smoothing UI
            double old = _async_write_speed_mbps.load();
            if (old == 0.0) _async_write_speed_mbps = mbps;
            else _async_write_speed_mbps = old * 0.8 + mbps * 0.2;
        }
        _async_busy.store(false);
    }
}
