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

#ifndef PXVIEW_PV_DATA_SNAPSHOT_H
#define PXVIEW_PV_DATA_SNAPSHOT_H

#include <mutex>
#include <vector>
#include <atomic>

namespace pv {
namespace data {

class Snapshot
{
public:
    Snapshot(int unit_size, uint64_t total_sample_count, unsigned int channel_num);

	virtual ~Snapshot();

    virtual void clear() = 0;
    virtual void init() = 0;

	uint64_t get_sample_count();
    uint64_t get_ring_sample_count();
    uint64_t get_ring_start();
    uint64_t get_ring_end();

    [[nodiscard]] inline int unit_size(){
        return _unit_size;
    }

    [[nodiscard]] inline uint8_t get_unit_bytes(){
        return _unit_bytes;
    }

    // 上游 libsigrok analog 数据可能是 float 格式（encoding->is_float）。
    // AnalogSignal::paint_trace 需据此选择读取方式。
    [[nodiscard]] inline bool is_float() const { return _is_float; }

    [[nodiscard]] inline bool memory_failed(){
        return _memory_failed;
    }

    bool empty();

    [[nodiscard]] inline bool last_ended(){
        return _last_ended;
    }

    [[nodiscard]] inline unsigned int get_channel_num(){
        return _channel_num;
    }

    [[nodiscard]] inline bool have_data(){
        return !empty();
    }

    [[nodiscard]] inline double samplerate(){
        return _samplerate; 
    }

    void set_samplerate(double samplerate);

    virtual void capture_ended();
    virtual bool has_data(int index) = 0;
    virtual int get_block_num() = 0;
    virtual uint64_t get_block_size(int block_index) = 0;
     

protected:
    // TS-4 fix: Callers MUST hold _mutex when calling free_data().
    // This establishes a happens-before with concurrent readers
    // (get_sample_count, empty, etc.) that also acquire _mutex.
    virtual void free_data();

    // TS-4 fix: Callers MUST hold _mutex when calling sample_count().
    // This is a non-locking accessor for subclass use under _mutex
    // protection. Public callers should use get_sample_count() instead.
    inline uint64_t sample_count(){
        return _sample_count;
    }

    uint64_t ring_start();
    uint64_t ring_end();

protected:
    mutable std::mutex  _mutex;  
    mutable std::vector<uint16_t> _ch_index;

    uint64_t    _capacity;
    unsigned int _channel_num;
	uint64_t    _sample_count;
    uint64_t    _total_sample_count;
    uint64_t    _ring_sample_count;
	int         _unit_size;
    uint8_t     _unit_bytes;
    uint16_t    _unit_pitch;
    // std::atomic: these fields are read by inline accessors (memory_failed(),
    // last_ended(), samplerate()) without holding _mutex, and written by
    // capture_ended() / set_samplerate() / subclass init without lock.
    // Atomic prevents data-race UB. operator= and implicit conversion keep
    // existing code (_memory_failed = true, if (_memory_failed), etc.) working.
    std::atomic<bool>   _memory_failed;
    std::atomic<bool>   _last_ended;
    std::atomic<double> _samplerate;
protected:
    bool        _is_float = false;  // 上游 encoding->is_float：paint_trace 需用 reinterpret_cast<float*>
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SNAPSHOT_H
