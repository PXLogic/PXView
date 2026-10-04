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

#ifndef PXVIEW_PV_VIEW_VIEW_INDEX_INVARIANTS_H
#define PXVIEW_PV_VIEW_VIEW_INDEX_INVARIANTS_H

#include <vector>

namespace pv {
namespace view {

class Trace;
struct SignalGroup;

// ---------------------------------------------------------------------------
// view_index / 信号分组不变量校验 —— 自由函数（零 View/QWidget 依赖，可单测）
// ---------------------------------------------------------------------------
// 核心判定只吃**序号序列**，与 Trace/SignalGroup 解耦，故可独立单测而无需
// 构造 Trace（QObject + 完整渲染依赖链），也无需 include iview_delegates.h
// （它会经 pulse_analyzer → logicsnapshot 拉进 libsigrok/glib 整条重依赖——
// 这正是把"纯逻辑校验"和"渲染生态类型"分开的原因）。
// 面向 Trace/SignalGroup 的重载在 view_index_invariants_adapters.cpp 定义。

// 不变量 1：序号必须构成 0..n-1 的排列（无重复、无空洞）。容器顺序无关。
bool view_indices_are_permutation(std::vector<int> indices);

// 不变量 2：每个 group 内的序号必须连续。groups/group_ids 并行。
// 【契约】groups 由 compute_signal_groups() 从 view_index 派生；本校验必须在
// 该函数【之后】调用，否则读到的是上一轮的陈旧分组（假告警）。
bool group_view_indices_are_contiguous(
    const std::vector<std::vector<int>> &groups,
    std::vector<int> group_ids);

// ---- Trace* / SignalGroup 适配层（生产路径用；定义在 adapters TU）----

// 不变量 1：从 Trace 列表取 view_index 后判定。
bool view_index_is_permutation(const std::vector<Trace *> &traces);

// 不变量 2：从 SignalGroup 列表取 view_index 后判定。
bool signal_groups_are_contiguous(const std::vector<SignalGroup> &groups);

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_INDEX_INVARIANTS_H
