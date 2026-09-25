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
 * Compute the insertion slot for the dragged row.
 *
 * The probe is the dragged row's *painted* center (visual offset), compared
 * against each other row's settled center. Rows are assumed to already be
 * sorted top-to-bottom by their layout offset.
 *
 * @return index in [0, others.size()]; others.size() means "append last".
 */
template <typename TraceLike>
inline std::size_t insertion_slot(const std::vector<TraceLike *> &others,
                                  int dragged_visual_y) {
  for (std::size_t i = 0; i < others.size(); i++) {
    if (others[i]->get_v_offset() > dragged_visual_y)
      return i;
  }
  return others.size();
}

/**
 * Rebuild the row order with the dragged row inserted at its slot, then assign
 * each *non-dragged* row a target center, laid out top-to-bottom in sequence.
 *
 * This mirrors PulseView's TraceTreeItemOwner::restack_items(): the list is
 * walked once with a running cursor, and the dragged row occupies a slot
 * without being repositioned (it keeps following the cursor). Because the
 * remaining rows are packed in order, the row that the dragged one passes is
 * pulled into the vacated slot — a genuine swap. A layout that instead spreads
 * the others symmetrically around the dragged row cannot swap: it translates
 * the whole column by one row.
 *
 * @param others       visible rows EXCLUDING the dragged one, sorted by
 *                     layout offset (top to bottom).
 * @param dragged      row under the cursor; must not be null.
 * @param top_center   row center assigned to the first entry of the result.
 * @param row_gap      extra vertical gap between two rows (mirrors
 *                     2 * SignalMargin).
 * @param out_order    receives the full ordering (dragged inserted).
 * @param out_targets  receives (row, target center) for every row EXCEPT the
 *                     dragged one.
 */
template <typename TraceLike>
inline void layout(const std::vector<TraceLike *> &others, TraceLike *dragged,
                   int top_center, int row_gap,
                   std::vector<TraceLike *> &out_order,
                   std::vector<std::pair<TraceLike *, int>> &out_targets) {
  out_order.clear();
  out_targets.clear();
  if (!dragged || others.empty())
    return;

  const std::size_t insert_at =
      insertion_slot(others, dragged->visual_v_offset());

  out_order.reserve(others.size() + 1);
  for (std::size_t i = 0; i < insert_at; i++)
    out_order.push_back(others[i]);
  out_order.push_back(dragged);
  for (std::size_t i = insert_at; i < others.size(); i++)
    out_order.push_back(others[i]);

  int cur = top_center;
  for (std::size_t i = 0; i < out_order.size(); i++) {
    TraceLike *t = out_order[i];
    if (i > 0) {
      TraceLike *prev = out_order[i - 1];
      cur += prev->get_totalHeight() / 2 + t->get_totalHeight() / 2 + row_gap;
    }
    if (t == dragged)
      continue; // 被拖项继续跟手，不参与动画
    out_targets.emplace_back(t, cur);
  }
}

/**
 * Top anchor for the layout: the smallest settled center among the rows.
 *
 * Using the column's own top (rather than the dragged row's current position)
 * is what keeps the exchange in place — anchoring on the dragged row would
 * translate the entire column along with the cursor.
 *
 * NOTE: this is only a *fallback* for callers with no remembered anchor. It is
 * NOT stable across a drag: once the dragged row leaves/enters the top slot the
 * minimum jumps by one row pitch, which translates the whole column. Drag code
 * must therefore capture the column top once (at drag start) with
 * anchor_from_rows(all_rows) and feed it to layout() via a constant. Use
 * resolve_anchor() to express that preference in one place.
 */
template <typename TraceLike>
inline int top_center(const std::vector<TraceLike *> &rows) {
  int top = INT_MAX;
  for (TraceLike *t : rows)
    top = std::min(top, t->get_v_offset());
  return top;
}

/**
 * Column top over *all* visible rows, including the one about to be dragged.
 *
 * Must be evaluated BEFORE the drag mutates any layout offset. The result is
 * the fixed slot-grid origin for the whole drag.
 */
template <typename TraceLike>
inline int anchor_from_rows(const std::vector<TraceLike *> &all_rows) {
  int top = INT_MAX;
  for (TraceLike *t : all_rows) {
    const int y = t->get_v_offset();
    if (y != INT_MAX) // INT_MAX = "not laid out yet", not a real coordinate
      top = std::min(top, y);
  }
  return top;
}

/**
 * Pick the anchor to lay out with.
 *
 * Prefers the caller-remembered `remembered` anchor (captured at drag start,
 * constant for the whole drag). Only when the caller has none (INT_MAX) — a
 * non-drag caller — does it fall back to deriving one from the current layout,
 * which is acceptable because nothing is moving in that case.
 *
 * This function exists so the "never re-derive the anchor mid-drag" rule has a
 * single, unit-testable home instead of being inlined in the drag handler.
 */
template <typename TraceLike>
inline int resolve_anchor(int remembered,
                          const std::vector<TraceLike *> &all_rows) {
  if (remembered != INT_MAX)
    return remembered;
  return anchor_from_rows(all_rows);
}

} // namespace make_way
} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_TRACE_MAKEWAY_H
