/*
 * test_view_index_invariants.cpp — view_index / 信号分组不变量校验
 *
 * 背景
 * ----
 * `signals_changed()` 里有一条隐式契约：
 *
 *     normalize_view_indices()      // 给所有 trace 重排 0..n-1
 *          ↓
 *     compute_signal_groups()       // 按 view_index 扫连续段分组
 *
 * view_index 是**第一次遍历**的结果，SignalGroup 是**第二次遍历**的结果，
 * 两者靠上面的调用顺序保持一致，而不是靠类型系统。历史踩坑：重复
 * view_index → std::sort 不稳定 → 通道顺序错乱（静默、不崩）。
 *
 * 这个测试锁死两条校验函数的语义，并要求它们可以【分开】调用：
 *
 *   1. view_indices_are_permutation()：0..n-1 无重复、无空洞。
 *   2. group_view_indices_are_contiguous()：每个 group 内序号连续。
 *
 * 为什么"分开可调用"本身是契约：不变量 2 读的是 `_signal_groups`，
 * 而它在 `compute_signal_groups()` 里才重建。若把两条校验都塞在
 * `normalize_view_indices()` 末尾（本 bug 的原始形态），不变量 2 读到的是
 * **上一轮**的陈旧分组 —— 切 tab / 增删通道时会刷出
 * "group 5 内 view_index 不连续（9 后跟 15）" 的**假告警**（序号本身完全
 * 合法）。因此本文件的 ContiguityIsMeaningfulOnlyAgainstCurrentGroups
 * 用纯逻辑数据复现"陈旧 group vs 新 index"的错配，把
 * "分组连续性只能对当轮分组判定"从注释变成可执行断言。
 *
 * 依赖链：零 View/QWidget/Trace 依赖 —— 校验核心只吃 std::vector<int>，
 * 因此本目标不需要链接 pxview-render，只编译 view_index_invariants.cpp。
 * （Trace/SignalGroup 适配层在同一 TU 里，但本测试不触达它们；见 CMakeLists
 *  中为何仍列出 pxview-render 依赖的说明。）
 */

#include <QtTest/QtTest>

#include <vector>

// xlog stub: view_index_invariants.cpp 经 log.h 的 pxv_assert 引用 pxv_err
// → pxv_log + xlog_*。本测试不链接 PXView / pxview-render，故在此提供替身。
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/view/view_index_invariants.h"

using pv::view::group_view_indices_are_contiguous;
using pv::view::view_indices_are_permutation;

class TestIdxCheck : public QObject {
  Q_OBJECT
private slots:
  // ---- 1) 不变量 1：0..n-1 排列 ----

  void PermutationAcceptsContiguousZeroBased() {
    QVERIFY(view_indices_are_permutation({0, 1, 2, 3}));
  }

  void PermutationAcceptsUnorderedInput() {
    // 容器顺序无关：3,0,2,1 仍是合法排列。
    QVERIFY(view_indices_are_permutation({3, 0, 2, 1}));
  }

  void PermutationAcceptsSingle() {
    QVERIFY(view_indices_are_permutation({0}));
  }

  void PermutationAcceptsEmpty() {
    QVERIFY(view_indices_are_permutation({}));
  }

  void PermutationRejectsDuplicate() {
    // 0,1,1,3 —— 重复 1、缺 2。
    QVERIFY(!view_indices_are_permutation({0, 1, 1, 3}));
  }

  void PermutationRejectsHole() {
    // 0,1,3 —— 缺 2（这正是历史 bug 的形态）。
    QVERIFY(!view_indices_are_permutation({0, 1, 3}));
  }

  void PermutationRejectsNonZeroStart() {
    // 1,2,3 —— 无 0。
    QVERIFY(!view_indices_are_permutation({1, 2, 3}));
  }

  void PermutationRejectsNegative() {
    QVERIFY(!view_indices_are_permutation({-1, 0, 1}));
  }

  // ---- 2) 不变量 2：group 内连续 ----

  void ContiguityAcceptsContiguousGroup() {
    QVERIFY(group_view_indices_are_contiguous({{0, 1, 2}}, {0}));
  }

  void ContiguityAcceptsUnorderedWithinGroup() {
    // group 内顺序无关：2,0,1 排序后仍是 0,1,2。
    QVERIFY(group_view_indices_are_contiguous({{2, 0, 1}}, {7}));
  }

  void ContiguityAcceptsMultipleGroups() {
    // 两个 group 各自内部连续（组间不要求跨组连续）。
    QVERIFY(group_view_indices_are_contiguous({{0, 1}, {5, 6, 7}}, {0, 1}));
  }

  void ContiguityRejectsGapWithinGroup() {
    // group 内 0,1,3（缺 2）—— 不连续。
    QVERIFY(!group_view_indices_are_contiguous({{0, 1, 3}}, {0}));
  }

  void ContiguityRejectsDuplicateWithinGroup() {
    // group 内 1,1 —— 重复，也判不连续。
    QVERIFY(!group_view_indices_are_contiguous({{1, 1}}, {0}));
  }

  void ContiguitySkipsSingleTraceGroup() {
    // 单元素 group 恒连续（无需相邻项），值本身不重要。
    QVERIFY(group_view_indices_are_contiguous({{42}}, {0}));
  }

  void ContiguitySkipsEmptyGroup() {
    QVERIFY(group_view_indices_are_contiguous({{}}, {0}));
  }

  void ContiguityAcceptsNoGroups() {
    QVERIFY(group_view_indices_are_contiguous({}, {}));
  }

  // ---- 3) 回归：陈旧 group vs 新 index 的错配 ----
  //
  // 复现本 bug 的判定语义：不变量 2 只对【与当前序号同轮】的 group 有意义。
  // 同一批序号，按"当轮分组"切片 vs 按"上一轮分组"切片，结论相反：
  //   - 当轮 group（成员 0,1,2）→ 连续（合法）
  //   - 陈旧 group（成员 0 与 3）→ 不连续（假告警的来源）
  // 这正是日志里 "group 5 内 view_index 不连续（9 后跟 15）" 的成因。
  void ContiguityIsMeaningfulOnlyAgainstCurrentGroups() {
    // 当轮 index 是 0..3 的合法排列。
    QVERIFY(view_indices_are_permutation({0, 1, 2, 3}));

    // 当轮 group：成员序号 0,1,2 → 连续。
    QVERIFY(group_view_indices_are_contiguous({{0, 1, 2}}, {0}));

    // 陈旧 group：成员序号 0 与 3 → 不连续（0 后跟 3）。
    // 校验函数本身没错；错的是"用当轮序号去判陈旧分组"这个调用时机。
    QVERIFY(!group_view_indices_are_contiguous({{0, 3}}, {0}));
  }
};

QTEST_MAIN(TestIdxCheck)
#include "test_view_index_invariants.moc"
