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
#include <assert.h>
#include <math.h>
#include <mutex>

#include "rowdata.h"

using std::max;
using std::min;
using std::vector;

namespace pv {
namespace data {
namespace decode {

RowData::RowData() : _max_annotation(0), _min_annotation(0) { _item_count = 0; }

RowData::~RowData() {
  // stack object can not destory the sources
}

void RowData::clear() {
  std::unique_lock<std::shared_mutex> lock(_visitor_mutex);

  // destroy objercts
  for (Annotation *p : _annotations) {
    delete p;
  }
  _annotations.clear();
  _item_count = 0;
  _min_annotation = 0;
}

uint64_t RowData::get_max_sample() {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return 0;
  return _annotations.back()->end_sample();
}

uint64_t RowData::get_max_annotation() { return _max_annotation; }

uint64_t RowData::get_min_annotation() {
  if (_min_annotation == 0)
    return 10;
  else
    return _min_annotation;
}

void RowData::get_annotation_subset(
    std::vector<pv::data::decode::Annotation *> &dest, uint64_t start_sample,
    uint64_t end_sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return;

  // Binary search: find the first annotation whose end_sample > start_sample
  // Since annotations are ordered by start_sample, we use lower_bound to find
  // the first annotation that could possibly overlap with [start_sample,
  // end_sample]
  auto it = std::lower_bound(
      _annotations.begin(), _annotations.end(), start_sample,
      [](Annotation *a, uint64_t val) { return a->end_sample() <= val; });

  // Iterate from the found position until annotations start beyond end_sample
  for (; it != _annotations.end(); ++it) {
    Annotation *p = *it;
    if (p->start_sample() > end_sample)
      break;
    dest.push_back(p);
  }
}

uint64_t RowData::get_annotation_index(uint64_t start_sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  auto it = std::upper_bound(
      _annotations.begin(), _annotations.end(), start_sample,
      [](uint64_t val, Annotation *a) { return val < a->start_sample(); });

  return std::distance(_annotations.begin(), it);
}

bool RowData::push_annotation(Annotation *a) {
  assert(a);

  std::unique_lock<std::shared_mutex> lock(_visitor_mutex);

  try {
    _annotations.push_back(a);
    _item_count = _annotations.size();
    _max_annotation = max(_max_annotation, a->end_sample() - a->start_sample());

    if (a->end_sample() != a->start_sample()) {
      if (_min_annotation == 0) {
        _min_annotation = a->end_sample() - a->start_sample();
      } else {
        _min_annotation =
            min(_min_annotation, a->end_sample() - a->start_sample());
      }
    }

    return true;

  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool RowData::get_annotation(Annotation *ann, uint64_t index) {
  assert(ann);

  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (index < _annotations.size()) {
    *ann = *_annotations[index]; // clone
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
      [](Annotation *a, uint64_t val) { return a->end_sample() <= val; });

  size_t start_idx = std::distance(_annotations.begin(), it);

  size_t end_idx = start_idx;
  for (; end_idx < _annotations.size(); ++end_idx) {
    if (_annotations[end_idx]->start_sample() > end_sample)
      break;
  }

  return {start_idx, end_idx};
}

size_t RowData::find_index_after_sample(uint64_t sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  auto it = std::upper_bound(
      _annotations.begin(), _annotations.end(), sample,
      [](uint64_t val, Annotation *a) { return val < a->start_sample(); });

  return std::distance(_annotations.begin(), it);
}

const Annotation *RowData::annotation_at(size_t index) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (index < _annotations.size())
    return _annotations[index];
  return nullptr;
}

const Annotation *RowData::get_first_annotation_ending_after(uint64_t sample) {
  std::shared_lock<std::shared_mutex> lock(_visitor_mutex);

  if (_annotations.empty())
    return nullptr;

  auto it = std::lower_bound(
      _annotations.begin(), _annotations.end(), sample,
      [](Annotation *a, uint64_t val) { return a->end_sample() <= val; });

  if (it != _annotations.end())
    return *it;
  return nullptr;
}

} // namespace decode
} // namespace data
} // namespace pv
