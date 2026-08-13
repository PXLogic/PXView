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

// P1-5 fix: Use recursive_mutex instead of mutex to allow nested locking
// within the same thread. This matches PulseView's Segment::mutex_ design
// and eliminates the need for the sample_count() non-locking accessor.
#include <mutex>

#include <QObject>

namespace pv {
namespace data {

// C-2: Snapshot now inherits QObject so it can emit the completed() signal.
// This matches PulseView's Segment::completed() pattern and allows
// event-driven notification instead of polling _last_ended.
class Snapshot : public QObject
{
    Q_OBJECT

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

    // B-1: Multi-segment support.
    // In PulseView, each Segment is a self-contained data block with its
    // own sample count and completion state. Multiple segments are stored
    // in a deque. PXView's current single-segment architecture is preserved
    // as a backward-compatible layer: when _segment_count == 1, all existing
    // code works unchanged.
    //
    // The following methods provide the multi-segment API:
    uint32_t get_segment_count() const;
    uint32_t get_current_segment_id() const;
    void set_current_segment_id(uint32_t id);

    // B-1: Create a new segment. Subclasses must override to create the
    // appropriate segment type (LogicSegment, AnalogSegment, etc.).
    // Returns the new segment ID.
    virtual uint32_t create_new_segment();

    // B-1: Get total sample count across all segments.
    uint64_t get_total_sample_count_across_segments() const;

    // B-1: Get sample count for a specific segment.
    uint64_t get_segment_sample_count(uint32_t segment_id) const;

    // C-2: PulseView-style segment completion notification.
    // set_complete() marks the snapshot as complete and emits completed().
    // is_complete() checks the flag without polling _last_ended.
    void set_complete();
    bool is_complete() const;

    // B-6: PulseView-style memory optimisation.
    // Frees memory from completed chunks that are no longer needed.
    // Skips release if there are active sample iterators.
    // Default implementation is a no-op; subclasses override to free
    // chunk-level unused memory (e.g. LogicSnapshot frees head blocks).
    virtual void free_unused_memory();

signals:
    // C-2: Emitted when set_complete() is called.
    void completed();

protected:
    // P1-5 fix: Callers MUST hold _mutex when calling free_data().
    // recursive_mutex allows the same thread to lock multiple times
    // without deadlocking, matching PulseView's Segment::mutex_ design.
    virtual void free_data();

    // P1-5 fix: With recursive_mutex, get_sample_count() can be called
    // from within other locked methods without deadlocking. The separate
    // non-locking sample_count() accessor is retained for performance in
    // hot paths where the caller already holds the lock.
    inline uint64_t sample_count(){
        return _sample_count;
    }

    uint64_t ring_start();
    uint64_t ring_end();

protected:
    // P1-5 fix: recursive_mutex allows nested locking within the same thread
    mutable std::recursive_mutex  _mutex;
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

    // C-2: Segment completion flag, separate from _last_ended.
    // _last_ended means the capture stream ended (may be temporary in
    // stream mode). _is_complete means the segment is fully populated.
    std::atomic<bool>   _is_complete{false};

    // B-6: Memory optimisation request flag. When set, free_unused_memory()
    // will attempt to release unused chunk memory on the next call.
    // Atomic: set by capture_ended() (data-feed thread) and read by
    // free_unused_memory() (main/view thread).
    std::atomic<bool>   _mem_optimization_requested{false};

    // B-1: Multi-segment state.
    // _segment_count: number of segments (1 for single-segment mode).
    // _current_segment_id: which segment is currently being filled.
    // _segment_sample_counts: per-segment sample counts.
    // When _segment_count == 1, all existing single-segment code works
    // unchanged using _sample_count for the single segment.
    uint32_t            _segment_count = 1;
    uint32_t            _current_segment_id = 0;
    std::vector<uint64_t> _segment_sample_counts;

protected:
    bool        _is_float = false;  // 上游 encoding->is_float：paint_trace 需用 reinterpret_cast<float*>
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SNAPSHOT_H
