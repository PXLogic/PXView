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
}

void Snapshot::set_samplerate(double samplerate)
{
    if (samplerate <= 0) {
        pxv_err("Snapshot: samplerate<=0, aborting");
        return;
    }
    _samplerate = samplerate;
}

} // namespace data
} // namespace pv
