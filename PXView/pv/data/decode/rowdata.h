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
#include "pv/data/decode/annotation_heap.h"

struct srd_ann_item;
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
  // Plan A: allocated from the DecoderStack's dedicated heap (HeapAllocator)
  // so decode-thread annotation storage doesn't contend with the GUI thread's
  // process-heap allocations (the ~1s heap-lock convoy). heap_ref keeps the
  // dedicated heap alive as long as any snapshot segment referencing it exists.
  std::deque<Annotation, HeapAllocator<Annotation>> anns;
  std::shared_ptr<void> heap_ref;
  // Fast bounds for cross-segment binary search (valid when !anns.empty()).
  uint64_t first_start = 0;  // first annotation's start_sample
  uint64_t last_end = 0;     // last annotation's end_sample (for get_max_sample)
  // MAXIMUM end_sample over the whole segment. The deque is ordered by
  // start_sample only; end_sample is NOT monotonic (a wide annotation can be
  // followed by narrow ones), so a segment-skip test must compare against the
  // true maximum end, not the last element's end — otherwise a segment whose
  // tail is narrow but whose head has a wide straddling annotation would be
  // wrongly skipped and its annotations would disappear when zooming.
  uint64_t max_end = 0;

  explicit AnnotationSegment(void *h = nullptr, std::shared_ptr<void> ref = {})
      : anns(HeapAllocator<Annotation>(h)), heap_ref(std::move(ref)) {}
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
  // Like get_first_annotation_ending_after but uses >= (end >= sample)
  // instead of > (end > sample). This finds instant annotations (end == start)
  // that would be missed by the strict > variant. Used by the dense render
  // path as a fallback when the strict variant returns nullptr.
  const Annotation *get_first_annotation_ending_at_or_after(
      uint64_t sample) const;
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
          fn(seg->anns[i], offset + i);
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

private:
  // Global index of the first annotation whose start_sample > sample
  // (start_sample IS monotonic across the deque, so this binary search is
  // always correct). Returns _item_count when none exists.
  size_t _first_start_after(uint64_t sample) const;
  // Global index of the first annotation whose end_sample > sample. Because
  // end_sample is not monotonic, this locates the start-ordered insertion
  // point and then scans a bounded window backwards for a straddling
  // annotation. Returns _item_count when none exists.
  size_t _first_end_after(uint64_t sample) const;
  // Global index of the first annotation whose end_sample >= sample. Same
  // segment-skip + bounded backward-scan strategy as _first_end_after but
  // with the >= boundary so instant annotations (end == start) are found.
  // Returns _item_count when none exists.
  size_t _first_end_at_or_after(uint64_t sample) const;
  const Annotation *_annotation_at(size_t index) const;
};

class RowData {
public:
  // Plan A: a RowData belongs to one DecoderStack; pass its dedicated heap
  // (AnnotationHeapPtr) so the annotation deque allocates off the shared
  // process heap. Default = process heap (safe fallback).
  explicit RowData(AnnotationHeapPtr heap = {});
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

  // 方案 E: 批量落库。items 为同一行的注解；内部一次获取 _visitor_mutex，
  // 遍历 items 在栈上构造 srd_proto_data 视图（pdo 置 NULL）后逐个落库，
  // 与 emplace_annotation 产生完全一致的最终数据。bad_alloc 返回 false。
  bool emplace_annotations(const std::vector<const srd_ann_item *> &items,
                           DecoderStatus *status);

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
      fn(_annotations[i], i);
  }

private:
  // 方案 E: 单个注解落库后的统计更新，emplace_annotation / emplace_annotations
  // 共用，保证两条路径的 _item_count/_max_annotation/_min_annotation 一致。
  inline void apply_annotation_stats(const Annotation &a) {
    _item_count = _annotations.size();
    _max_annotation = std::max(_max_annotation, a.end_sample() - a.start_sample());
    if (a.end_sample() != a.start_sample()) {
      if (_min_annotation == 0) {
        _min_annotation = a.end_sample() - a.start_sample();
      } else {
        _min_annotation = std::min(_min_annotation, a.end_sample() - a.start_sample());
      }
    }
  }

  uint64_t _max_annotation;
  uint64_t _min_annotation;
  uint64_t _item_count;
  // Plan A: dedicated-heap reference + raw HANDLE. Declared before
  // _annotations so the deque ctor can use _heap for its allocator.
  AnnotationHeapPtr _heap_ref;
  void *_heap = nullptr;
  // P2-7 fix: deque<Annotation> value storage — no pointer ownership,
  // no manual delete. deque keeps element pointers stable across
  // push_back (unlike vector which may reallocate). Allocated from the
  // DecoderStack's dedicated heap to avoid process-heap lock contention.
  std::deque<Annotation, HeapAllocator<Annotation>> _annotations;
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
