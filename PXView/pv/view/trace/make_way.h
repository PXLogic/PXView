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

#ifndef PXVIEW_PV_VIEW_TRACE_MAKEWAY_H
#define PXVIEW_PV_VIEW_TRACE_MAKEWAY_H

#include <algorithm>
#include <climits>
#include <cstddef>
#include <utility>
#include <vector>

namespace pv {
namespace view {

/**
 * Pure geometry for the channel drag "make-way" preview.
 *
 * Kept header-only and free of any View/QWidget dependency so it can be unit
 * tested directly (see tests/qtest/view/test_v_offset_animation.cpp).
 *
 * @tparam TraceLike  anything exposing visual_v_offset(), get_v_offset() and
 *                    get_totalHeight() — in production this is view::Trace.
 */
namespace make_way {

/**
 * Immutable row geometry captured when a drag begins.
 *
 * `group_id` controls the visual separator inserted by the production layout.
 * `block_id` identifies a hard drag block. Rows with the same contiguous
 * block id move together; callers give independently draggable rows unique
 * negative block ids. Hidden rows remain in `rows` so committing a drag does
 * not scramble their logical order, but they consume no geometry while hidden.
 */
template <typename Item>
struct DragRow {
  Item item{};
  int center = INT_MAX;
  int height = 0;
  int group_id = -1;
  int block_id = -1;
  bool visible = false;
};

template <typename Item>
struct DragSnapshot {
  std::vector<DragRow<Item>> rows;
  Item dragged{};
  int dragged_start_center = INT_MAX;
  int content_top = 0;
  int margin = 0;
  int group_gap = 0;

  bool valid() const {
    return dragged != Item{} && dragged_start_center != INT_MAX &&
           !rows.empty();
  }
};

template <typename Item>
struct DragLayout {
  std::vector<Item> order;
  std::vector<std::pair<Item, int>> targets;
};

namespace detail {

template <typename Item>
struct DragBlock {
  int group_id = -1;
  std::vector<DragRow<Item>> rows;
  int top = INT_MAX;
  int bottom = INT_MIN;
  int original_index = -1;

  bool visible() const { return top != INT_MAX && bottom != INT_MIN; }
  int center() const { return visible() ? top + (bottom - top) / 2 : 0; }
};

template <typename Item>
inline void update_block_extents(DragBlock<Item> &block) {
  block.top = INT_MAX;
  block.bottom = INT_MIN;
  for (const auto &row : block.rows) {
    if (!row.visible || row.center == INT_MAX || row.height <= 0)
      continue;
    block.top = std::min(block.top, row.center - row.height / 2);
    block.bottom = std::max(block.bottom,
                            row.center + (row.height + 1) / 2);
  }
}

template <typename Item>
inline void reorder_dragged_inside_block(DragBlock<Item> &block,
                                         Item dragged,
                                         int hand_center,
                                         int dragged_start_center) {
  auto dragged_it = std::find_if(
      block.rows.begin(), block.rows.end(),
      [dragged](const DragRow<Item> &row) { return row.item == dragged; });
  if (dragged_it == block.rows.end())
    return;

  const DragRow<Item> dragged_row = *dragged_it;
  block.rows.erase(dragged_it);

  const bool moving_down = hand_center >= dragged_start_center;
  std::size_t visible_before = 0;
  for (const auto &row : block.rows) {
    if (!row.visible || row.center == INT_MAX)
      continue;
    if (row.center < hand_center ||
        (moving_down && row.center == hand_center))
      visible_before++;
  }

  std::size_t seen_visible = 0;
  auto insert_at = block.rows.end();
  for (auto it = block.rows.begin(); it != block.rows.end(); ++it) {
    if (it->visible && it->center != INT_MAX) {
      if (seen_visible == visible_before) {
        insert_at = it;
        break;
      }
      seen_visible++;
    }
  }
  block.rows.insert(insert_at, dragged_row);
}

} // namespace detail

/**
 * Compute a group-aware drag preview from immutable press-time geometry.
 *
 * Unlike the legacy fixed-pitch helper below, this function preserves each
 * row's real height and the production group gap. A group is a hard block:
 * while the pointer remains inside its original block only the dragged row is
 * reordered; once it leaves that block, the whole block moves across sibling
 * blocks. This matches the View invariant that group view indices are
 * contiguous and prevents preview/drop semantics from disagreeing.
 */
template <typename Item>
inline DragLayout<Item> layout_snapshot(const DragSnapshot<Item> &snapshot,
                                        int hand_center) {
  DragLayout<Item> result;
  if (!snapshot.valid())
    return result;

  std::vector<detail::DragBlock<Item>> blocks;
  for (const auto &row : snapshot.rows) {
    if (blocks.empty() || blocks.back().group_id != row.block_id) {
      detail::DragBlock<Item> block;
      block.group_id = row.block_id;
      block.original_index = static_cast<int>(blocks.size());
      block.rows.push_back(row);
      blocks.push_back(std::move(block));
    } else {
      blocks.back().rows.push_back(row);
    }
  }

  for (auto &block : blocks)
    detail::update_block_extents(block);

  auto source_it = std::find_if(
      blocks.begin(), blocks.end(), [&snapshot](const auto &block) {
        return std::any_of(block.rows.begin(), block.rows.end(),
                           [&snapshot](const auto &row) {
                             return row.item == snapshot.dragged;
                           });
      });
  if (source_it == blocks.end())
    return result;

  const int source_index = static_cast<int>(source_it - blocks.begin());
  const int source_top = source_it->top;
  const int source_bottom = source_it->bottom;
  const bool inside_source = source_it->visible() &&
                             hand_center >= source_top &&
                             hand_center <= source_bottom;

  if (inside_source || blocks.size() == 1) {
    detail::reorder_dragged_inside_block(*source_it, snapshot.dragged,
                                         hand_center,
                                         snapshot.dragged_start_center);
  } else if (blocks.size() > 1 && source_it->visible()) {
    detail::DragBlock<Item> source = std::move(*source_it);
    blocks.erase(source_it);

    const int delta = hand_center - snapshot.dragged_start_center;
    const int moving_center = source.center() + delta;
    const bool moving_down = delta >= 0;
    std::size_t insert_index = 0;
    for (const auto &block : blocks) {
      if (!block.visible()) {
        if (block.original_index < source_index)
          insert_index++;
        continue;
      }
      if (block.center() < moving_center ||
          (moving_down && block.center() == moving_center))
        insert_index++;
    }
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(insert_index),
                  std::move(source));
  }

  result.order.reserve(snapshot.rows.size());
  result.targets.reserve(snapshot.rows.size());

  int cursor = snapshot.content_top;
  bool have_visible = false;
