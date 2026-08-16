/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
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

#ifndef PXVIEW_PV_VIEW_CHANGE_EVENT_H
#define PXVIEW_PV_VIEW_CHANGE_EVENT_H

#include <set>

namespace pv {
namespace view {

/**
 * ChangeEventKind — 信号列表变化的纯分类结果。
 *
 * 从 SignalFactory::SignalChangeEvent 抽取为独立的纯枚举 + 纯函数，
 * 使复杂的变化判定算法可在不链接 View 层具体类（Signal/SignalModel）
 * 的情况下隔离单测。
 */
enum class ChangeEventKind {
    Added,
    Removed,
    Modified,
    AllReplaced
};

/**
 * compute_change_event_pure — 纯函数：根据两组通道索引集合与模型指针
 * 集合判定信号列表变化类型。不依赖任何 Signal/SignalModel 具体类，
 * 仅需调用方提取索引与指针。
 *
 * @param current_indices 当前 Signal 的通道索引集合。
 * @param model_indices   新 SignalModel 的通道索引集合。
 * @param model_ptrs      新 SignalModel 的裸指针集合（用于指针同一性检查：
 *                        索引相同但对象整体重建时须返回 AllReplaced）。
 * @return 变化分类结果。
 */
inline ChangeEventKind compute_change_event_pure(
    const std::set<int> &current_indices,
    const std::set<int> &model_indices,
    const std::set<const void *> &model_ptrs)
{
    // Empty current + non-empty models → first creation → AllReplaced
    if (current_indices.empty() && !model_indices.empty())
        return ChangeEventKind::AllReplaced;

    // Non-empty current + empty models → all removed → AllReplaced
    if (!current_indices.empty() && model_indices.empty())
        return ChangeEventKind::AllReplaced;

    // Both empty → no change, but Modified is safe fallback
    if (current_indices.empty() && model_indices.empty())
        return ChangeEventKind::Modified;

    // Check if index sets are identical → Modified (properties may have
    // changed), unless Core rebuilt the SignalModels wholesale — detected by
    // pointer identity: any current index whose model pointer is not in the
    // new model_ptrs set means the View must fully rebuild (AllReplaced) to
    // avoid a stale _model (UAF when handlers later access it).
    if (current_indices == model_indices)
        return ChangeEventKind::Modified;

    // Check if models is pure superset of current → Added
    // (all current indices exist in models, and models has extra indices)
    bool all_current_in_models = true;
    bool some_new_not_in_current = false;
    for (int idx : current_indices) {
        if (model_indices.find(idx) == model_indices.end()) {
            all_current_in_models = false;
            break;
        }
    }
    for (int idx : model_indices) {
        if (current_indices.find(idx) == current_indices.end()) {
            some_new_not_in_current = true;
            break;
        }
    }
    if (all_current_in_models && some_new_not_in_current)
        return ChangeEventKind::Added;

    // Check if current is pure superset of models → Removed
    // (all model indices exist in current, and current has extra indices)
    bool all_models_in_current = true;
    bool some_current_not_in_models = false;
    for (int idx : model_indices) {
        if (current_indices.find(idx) == current_indices.end()) {
            all_models_in_current = false;
            break;
        }
    }
    for (int idx : current_indices) {
        if (model_indices.find(idx) == model_indices.end()) {
            some_current_not_in_models = true;
            break;
        }
    }
    if (all_models_in_current && some_current_not_in_models)
        return ChangeEventKind::Removed;

    // Mixed: both additions and removals → conservative fallback to AllReplaced
    return ChangeEventKind::AllReplaced;
}

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_CHANGE_EVENT_H
