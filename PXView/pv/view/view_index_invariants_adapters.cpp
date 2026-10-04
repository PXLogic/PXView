/*
 * view_index_invariants_adapters.cpp — Trace* / SignalGroup 适配层
 *
 * 把 ViewSignalSync 的两类实参（std::vector<Trace*> / std::vector<SignalGroup>）
 * 转成 widget-free 核心判定所吃的 std::vector<int>。
 *
 * 单独成 TU 的原因：这里需要 Trace::get_view_index()（QObject 派生 + 完整
 * 渲染依赖链），而核心判定 view_index_invariants.cpp 保持零依赖以便单测。
 * 两者分开后，测试目标可以只编译核心 TU。
 */

#include "pv/view/view_index_invariants.h"

#include <utility>

// 完整类型：SignalGroup（按值遍历 traces）+ Trace::get_view_index()。
// 这两个头会拉进 libsigrok/glib，故本 TU 与核心校验 TU 分开编译。
#include "pv/view/iview_delegates.h"
#include "pv/view/trace/trace.h"

namespace pv {
namespace view {

bool view_index_is_permutation(const std::vector<Trace *> &traces) {
  std::vector<int> indices;
  indices.reserve(traces.size());
  for (auto t : traces) {
    if (t)
      indices.push_back(t->get_view_index());
  }
  return view_indices_are_permutation(std::move(indices));
}

bool signal_groups_are_contiguous(const std::vector<SignalGroup> &groups) {
  std::vector<std::vector<int>> indices;
  std::vector<int> ids;
  indices.reserve(groups.size());
  ids.reserve(groups.size());
  for (const auto &group : groups) {
    std::vector<int> gi;
    gi.reserve(group.traces.size());
    for (auto *t : group.traces) {
      if (t)
        gi.push_back(t->get_view_index());
    }
    indices.push_back(std::move(gi));
    ids.push_back(group.group_id);
  }
  return group_view_indices_are_contiguous(indices, std::move(ids));
}

} // namespace view
} // namespace pv
