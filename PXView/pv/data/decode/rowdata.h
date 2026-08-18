/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_DATA_DECODE_ROWDATA_H
#define PXVIEW_PV_DATA_DECODE_ROWDATA_H

#include <algorithm>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "pv/data/decode/annotation.h"

struct srd_proto_data;
class DecoderStatus;

namespace pv {
namespace data {
namespace decode {

// P2-7 fix: RowData now stores Annotation as a value type in a deque,
// matching PulseView's design. This eliminates:
//   - Manual new/delete of Annotation objects
//   - The AnnotationPool global singleton memory pool
//   - Memory leak risks from missed delete paths
//   - const-correctness violations (API now returns const Annotation*)

// Immutable segment of a row's annotation data. Produced by RowData::frozen_
// snapshot() when new annotations accumulate between two publishes; once a
// segment is handed to a RowDataSnapshot it is never modified again, so the
// GUI render path can read it lock-free. This makes publishing *incremental*:
// a new snapshot only copies the annotations added since the previous publish
// instead of re-copying the whole deque on every 16ms tick.
struct AnnotationSegment {
  // Frozen batch of annotations (append order == start_sample order).
  std::deque<Annotation> anns;
  // Fast bounds for cross-segment binary search (valid when !anns.empty()).
  uint64_t first_start = 0;
  uint64_t last_end = 0;
};

// Immutable snapshot of a row's annotation data, published by the decode
// thread (via RowData::frozen_snapshot) and read lock-free by the GUI
// render path (paint_mid / paint_back). The snapshot is a list of frozen
// segments; a new publish reuses the previous segments (shared_ptr) and only
// appends a segment with the newly arrived annotations, so publish cost is
// O(annotations since last publish + segment count), not O(total annotations).
class RowDataSnapshot {
public:
  RowDataSnapshot();
  RowDataSnapshot(const RowDataSnapshot &) = delete;
  RowDataSnapshot &operator=(const RowDataSnapshot &) = delete;

  inline bool empty() const { return _item_count == 0; }
  inline uint64_t get_annotation_size() const { return _item_count; }
  inline uint64_t get_max_sample() const {
    return _segments.empty() ? 0 : _segments.back()->last_end;
  }
  inline uint64_t get_max_annotation() const { return _max_annotation; }
  inline uint64_t get_min_annotation() const {
    return (_min_annotation == 0) ? 10 : _min_annotation;
  }

  // Lock-free analogues of the corresponding RowData methods, operating
  // on the immutable segment list.
  std::pair<size_t, size_t> get_visible_range(uint64_t start_sample,
                                              uint64_t end_sample) const;
  const Annotation *get_first_annotation_ending_after(uint64_t sample) const;
  // Index of the first annotation whose start_sample >= sample (global index).
  uint64_t get_annotation_index(uint64_t start_sample) const;
  bool get_annotation(Annotation *ann, uint64_t index) const;
  void get_annotation_subset(std::vector<const Annotation *> &dest,
                             uint64_t start_sample,
                             uint64_t end_sample) const;

  template <typename Fn>
  void for_each_index(size_t start_idx, size_t end_idx, Fn &&fn) const {
    size_t offset = 0;
    for (const auto &seg : _segments) {
      const size_t n = seg->anns.size();
      if (end_idx <= offset)
        break;
      if (start_idx < offset + n) {
        const size_t lo = (start_idx > offset) ? (start_idx - offset) : 0;
        const size_t hi = std::min(n, end_idx - offset);
        for (size_t i = lo; i < hi; ++i)
          fn(seg->anns[i]);
      }
      offset += n;
    }
  }

  // Frozen segments in time order. Only appended to by the decode thread
  // under _visitor_mutex when publishing; never modified afterwards.
  std::vector<std::shared_ptr<const AnnotationSegment>> _segments;
  uint64_t _max_annotation = 0;
  uint64_t _min_annotation = 0;
  uint64_t _item_count = 0;
};

class RowData {
public:
  RowData();
  ~RowData();

public:
  uint64_t get_max_sample();

  uint64_t get_max_annotation();
  uint64_t get_min_annotation();

  uint64_t get_annotation_index(uint64_t start_sample);

  // P2-7 fix: Construct an Annotation in-place from srd_proto_data.
  // Returns true on success, false on allocation failure.
  // Replaces the old push_annotation(Annotation*) which required
  // the caller to new the Annotation.
  bool emplace_annotation(const srd_proto_data *pdata, DecoderStatus *status);

  // Builds (incrementally) an immutable snapshot of this row's annotation
  // data for the render path. Acquires _visitor_mutex to safely copy the
  // annotations that arrived since the previous publish into a new frozen
  // segment; earlier segments are reused through shared_ptr, so this is
  // O(new annotations), not O(total). Returns a shared_ptr to a const
  // snapshot readable lock-free by the GUI thread.
  std::shared_ptr<const RowDataSnapshot> frozen_snapshot();

  inline uint64_t get_annotation_size() {
    std::shared_lock<std::shared_mutex> lock(_visitor_mutex);
    return _item_count;
  }
  inline uint64_t get_annotation_capacity() {
    // std::deque has no capacity() — return size as the effective
    // capacity metric for compatibility with callers that check this.
    std::shared_lock<std::shared_mutex> lock(_visitor_mutex);
    return _item_count;
  }

  bool get_annotation(pv::data::decode::Annotation *ann, uint64_t index);

  // P2-7 fix: returns const Annotation* instead of Annotation*,
  // restoring const-correctness. Pointers point into the deque and
  // are valid until the next emplace_annotation or clear.
  void get_annotation_subset(std::vector<const pv::data::decode::Annotation *> &dest,
                             uint64_t start_sample, uint64_t end_sample);

  void clear();

  std::pair<size_t, size_t> get_visible_range(uint64_t start_sample,
                                              uint64_t end_sample);
  size_t find_index_after_sample(uint64_t sample);
  const Annotation *get_first_annotation_ending_after(uint64_t sample);

  // Iterate annotations in [start_idx, end_idx) under a single shared lock.
  // The previous render path called annotation_at() per element, acquiring
  // and releasing the shared_mutex N times per repaint for dense rows
  // (~1.5M annotations -> ~1.5M lock/unlock cycles on the GUI thread).
  // Indices stay stable across appends because std::deque never reallocates,
  // so the decode thread can keep appending while the caller iterates.
  template <typename Fn>
  void for_each_index(size_t start_idx, size_t end_idx, Fn &&fn) {
    std::shared_lock<std::shared_mutex> lock(_visitor_mutex);
    const size_t n = _annotations.size();
    for (size_t i = start_idx; i < end_idx && i < n; i++)
      fn(_annotations[i]);
  }

private:
  uint64_t _max_annotation;
  uint64_t _min_annotation;
  uint64_t _item_count;
  // P2-7 fix: deque<Annotation> value storage — no pointer ownership,
  // no manual delete. deque keeps element pointers stable across
  // push_back (unlike vector which may reallocate).
  std::deque<Annotation> _annotations;
  // Incremental publish state: the last published snapshot (segments reused
  // by the next publish) and how many annotations are already frozen into it.
  std::shared_ptr<const RowDataSnapshot> _last_snapshot;
  uint64_t _published_count = 0;
  std::shared_mutex _visitor_mutex;
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODE_ROWDATA_H
