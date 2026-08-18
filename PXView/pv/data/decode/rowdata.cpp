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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <mutex>

#include "pv/data/decode/rowdata.h"

using std::max;
using std::min;
using std::vector;

namespace pv {
namespace data {
namespace decode {

RowDataSnapshot::RowDataSnapshot() = default;

// Global index of the first annotation whose start_sample > sample.
// start_sample IS monotonic across the whole deque (decode append order),
// so segment-skip + in-segment binary search is always correct.
size_t RowDataSnapshot::_first_start_after(uint64_t sample) const {
  size_t offset = 0;
  for (const auto &seg : _segments) {
    const auto &anns = seg->anns;
    if (anns.empty())
      continue;
    if (seg->first_start > sample)
      return offset; // whole segment starts after sample
    const auto it = std::upper_bound(
        anns.begin(), anns.end(), sample,
        [](uint64_t val, const Annotation &a) { return val < a.start_sample(); });
    offset += std::distance(anns.begin(), it);
    if (it != anns.end())
      return offset; // found inside this segment; later segments start later
  }
  return offset; // == _item_count
}

// Global index of the FIRST (lowest-index) annotation whose end_sample >
// sample. This is the correct lower bound for the visible range: every
// annotation overlapping [sample, ...] has end_sample > sample, so its index
// is >= this value, and no overlapping annotation can be skipped.
//
// end_sample is NOT monotonic (a wide annotation can be followed by narrow
// ones), so a binary search on end would be wrong. The deque is ordered by
// start_sample though, which IS monotonic. So: walk segments forward, skipping
// any whole segment whose MAX end <= sample (max_end, not last_end — the tail
// of a segment may be narrow while its head holds a wide straddling
// annotation). In the first segment that contains an end > sample, scan
// FORWARD from the segment head and return the first match. Scanning forward
// (not backward) is essential: a backward scan from the start-ordered
// insertion point returns the LARGEST such index (a narrow sliver just before
// the viewport), which silently drops the low-index wide annotation that
// actually straddles the window — exactly the high-zoom "row disappears" bug.
// max_end > sample guarantees a match exists in this segment before any later
// segment, so the first match here is the global answer.
size_t RowDataSnapshot::_first_end_after(uint64_t sample) const {
  size_t offset = 0;
  for (const auto &seg : _segments) {
    const auto &anns = seg->anns;
    if (anns.empty()) {
      offset += anns.size();
      continue;
    }
    if (seg->max_end <= sample) {
      offset += anns.size();
      continue;
    }
    for (size_t i = 0; i < anns.size(); ++i) {
      if (anns[i].end_sample() > sample)
        return offset + i;
    }
    offset += anns.size(); // unreachable when max_end > sample
  }
  return offset; // == _item_count
}

// Global index of the FIRST (lowest-index) annotation whose end_sample >=
// sample. Same forward-scan strategy as _first_end_after but with the >=
// boundary so instant annotations (start == end) at exactly `sample` are
// found. The dense render path falls back to this when _first_end_after finds
// nothing (e.g. a row of all-instant annotations).
size_t RowDataSnapshot::_first_end_at_or_after(uint64_t sample) const {
  size_t offset = 0;
  for (const auto &seg : _segments) {
    const auto &anns = seg->anns;
    if (anns.empty()) {
      offset += anns.size();
      continue;
    }
    if (seg->max_end < sample) {  // segment skip: NOT >= sample
      offset += anns.size();
      continue;
    }
    for (size_t i = 0; i < anns.size(); ++i) {
      if (anns[i].end_sample() >= sample)
        return offset + i;
    }
    offset += anns.size(); // unreachable when max_end >= sample
  }
  return offset; // == _item_count
}

const Annotation *RowDataSnapshot::_annotation_at(size_t index) const {
  size_t offset = 0;
  for (const auto &seg : _segments) {
    const auto &anns = seg->anns;
    if (index < offset + anns.size())
      return &anns[index - offset];
    offset += anns.size();
  }
  return nullptr;
}

std::pair<size_t, size_t> RowDataSnapshot::get_visible_range(
    uint64_t start_sample, uint64_t end_sample) const {
  if (_segments.empty())
    return {0, 0};

  const size_t begin = _first_end_after(start_sample);

  // Upper bound. The deque is ordered by start_sample ONLY when decoders emit
  // annotations in time order. In practice many decoders append a WIDE summary
  // annotation (e.g. a whole-frame label) AFTER the narrow detail annotations
  // it covers: that summary has an EARLIER start_sample but a LATER index, so
  // start_sample is NOT globally monotonic. A pure binary search on start_sample
  // therefore returns an upper bound that excludes such late, early-start
  // annotations, dropping them from the row (rendered as AFTER_LOOKUP_END — the
  // row "disappears"). So after the fast binary-search candidate we linearly
  // extend through the remaining segments to include any annotation whose
  // start_sample is at or before the window end, regardless of its (late)
  // position. Sorted intermediate segments can be safely early-stopped; the
  // LAST segment is always scanned in full (summaries are typically appended
  // there), and any segment that proves non-monotonic keeps scanning so an
  // in-segment summary is not missed.
  size_t end = _first_start_after(end_sample);
  {
    size_t last_nonempty = _segments.size();
    for (size_t s = _segments.size(); s-- > 0;) {
      if (!_segments[s]->anns.empty()) {
        last_nonempty = s;
        break;
      }
    }
    size_t offset = 0;
    bool scanning = false;
    for (size_t si = 0; si < _segments.size(); ++si) {
      const auto &seg = _segments[si];
      const auto &anns = seg->anns;
      const size_t n = anns.size();
      if (n == 0) {
        offset += n;
        continue;
      }
      if (!scanning && offset + n > end)
        scanning = true;
      if (scanning) {
        const bool is_last = (si == last_nonempty);
        uint64_t prev = 0;
        bool monotonic = true;
        for (size_t i = 0; i < n; ++i) {
          const uint64_t s = anns[i].start_sample();
          if (s <= end_sample)
            end = std::max(end, offset + i + 1);
          if (!is_last && monotonic && s > end_sample && s >= prev)
            break;  // sorted tail past the window: safe to stop
          if (i > 0 && s < prev)
            monotonic = false;  // decoder went backwards -> summary present
          prev = s;
        }
      }
      offset += n;
    }
  }

  if (end < begin)
    return {begin, begin};
  return {begin, end};
}

const Annotation *RowDataSnapshot::get_first_annotation_ending_after(
    uint64_t sample) const {
  const size_t idx = _first_end_after(sample);
  if (idx >= _item_count)
    return nullptr;
  return _annotation_at(idx);
}

const Annotation *RowDataSnapshot::get_first_annotation_ending_at_or_after(
    uint64_t sample) const {
  const size_t idx = _first_end_at_or_after(sample);
  if (idx >= _item_count)
    return nullptr;
  return _annotation_at(idx);
}

uint64_t RowDataSnapshot::get_annotation_index(uint64_t start_sample) const {
  // Global index of the first annotation whose start_sample > start_sample
  // (same semantics as RowData::get_annotation_index).
  return _first_start_after(start_sample);
}

bool RowDataSnapshot::get_annotation(Annotation *ann, uint64_t index) const {
  if (!ann)
    return false;
  size_t offset = 0;
  for (const auto &seg : _segments) {
    const auto &anns = seg->anns;
    if (index < offset + anns.size()) {
      *ann = anns[index - offset];
      return true;
    }
    offset += anns.size();
  }
  return false;
}

void RowDataSnapshot::get_annotation_subset(
    std::vector<const Annotation *> &dest, uint64_t start_sample,
    uint64_t end_sample) const {
  if (_segments.empty())
    return;
  const auto range = get_visible_range(start_sample, end_sample);
  for_each_index(range.first, range.second,
                 [&dest](const Annotation &a, size_t) {
                   dest.push_back(&a);
                 });
}

RowData::RowData() : _max_annotation(0), _min_annotation(0) { _item_count = 0; }

std::shared_ptr<const RowDataSnapshot> RowData::frozen_snapshot() {
  // Safe to touch the deque because the caller (publish_snapshot) holds
  // _rows_mutex exclusively, and every write to this deque is gated by
  // _rows_mutex too. We still take _visitor_mutex for defense-in-depth
  // and to keep lock ordering (_rows_mutex -> _visitor_mutex) consistent
  // with the write path.
  //
  // Incremental publish: reuse the previously frozen segments and only copy
  // the annotations that arrived since the last publish into a new segment.
  // Publish cost is therefore O(new annotations + segment count) instead of
  // O(total annotations), which keeps the 16ms decode-loop publish cheap even
  // for multi-hundred-thousand annotation rows.
  std::unique_lock<std::shared_mutex> lock(_visitor_mutex);

  // Fast path: no new annotations since the last publish -> the immutable
  // snapshot is still accurate, reuse it as-is.
  if (_last_snapshot && _published_count == _annotations.size())
    return _last_snapshot;

  auto snap = std::make_shared<RowDataSnapshot>();
  if (_last_snapshot)
    snap->_segments = _last_snapshot->_segments; // reuse frozen segments

  if (_published_count < _annotations.size()) {
    auto seg = std::make_shared<AnnotationSegment>();
    seg->anns.assign(_annotations.begin() + _published_count,
                     _annotations.end());
    seg->first_start = seg->anns.front().start_sample();
    seg->last_end = seg->anns.back().end_sample();
    // max_end: true maximum end over the whole segment. Because end_sample is
    // not monotonic, the last element's end is NOT the maximum; computing the
    // max here keeps the segment-skip test in _first_end_after correct.
    uint64_t max_end = 0;
    for (const auto &a : seg->anns)
      max_end = std::max(max_end, a.end_sample());
    seg->max_end = max_end;
    snap->_segments.push_back(std::move(seg));
    _published_count = _annotations.size();
  }

  snap->_max_annotation = _max_annotation;
  snap->_min_annotation = _min_annotation;
  snap->_item_count = _item_count;
  _last_snapshot = snap;
  return snap;
}

RowData::~RowData() {
  // P2-7 fix: deque automatically destroys all Annotation objects.
  // No manual delete needed.
}

void RowData::clear() {
  std::unique_lock<std::shared_mutex> lock(_visitor_mutex);

  // P2-7 fix: deque's clear() destroys all Annotation value objects.
  // No manual delete loop needed.
  _annotations.clear();
  _item_count = 0;
  _min_annotation = 0;
  _max_annotation = 0;
  _published_count = 0;
  _last_snapshot.reset();
}

uint64_t RowData::get_max_sample() {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return 0;
  return _annotations.back().end_sample();
}

uint64_t RowData::get_max_annotation() { return _max_annotation; }

uint64_t RowData::get_min_annotation() {
  if (_min_annotation == 0)
    return 10;
  else
    return _min_annotation;
}

void RowData::get_annotation_subset(
    std::vector<const pv::data::decode::Annotation *> &dest, uint64_t start_sample,
    uint64_t end_sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return;

  // Lower bound: SMALLEST index whose end_sample > start_sample. end_sample is
  // NOT monotonic (wide annotations can be followed by narrow ones), so a
  // backward scan would return the LARGEST such index and silently drop wide
  // annotations straddling the window. A forward scan from the head finds the
  // true first overlap.
  size_t start_idx = _annotations.size();
  for (size_t i = 0; i < _annotations.size(); ++i) {
    if (_annotations[i].end_sample() > start_sample) {
      start_idx = i;
      break;
    }
  }

  // Iterate from the lower bound until annotations start beyond end_sample
  for (size_t i = start_idx; i < _annotations.size(); ++i) {
    const Annotation &a = _annotations[i];
    if (a.start_sample() > end_sample)
      break;
    dest.push_back(&a);
  }
}

uint64_t RowData::get_annotation_index(uint64_t start_sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  auto it = std::upper_bound(
      _annotations.begin(), _annotations.end(), start_sample,
      [](uint64_t val, const Annotation &a) { return val < a.start_sample(); });

  return std::distance(_annotations.begin(), it);
}

// P2-7 fix: emplace_annotation constructs the Annotation in-place inside
// the deque, replacing the old push_annotation(Annotation*) which required
// the caller to new the Annotation object.
bool RowData::emplace_annotation(const srd_proto_data *pdata, DecoderStatus *status) {
  if (!pdata)
    return false;
  assert(pdata);

  std::unique_lock<std::shared_mutex> lock(_visitor_mutex);

  try {
    _annotations.emplace_back(pdata, status);
    _item_count = _annotations.size();

    const Annotation &a = _annotations.back();
    _max_annotation = max(_max_annotation, a.end_sample() - a.start_sample());

    if (a.end_sample() != a.start_sample()) {
      if (_min_annotation == 0) {
        _min_annotation = a.end_sample() - a.start_sample();
      } else {
        _min_annotation =
            min(_min_annotation, a.end_sample() - a.start_sample());
      }
    }

    return true;

  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool RowData::get_annotation(Annotation *ann, uint64_t index) {
  if (!ann)
    return false;
  assert(ann);

  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (index < _annotations.size()) {
    *ann = _annotations[index]; // copy
    return true;
  } else {
    return false;
  }
}

std::pair<size_t, size_t> RowData::get_visible_range(uint64_t start_sample,
                                                     uint64_t end_sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return {0, 0};

  // ---- Lower bound: SMALLEST index whose end_sample > start_sample ----
  // end_sample is NOT monotonic (a wide annotation can be followed by narrow
  // ones), so no direct binary search on end is possible. However every
  // annotation satisfies end_sample >= start_sample (decode semantics), hence
  // the first index whose start_sample > start_sample -- found by binary
  // search below -- always has end >= start > start_sample and is a valid
  // match. The true lower bound is therefore the first match inside [0, pos_b);
  // if none exists there, pos_b itself is the answer. This caps the miss-path
  // scan at pos_b instead of walking the whole deque, and degrades to O(1)
  // when the window is before the first annotation.
  const size_t pos_b =
      std::upper_bound(_annotations.begin(), _annotations.end(), start_sample,
                       [](uint64_t val, const Annotation &a) {
                         return val < a.start_sample();
                       }) -
      _annotations.begin();
  size_t start_idx = pos_b;
  for (size_t i = 0; i < pos_b; ++i) {
    if (_annotations[i].end_sample() > start_sample) {
      start_idx = i;
      break;
    }
  }

  // ---- Upper bound: largest index whose start_sample <= end_sample ----
  // start_sample is NOT globally monotonic: decoders often append a wide
  // summary annotation (early start, LATE index) after the detail annotations
  // it covers, so a pure binary search would exclude it and make the row
  // disappear. Strategy (mirrors RowDataSnapshot::get_visible_range): take the
  // binary-search candidate, then scan forward from it. A monotonic tail past
  // the window is early-stopped; once a start_sample regression is detected
  // the scan must run to the end so late-appended summaries are never missed.
  size_t end_idx = pos_b;
  {
    const size_t ub =
        std::upper_bound(_annotations.begin(), _annotations.end(), end_sample,
                         [](uint64_t val, const Annotation &a) {
                           return val < a.start_sample();
                         }) -
        _annotations.begin();
    end_idx = ub;
    uint64_t prev = 0;
    bool monotonic = true;
    for (size_t i = ub; i < _annotations.size(); ++i) {
      const uint64_t s = _annotations[i].start_sample();
      if (s <= end_sample)
        end_idx = i + 1;
      if (monotonic && s > end_sample && s >= prev)
        break;  // sorted tail past the window: safe to stop
      if (i > ub && s < prev)
        monotonic = false;  // start_sample went backwards -> keep scanning
      prev = s;
    }
  }

  if (end_idx < start_idx)
    end_idx = start_idx;

  return {start_idx, end_idx};
}

size_t RowData::find_index_after_sample(uint64_t sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  auto it = std::upper_bound(
      _annotations.begin(), _annotations.end(), sample,
      [](uint64_t val, const Annotation &a) { return val < a.start_sample(); });

  return std::distance(_annotations.begin(), it);
}

const Annotation *RowData::get_first_annotation_ending_after(uint64_t sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return nullptr;

  // First (lowest-index) annotation whose end_sample > sample. end_sample is
  // NOT monotonic, so a binary search on end would be wrong; a forward scan
  // from the head finds the true first. A backward scan would return the
  // LARGEST such index and silently drop the wide annotation that actually
  // straddles the window.
  for (const auto &a : _annotations) {
    if (a.end_sample() > sample)
      return &a;
  }
  return nullptr;
}

} // namespace decode
} // namespace data
} // namespace pv
