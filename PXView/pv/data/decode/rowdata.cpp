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

RowData::RowData() : _max_annotation(0), _min_annotation(0) { _item_count = 0; }

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

  // Binary search: find the first annotation whose end_sample > start_sample
  auto it = std::lower_bound(
      _annotations.begin(), _annotations.end(), start_sample,
      [](const Annotation &a, uint64_t val) { return a.end_sample() <= val; });

  // Iterate from the found position until annotations start beyond end_sample
  for (; it != _annotations.end(); ++it) {
    if (it->start_sample() > end_sample)
      break;
    dest.push_back(&(*it));
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

  auto it = std::lower_bound(
      _annotations.begin(), _annotations.end(), start_sample,
      [](const Annotation &a, uint64_t val) { return a.end_sample() <= val; });

  size_t start_idx = std::distance(_annotations.begin(), it);

  // End boundary: first annotation whose start_sample > end_sample.
  // The deque is ordered by start_sample (decode append order), so the end
  // boundary is a binary search instead of the previous O(N) linear scan.
  // A row with ~1.5M annotations previously scanned all of them on every
  // repaint (worst case when zoomed out to the full capture), which stalled
  // the GUI thread.
  auto it_end = std::upper_bound(
      _annotations.begin(), _annotations.end(), end_sample,
      [](uint64_t val, const Annotation &a) { return val < a.start_sample(); });

  size_t end_idx = std::distance(_annotations.begin(), it_end);

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

  auto it = std::lower_bound(
      _annotations.begin(), _annotations.end(), sample,
      [](const Annotation &a, uint64_t val) { return a.end_sample() <= val; });

  if (it != _annotations.end())
    return &(*it);
  return nullptr;
}

} // namespace decode
} // namespace data
} // namespace pv
