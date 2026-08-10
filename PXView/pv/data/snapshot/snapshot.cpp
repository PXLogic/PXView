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


#include "pv/data/snapshot/snapshot.h"
#include "pv/base/log.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

namespace pv {
namespace data {

Snapshot::Snapshot(int unit_size, uint64_t total_sample_count, unsigned int channel_num)
    : QObject(nullptr)
{
    assert(unit_size > 0);

    _capacity = 0;
    _channel_num = channel_num;
    _sample_count = 0;
    _total_sample_count = total_sample_count;
    _ring_sample_count = 0;
    _unit_size = unit_size;
    _memory_failed = false;
    _last_ended = true;
    _unit_bytes = 1;
    _unit_pitch = 0;
    _samplerate = 0.0;
}

Snapshot::~Snapshot()
{
    free_data();
}

void Snapshot::free_data()
{
    // TS-4 fix: Callers MUST hold _mutex when calling free_data().
    // All current callers (LogicSnapshot::clear, first_payload, copy_from)
    // already acquire _mutex before calling this method. The destructor
    // (~Snapshot) calls it without locking, which is safe because no
    // concurrent access is possible (the object is being destroyed after
    // all shared_ptr references are released).
    // Previously these fields were written without any locking documentation,
    // making it unclear whether concurrent readers (e.g. decode thread calling
    // get_sample_count()) were safe. The protocol is now explicit: callers
    // must hold _mutex, and concurrent readers (get_sample_count, empty, etc.)
    // also acquire _mutex, establishing a happens-before relationship.
    _capacity = 0;
    _sample_count = 0;
    _total_sample_count = 0;
    _ch_index.clear();
}

bool Snapshot::empty()
{
    if (get_sample_count() == 0)
        return true;
    else
        return false;
}

uint64_t Snapshot::get_sample_count()
{
    // P1-5 fix: recursive_mutex allows this to be called from within
    // other methods that already hold the lock without deadlocking.
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _sample_count;
}

uint64_t Snapshot::get_ring_sample_count()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _ring_sample_count;
}
 
uint64_t Snapshot::get_ring_start()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return ring_start();    
}

uint64_t Snapshot::get_ring_end()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return ring_end();     
}
 
uint64_t Snapshot::ring_start()
{ 
    if (_sample_count < _total_sample_count)
        return 0;
    else
        return _ring_sample_count;
}
 
uint64_t Snapshot::ring_end()
{ 
    if (_sample_count == 0)
        return 0;
    else if (_ring_sample_count == 0)
        return _total_sample_count - 1;
    else
        return _ring_sample_count - 1;
}

void Snapshot::capture_ended()
{
    _last_ended = true;
    // C-2: Mark segment as complete and emit completed() signal so
    // that any listener (e.g. SigSession, DecoderStack) is notified
    // via Qt's signal/slot mechanism instead of polling _last_ended.
    set_complete();
}

// C-2: Segment completion — sets flag and emits Qt signal.
// This replaces polling-based completion detection.
void Snapshot::set_complete()
{
    {
        // _is_complete is atomic, but we use the mutex to establish
        // a happens-before relationship with any reader that holds _mutex.
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _is_complete = true;
    }
    _mem_optimization_requested = true;
    emit completed();
}

bool Snapshot::is_complete() const
{
    return _is_complete.load(std::memory_order_acquire);
}

// B-6: Default no-op. Subclasses (e.g. LogicSnapshot) override to free
// chunk-level unused memory. If _mem_optimization_requested is false,
// nothing happens. If there are active iterators (subclass checks
// _iterator_count), release is deferred.
void Snapshot::free_unused_memory()
{
    _mem_optimization_requested = false;
}

void Snapshot::set_samplerate(double samplerate)
{
    if (samplerate <= 0) {
        pxv_err("Snapshot: samplerate<=0, aborting");
        return;
    }
    _samplerate = samplerate;
}

// ============================================================================
// B-1: Multi-segment support
// ============================================================================

uint32_t Snapshot::get_segment_count() const
{
    return _segment_count;
}

uint32_t Snapshot::get_current_segment_id() const
{
    return _current_segment_id;
}

void Snapshot::set_current_segment_id(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    _current_segment_id = id;
}

uint32_t Snapshot::create_new_segment()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // Complete the current segment if not already.
    if (!_is_complete.load(std::memory_order_acquire)) {
        _is_complete.store(true, std::memory_order_release);
        emit completed();
    }

    // Allocate a new segment ID.
    uint32_t new_id = _segment_count;
    _segment_count++;
    _current_segment_id = new_id;

    // Grow the per-segment sample count vector.
    if (_segment_sample_counts.size() < _segment_count)
        _segment_sample_counts.resize(_segment_count, 0);

    // Reset completion flag for the new segment.
    _is_complete.store(false, std::memory_order_release);

    return new_id;
}

uint64_t Snapshot::get_total_sample_count_across_segments() const
{
    // _mutex is mutable, so we can lock directly in const methods.
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    uint64_t total = 0;
    for (uint32_t i = 0; i < _segment_count; i++) {
        if (i < _segment_sample_counts.size())
            total += _segment_sample_counts[i];
        else
            total += _sample_count;  // fallback for single-segment
    }
    return total;
}

uint64_t Snapshot::get_segment_sample_count(uint32_t segment_id) const
{
    // _mutex is mutable, so we can lock directly in const methods.
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (segment_id < _segment_sample_counts.size())
        return _segment_sample_counts[segment_id];
    // Single-segment backward compatibility
    if (segment_id == 0)
        return _sample_count;
    return 0;
}

} // namespace data
} // namespace pv
