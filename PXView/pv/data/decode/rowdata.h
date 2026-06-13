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

#include <shared_mutex>
#include <utility>
#include <vector>

#include "annotation.h"

namespace pv {
namespace data {
namespace decode {

class RowData {
public:
  RowData();
  ~RowData();

public:
  uint64_t get_max_sample();

  uint64_t get_max_annotation();
  uint64_t get_min_annotation();

  uint64_t get_annotation_index(uint64_t start_sample);

  bool push_annotation(Annotation *a);

  inline uint64_t get_annotation_size() { return _item_count; }
  inline uint64_t get_annotation_capacity() { return _annotations.capacity(); }

  bool get_annotation(pv::data::decode::Annotation *ann, uint64_t index);

  /**
   * Extracts sorted annotations between two period into a vector.
   */
  void get_annotation_subset(std::vector<pv::data::decode::Annotation *> &dest,
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
  std::vector<Annotation *> _annotations;
  std::shared_mutex _visitor_mutex;
};

} // namespace decode
} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODE_ROWDATA_H
