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

#include <deque>
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
  const Annotation *annotation_at(size_t index);
  const Annotation *get_first_annotation_ending_after(uint64_t sample);

private:
  uint64_t _max_annotation;
  uint64_t _min_annotation;
  uint64_t _item_count;
  // P2-7 fix: deque<Annotation> value storage — no pointer ownership,
  // no manual delete. deque keeps element pointers stable across
  // push_back (unlike vector which may reallocate).
  std::deque<Annotation> _annotations;
  std::shared_mutex _visitor_mutex;
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODE_ROWDATA_H
