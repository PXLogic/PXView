/*
 * view_index_invariants.cpp — view_index / 信号分组不变量校验（widget-free）
 *
 * 这两个校验从 ViewSignalSync 抽出来，成为**不依赖 View/QWidget 的自由函数**，
 * 从而可以独立单测（见 tests/qtest/view/test_view_index_invariants.cpp）。
 *
 * 设计取舍：核心判定只吃**序号序列**（`std::vector<int>`），不吃 `Trace*`。
 * 这样测试无需构造 Trace（QObject + 完整渲染依赖链），也不必用 reinterpret_cast
 * 的布局假设去伪造 Trace —— 后者在字段偏移变化时会静默读错内存。
 * 本 TU 因此**不含**任何 Trace/SignalGroup 依赖，可被轻量测试目标直接编译。
 * 面向 Trace* / SignalGroup 的适配层在 view_index_invariants_adapters.cpp。
 *
 * 为什么需要显式校验：`SignalGroup` 是从 `view_index` 派生的**第二次独立遍历**
 * （compute_signal_groups），两者靠"normalize_view_indices() 先于
 * compute_signal_groups() 调用"这个**约定**保持一致，而不是靠类型系统保证。
 * 历史踩坑：重复 view_index → std::sort 不稳定 → 通道顺序错乱（静默、不崩）。
 *
 * 【调用契约】不变量 2 依赖 SignalGroup 列表，必须在该列表被
 * compute_signal_groups() 重建【之后】校验；否则读到的是上一轮陈旧分组，
 * 会刷出 "group N 内 view_index 不连续" 的假告警（序号本身完全合法）。
 */

#include "pv/view/view_index_invariants.h"

#include <algorithm>
#include <utility>

#include "pv/base/log.h" // pxv_assert

namespace pv {
namespace view {

bool view_indices_are_permutation(std::vector<int> indices) {
  // 排序后应与 0..n-1 逐位相等。刻意不依赖入参顺序。
  std::sort(indices.begin(), indices.end());
  for (size_t i = 0; i < indices.size(); i++) {
    if (indices[i] != static_cast<int>(i)) {
      pxv_assert(false,
                 "validate_view_index_invariants: view_index 不是 0..%d 的排列"
                 "（第 %zu 位 = %d，期望 %zu）。"
                 "normalize_view_indices() 与下游派生态已分叉。",
                 static_cast<int>(indices.size()) - 1, i, indices[i], i);
      return false;
    }
  }
  return true;
}

bool group_view_indices_are_contiguous(
    const std::vector<std::vector<int>> &groups, std::vector<int> group_ids) {
  for (size_t g = 0; g < groups.size(); g++) {
    if (groups[g].size() < 2)
      continue;

    std::vector<int> gi = groups[g];
    std::sort(gi.begin(), gi.end());
    const int gid =
        (g < group_ids.size()) ? group_ids[g] : static_cast<int>(g);

    for (size_t i = 1; i < gi.size(); i++) {
      if (gi[i] != gi[i - 1] + 1) {
        pxv_assert(false,
                   "validate_view_index_invariants: group %d 内 view_index 不连续"
                   "（%d 后跟 %d）。",
                   gid, gi[i - 1], gi[i]);
        return false;
      }
    }
  }
  return true;
}

} // namespace view
} // namespace pv
