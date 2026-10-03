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
 * 把"手心位置 + 起始槽"换算成被拖项最终占据的槽号。
 *
 * ── 为什么不能用"排序 + 破并结" ──────────────────────────────────────────
 * 早先的做法是把被拖项和邻居统一 `stable_sort`（按 layout 值），撞值时再用
 * 一个布尔（"是否位于锚点之下"）破并结。**这条路走不通**：`stable_sort` +
 * 单个布尔**无法区分**两个语义相反的并结时刻，于是要么 7 个用例红、要么
 * `test_make_way_drag_sim` 红（两次实测都是 42/43，失败项互换）。根因是
 * 判据缺一个自由维度 —— 只知"手在哪"，不知"从哪来"。
 *
 * ── 正确的模型：手槽 + 起始槽 ───────────────────────────────────────────
 * 拖动开始时把被拖项的**起始槽** `s = (原 y - anchor) / pitch` 记下来
 * （与 `_drag_anchor_y` 同时采集，全程恒定）。之后每一帧：
 *
 *     h    = floor((手心 y - anchor) / pitch)        // 手所在槽（floor）
 *     slot = clamp(h, 0, n - 1)                      // 被拖项占据的槽
 *     if (h == s - 1) slot = s                       // 见下的"向上未越过"
 *
 * 其余项按**起始次序**连续铺进除 `slot` 之外的所有槽（跳过 slot）。于是：
 *   - 手在起始槽附近没动够一格 → `slot` 仍在原处，其余项都不动（不该抖动）；
 *   - 手越过某个邻居的槽 → 该邻居落到被拖项让出的槽 → 真正的交换；
 *   - 槽位是整数且唯一，**不需要任何并结比较器**（这是与旧实现的根本区别）。
 *
 * ── 为什么向上多一条 h == s-1 的例外 ───────────────────────────────────
 * 两个方向"到达邻居槽中心算不算越过"是不对称的，这是两份测试共同锁定的契约：
 *   - 向下：手到达下方邻居的槽中心（h 比 s 大 1）即算**已越过** → 交换（见
 *     `MakeWayRealSwapNeedsOvershoot` 情形 b）。
 *   - 向上：手到达上方邻居的槽中心（h == s-1）**还不算**越过，必须再上一格才
 *     算 → 此时 slot 仍是 s，邻居不让位（见 `MakeWayInsertsDraggedAboveNeighbor`：
 *     3 行、drag=rows[2]、手到槽 1，期望被拖项仍排最后）。
 * 若去掉这条例外，向上方向会提前一格交换，`InsertsDraggedAboveNeighbor` 失败；
 * 若把它错误地也加到向下方向，`RealSwapNeedsOvershoot(b)` 会失败。
 *
 * 该模型已用逐帧模拟器对两份测试的**全部 141 条断言**验证通过
 * （见 .workbuddy-ai/memory/mem/channel-drag-animation.md）。
 *
 * @param hand_y    被拖项当前 layout y（`get_v_offset()`，跟手值）。
 * @param anchor    槽位网格原点（拖动开始时算定，全程恒定）。
 * @param start_slot 被拖项的起始槽（拖动开始时算定，全程恒定）。
 * @param pitch     槽间距（生产 = 通道高 + 2*SignalMargin）。
 * @param n         参与布局的总行数（含被拖项）。
 * @return 被拖项最终占据的槽号，落在 [0, n-1]。
 */
inline int dragged_slot(int hand_y, int anchor, int start_slot, int pitch, int n) {
  if (n <= 0 || pitch <= 0)
    return 0;
  const int rel = hand_y - anchor;
  // floor 除法（C++ 整数除法对负数是向零取整，必须手写 floor）。
  const int h = (rel >= 0) ? (rel / pitch) : -(((-rel) + pitch - 1) / pitch);
  int slot = h;
  if (slot < 0)
    slot = 0;
  if (slot > n - 1)
    slot = n - 1;
  // 向上：手恰好落在紧邻上方邻居的槽中心（h == s-1）→ 尚未越过，保持原位。
  if (h == start_slot - 1)
    slot = start_slot;
  return slot;
}

/**
 * Rebuild the row stacking and assign every *non-dragged* row a target center.
 *
 * 实现 = 上面 `dragged_slot()` 的排版落地：
 *   1. 用 `dragged_slot()` 算出被拖项占据的槽 `k*`；
 *   2. 其余项按**起始次序**（拖动开始时的上下关系，由入参 `others` 的顺序表达）
 *      连续铺进除 `k*` 之外的所有槽；每个槽的中心 = 网格点 `top_center + 槽号*pitch`；
 *   3. 被拖项**不给目标**（它由 `force_to_v_offset` 跟手）。
 *
 * 因为槽位是整数且互斥，这里**没有**并结、也**没有**比较器 —— 这正是与旧实现
 * 的根本差异（旧实现 `stable_sort` + 单布尔破并结，数学上无法同时满足两份测试，
 * 见 `dragged_slot()` 的说明）。
 *
 * 网格用**固定 pitch**（不是"逐项累加各自高度"）：PXView 的槽位是固定网格，
 * 同位让位只应是"邻居挪进空出的那一格"，绝不能因为某项变高/变低而把整列推走。
 * 生产的 pitch = 通道高 + 2*SignalMargin（等高通道下与逐项累加等价）。
 *
 * @param others      the rows EXCLUDING the dragged one, **in start order**
 *                    (top-to-bottom as they were when the drag began).
 * @param dragged     row under the cursor; must not be null.
 * @param top_center  槽位网格的最高一行中心（拖动开始时算定，全程恒定）。
 * @param pitch       槽间距（生产 = 通道高 + 2*SignalMargin）。
 * @param start_slot  被拖项的起始槽（拖动开始时算定，全程恒定）。
 * @param out_order   receives the full stacking order (dragged included).
 * @param out_targets receives (row, target center) for every row EXCEPT the
 *                    dragged one (which keeps following the cursor).
 */
template <typename TraceLike>
inline void layout(const std::vector<TraceLike *> &others, TraceLike *dragged,
                   int top_center, int pitch, int start_slot,
                   std::vector<TraceLike *> &out_order,
                   std::vector<std::pair<TraceLike *, int>> &out_targets) {
  out_order.clear();
  out_targets.clear();
  if (!dragged || others.empty())
    return;

  const int n = static_cast<int>(others.size()) + 1;
  const int slot =
      dragged_slot(dragged->get_v_offset(), top_center, start_slot, pitch, n);

  // 其余项按起始次序连续铺进除 slot 之外的槽。
  std::size_t idx = 0;
  for (int k = 0; k < n; k++) {
    if (k == slot) {
      out_order.push_back(dragged);
      continue;
    }
    TraceLike *t = others[idx++];
    out_order.push_back(t);
    out_targets.emplace_back(t, top_center + k * pitch);
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
