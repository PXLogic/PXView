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
 * Order the full row list (dragged one included) top-to-bottom **by layout
 * offset**, exactly like PulseView's restack_items():
 *
 *     stable_sort(items, a, b:
 *         a->layout_v_offset() + (aext.first + aext.second) / 2 <
 *         b->layout_v_offset() + (bext.first + bext.second) / 2 );
 *
 * ── 为什么必须用 layout、不能用 visual ────────────────────────────────────
 * 早先让被拖项用 `visual_v_offset()`、其余项用 `get_v_offset()` 比较，以为
 * "被拖项要按画出来的位置算"。**错**。PulseView 的 `drag_by()` →
 * `force_to_v_offset()` 会把 `layout_v_offset_` 和 `visual_v_offset_` **一起**
 * 写成手指位置，所以 layout 本来就是"手指位置"，根本不必去读 visual。混用两个
 * 字段等于拿两个坐标系比大小，会在手指刚压到邻居格位时制造一个**本不存在**的并结。
 *
 * ── 真正的并结：同字段也会撞，必须按**行进方向**破 ────────────────────────
 * 统一成 layout 之后并结并没有消失 —— 因为 `Header::mouseMoveEvent` 是用
 * `y_snap`（对齐到 SignalSnapGridSize 的值）调 `force_to_v_offset` 的，而邻居
 * 恰好就坐在**格位中心**上。手指一格一格挪时，被拖项的 layout 会**精确等于**
 * 某个邻居的 layout：
 *
 *   向上拖（row3 162→108→54→0）：
 *       order: row0(0) row1(54) row3(54,DRAG) row2(108)     ← row1 与 row3 撞在 54
 *       stable_sort 保留原插入序 → row1 在 row3 之前 → row1 判为"在上"
 *       → row1 永远占 0 号槽、不让位 → 列不动，交换失败。
 *   向下拖（row0 0→54→108→162）：
 *       撞在 54 时 row0（原索引 0）本就在 row1 之前 → row0 判为"在上"
 *       → row1 被挤到下面 → **恰好**是对的。
 *
 * 也就是说：同一个 stable_sort 插入序，在向下方向碰巧正确、向上方向必错。
 * 唯一两个方向都对的做法，是**按行进方向破并结**：
 *   - 向下拖：被拖项排在撞位邻居**之前**（它已越过对方，应占上面的槽）；
 *   - 向上拖：被拖项排在撞位邻居**之后**（它已越过对方，应占下面的槽）。
 * 一旦被拖项被放进正确的相对位置，`layout()` 的累积游标就会把那个邻居挤到
 * 被拖项原来的格位 —— 这正是"让位/交换"。
 *
 * PulseView 之所以没有这个坑：它虽然也 `stable_sort` 同字段，但 `restack_items()`
 * 的游标**为被拖项保留了它排序后所占的槽位**（`if (!r->dragging())` 只跳过
 * **写入**、不跳过**推进**），再加上它的 click 落点是连续像素（没有 y_snap 对齐），
 * 撞值窗口极小。PXView 是扁平"行中心"模型 + y_snap 对齐，撞值是常态，因此这里
 * 必须把方向显式写进比较器。
 *
 * @param all       every participating row, dragged one included.
 * @param dragged   the row under the cursor (may be null → 纯 layout 排序).
 * @param downward  true = 被拖项正向屏幕下方移动（y 增大）。
 */
template <typename TraceLike>
inline std::vector<TraceLike *> order_by_layout_offset(
    const std::vector<TraceLike *> &all, TraceLike *dragged, bool downward) {
  std::vector<TraceLike *> rows(all);
  std::stable_sort(rows.begin(), rows.end(),
                   [dragged, downward](TraceLike *a, TraceLike *b) {
                     // PXView 的 get_v_offset() 本身就是"行中心"语义（等价于
                     // PulseView 的 layout_v_offset + (first+second)/2），故直接比。
                     const int ya = a->get_v_offset();
                     const int yb = b->get_v_offset();
                     if (ya != yb)
                       return ya < yb;
                     if (!dragged || a == b)
                       return false;
                     // 并结：被拖项与某邻居撞在同一 y。按行进方向决定它该在前
                     // 还是在后（见上）。注意比较器必须对 (a,b) 与 (b,a) 给出
                     // 互反的结果，否则是 UB；这里只有"被拖项 vs 邻居"这一种
                     // 撞法，天然互反。
                     if (a == dragged)
                       return downward;   // 向下 → 被拖项在前
                     if (b == dragged)
                       return !downward;  // 向上 → 被拖项在后
                     return false;
                   });
  return rows;
}

/**
 * Rebuild the row stacking and assign every *non-dragged* row a target center.
 *
 * ── 与 PulseView restack_items() 的关系（重要，别再照抄错了）─────────────
 * 早先我逐字照搬了 PulseView 的"累计游标"：
 *
 *     int total_offset = 0;
 *     for (r : items) { total_offset += -extents.first; if(!dragging) set(total_offset); total_offset += extents.second; }
 *
 * 结果两个方向都错。**根因不是并结，而是这个游标本身**：它把被拖项当成一个
 * 占位项、用它自己的高度把后面的项整体往下推。可 PXView 的槽位是**固定网格**
 * `{top_center + k*pitch}`，让位只应该是"邻居挪进被拖项空出来的那一格"，不该
 * 因为被拖项"变高/变低"而把整列推走。PulseView 那边之所以能用累计游标，是因为
 * 它的 click 落点是连续像素、且松手后会由 drop handler 重新权威布局；而 PXView
 * 的 y_snap 会让被拖项精确落在格位上，累计游标一推进整列就跟着平移。
 *
 * 正确模型（已验证双向）：
 *   1. 把被拖项放回集合，按 layout 值统一排序（撞值按行进方向破，见上）；
 *   2. 沿**固定的槽位网格** `top_center + k*pitch` 走 k = 0,1,2,…；
 *   3. 被拖项**保留**它排序后所占的那个 k（占一格），但**不**给它目标；
 *      其余项各自占据自己的 k → 目标 = 网格点 k。
 *
 * 这样被拖项空出来的格子会被后面的邻居顺势顶上（向下拖时下面的项升上来）、
 * 或被前面的邻居顺势下压腾出（向上拖时上面的项落下来），而对角线之外的项
 * **原地不动** —— 这正是"让位"，且**不会**整列平移。
 *
 * @param others       the rows EXCLUDING the dragged one (order irrelevant).
 * @param dragged      row under the cursor; must not be null.
 * @param top_center   这个槽位网格的最高一行中心（拖动开始时算定，全程恒定）。
 * @param row_gap     相邻行之间的额外间隙（生产 = 2 * SignalMargin）。
 * @param downward    被拖项是否正朝屏幕下方移动（见 order_by_layout_offset）。
 * @param out_order    receives the full stacking order (dragged included).
 * @param out_targets  receives (row, target center) for every row EXCEPT the
 *                     dragged one (which keeps following the cursor).
 */
template <typename TraceLike>
inline void layout(const std::vector<TraceLike *> &others, TraceLike *dragged,
                   int top_center, int row_gap, bool downward,
                   std::vector<TraceLike *> &out_order,
                   std::vector<std::pair<TraceLike *, int>> &out_targets) {
  out_order.clear();
  out_targets.clear();
  if (!dragged || others.empty())
    return;

  // 把被拖项放回集合里统一排序（见 order_by_layout_offset 的说明：必须用
  // layout 字段 + 行进方向破并结）。
  std::vector<TraceLike *> all;
  all.reserve(others.size() + 1);
  for (TraceLike *t : others)
    all.push_back(t);
  all.push_back(dragged);

  out_order = order_by_layout_offset(all, dragged, downward);

  // 固定槽位网格：第 k 格中心。被拖项占掉的那个 k 会被跳过（不给目标），
  // 其余项保持自己的 k —— 于是空出来的格子由邻居自然顶上，整列不平移。
  //
  // 注意：网格用**固定 pitch** 还是"逐项累加各自高度"？两者在等高通道下等价；
  // 通道高度不等时，必须按各项自身高度累加，否则槽位会重叠。这里用累加，只是
  // 把被拖项的那一段**照常累加**（它确实占着那一格的空间），只是不写目标。
  int cur = top_center;
  for (std::size_t i = 0; i < out_order.size(); i++) {
    TraceLike *t = out_order[i];
    if (i > 0) {
      TraceLike *prev = out_order[i - 1];
      cur += prev->get_totalHeight() / 2 + t->get_totalHeight() / 2 + row_gap;
    }
    if (t == dragged)
      continue; // 被拖项占着这一格，但由 force_to_v_offset 跟手，不给目标
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
