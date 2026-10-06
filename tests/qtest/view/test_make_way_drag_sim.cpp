// 让位拖动"整列平移"复现器：按生产代码的**真实调用顺序**逐帧模拟一次拖动，
// 逐帧检查每个通道的目标坐标，用于锁死快照事务模型的两条跨帧行为：
//
//   1. 锚点恒定 —— 所有目标都从按下时捕获的 content_top 推导，无论手移到哪，
//      整列绝不跟着手指平移；
//   2. 被拖项空出的槽位由邻居接管 —— 这是"真的发生让位"的判据。
//
// 与 test_v_offset_animation 的区别：那个测 Trace 双层 y 坐标原语；本文件测
// make_way::layout_snapshot 的**几何语义**（分组间距、真实高度、隐藏行、
// 硬块整组搬移），外加跨帧仿真。

#include <QtTest/QtTest>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "pv/view/trace/make_way.h"

using pv::view::make_way::DragRow;
using pv::view::make_way::DragSnapshot;

namespace {

const int kH = 40;
const int kMargin = 7;
const int kTop = 7;
// 生产槽距 pitch = 通道高 + 2*SignalMargin。
const int kPitch = kH + 2 * kMargin;

/**
 * 造 n 个等高未分组行，中心依次落在 kTop+h/2, +kPitch, …，返回快照。
 *
 * 生产语义（begin_trace_drag）：未分组行的 group_id 一律是 -1（同值），
 * 只有 block_id 才拿互异的负数 id；item id 从 1 起 —— 0 会被
 * DragSnapshot::valid() 的 Item{} 哨兵当成"未设置"。
 */
DragSnapshot<int> make_snapshot(int n, int dragged_index) {
  DragSnapshot<int> snapshot;
  for (int i = 0; i < n; i++) {
    DragRow<int> row;
    row.item = i + 1;
    row.center = kTop + kH / 2 + i * kPitch;
    row.height = kH;
    row.group_id = -1; // 未分组：共享 -1，不触发组间距
    row.block_id = -1 - i; // 未分组：每行独立成块
    row.visible = true;
    snapshot.rows.push_back(row);
  }
  snapshot.dragged = dragged_index + 1;
  snapshot.dragged_start_center =
      snapshot.rows[dragged_index].center;
  snapshot.content_top = kTop;
  snapshot.margin = kMargin;
  snapshot.group_gap = 15;
  return snapshot;
}

} // namespace

class TestMakeWayDragSim : public QObject {
  Q_OBJECT

private slots:
  // ---- 跨帧仿真：锚点恒定 + 邻居接管空槽 -------------------------------

  void SimDraggingTopRowDownward() {
    const auto snapshot = make_snapshot(4, 0);

    // 逐帧 mouseMoveEvent：手从原槽逐格下移（量化坐标），每帧都从
    // **同一个不可变快照**重算 —— 生产中 animate_make_way_for_drag 就是
    // 每个量化步调一次 layout_snapshot(_drag_snapshot, y_snap)。
    std::vector<int> first_targets;
    for (int step = 1; step <= 3; step++) {
      const int hand = kTop + kH / 2 + step * kPitch;
      const auto layout = pv::view::make_way::layout_snapshot(snapshot, hand);
      QCOMPARE(layout.order.size(), std::size_t(4));

      // 被拖项不拿目标（跟手），其余行目标全部落在固定网格上。
      int seen = 0;
      for (const auto &t : layout.targets) {
        QVERIFY(t.first != snapshot.dragged);
        const int slot = (t.second - kTop - kH / 2) / kPitch;
        QCOMPARE((t.second - kTop - kH / 2) % kPitch, 0); // 固定网格 → 无整列平移
        QVERIFY(slot >= 0);
        seen++;
      }
      QCOMPARE(seen, 3);
      if (step == 1) {
        for (const auto &t : layout.targets)
          first_targets.push_back(t.second);
      }
    }

    // 最终帧：被拖项排到最后，其原占的 0 号槽由邻居接管。
    const auto final_layout =
        pv::view::make_way::layout_snapshot(snapshot, kTop + kH / 2 + 3 * kPitch);
    QCOMPARE(final_layout.order[0], 2);
    QCOMPARE(final_layout.order[3], 1);
    // 第一帧之后邻居进入 0 号槽且不再离开：锚点恒定的直接推论。
    QCOMPARE(first_targets[0], kTop + kH / 2);
  }

  void SimDraggingBottomRowUpward() {
    const auto snapshot = make_snapshot(4, 3);

    // 反向场景：从最底往上拖。旧实现的 stable_sort 插入序在这个方向必错
    // （邻居不让位）；快照模型必须两个方向对称。
    for (int step = 1; step <= 3; step++) {
      const int hand = kTop + kH / 2 + (3 - step) * kPitch;
      const auto layout = pv::view::make_way::layout_snapshot(snapshot, hand);
      QCOMPARE(layout.order.size(), std::size_t(4));
      for (const auto &t : layout.targets) {
        QVERIFY(t.first != snapshot.dragged);
        QCOMPARE((t.second - kTop - kH / 2) % kPitch, 0);
        QVERIFY(t.second >= kTop + kH / 2); // 整列未被向上拽走
      }
    }

    // 最终帧：被拖项占据 0 号槽，其余三行依次下移一格。
    const auto final_layout =
        pv::view::make_way::layout_snapshot(snapshot, kTop + kH / 2);
    QCOMPARE(final_layout.order[0], 4);
    QCOMPARE(final_layout.order[1], 1);
    std::vector<int> centers;
    for (const auto &t : final_layout.targets)
      centers.push_back(t.second);
    std::sort(centers.begin(), centers.end());
    QCOMPARE(centers[0], kTop + kH / 2 + kPitch); // 0 号槽让给了被拖项
  }

  // ---- 快照几何语义 -----------------------------------------------------

  void VariableHeightRowsPreserveGroupGap() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 101, 80, 0, 0, true},
        DragRow<int>{3, 185, 30, 1, 1, true},
    };
    snapshot.dragged = 1;
    snapshot.dragged_start_center = 27;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 101);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 2);
    QCOMPARE(layout.order[1], 1);
    QCOMPARE(layout.order[2], 3);
    QCOMPARE(layout.targets.size(), std::size_t(2));
    QCOMPARE(layout.targets[0].first, 2);
    QCOMPARE(layout.targets[0].second, 47);
    QCOMPARE(layout.targets[1].first, 3);
    QCOMPARE(layout.targets[1].second, 185);
  }

  void CrossingBoundaryMovesWholeGroup() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 101, 80, 0, 0, true},
        DragRow<int>{3, 185, 30, 1, 1, true},
    };
    snapshot.dragged = 1;
    snapshot.dragged_start_center = 27;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 250);
    QCOMPARE(layout.order.size(), std::size_t(3));
    // 整块搬移 + 被拖行（组头顶行）骑到运动前缘（向下 → 组尾）。
    QCOMPARE(layout.order[0], 3);
    QCOMPARE(layout.order[1], 2);
    QCOMPARE(layout.order[2], 1);
    QCOMPARE(layout.targets.size(), std::size_t(2));
    QCOMPARE(layout.targets[0].first, 3);
    QCOMPARE(layout.targets[0].second, 22);
    QCOMPARE(layout.targets[1].first, 2);
    QCOMPARE(layout.targets[1].second, 106);
  }

  void UngroupedRowsDoNotGainGroupGap() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, -1, -1, true},
        DragRow<int>{2, 81, 40, -2, -2, true},
        DragRow<int>{3, 135, 40, -3, -3, true},
    };
    snapshot.dragged = 2;
    snapshot.dragged_start_center = 81;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 135);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 1);
    QCOMPARE(layout.order[1], 3);
    QCOMPARE(layout.order[2], 2);
    QCOMPARE(layout.targets.size(), std::size_t(2));
    QCOMPARE(layout.targets[0].first, 1);
    QCOMPARE(layout.targets[0].second, 27);
    QCOMPARE(layout.targets[1].first, 3);
    QCOMPARE(layout.targets[1].second, 81);
  }

  void SingleGroupAllowsEdgeOvershoot() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 81, 40, 0, 0, true},
        DragRow<int>{3, 135, 40, 0, 0, true},
    };
    snapshot.dragged = 3;
    snapshot.dragged_start_center = 135;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, -100);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 3);
    QCOMPARE(layout.order[1], 1);
    QCOMPARE(layout.order[2], 2);
  }

  void HiddenRowsRemainInCommittedPermutation() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, INT_MAX, 40, 0, 0, false},
        DragRow<int>{3, 81, 40, 0, 0, true},
    };
    snapshot.dragged = 3;
    snapshot.dragged_start_center = 81;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 20);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 3);
    QCOMPARE(layout.order[1], 1);
    QCOMPARE(layout.order[2], 2);
    QCOMPARE(layout.targets.size(), std::size_t(1));
    QCOMPARE(layout.targets[0].first, 1);
    QCOMPARE(layout.targets[0].second, 81);
  }

  void MovingGroupUpwardPreservesItsMembers() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 81, 40, 0, 0, true},
        DragRow<int>{3, 150, 40, 1, 1, true},
    };
    snapshot.dragged = 3;
    snapshot.dragged_start_center = 150;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 0);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 3);
    QCOMPARE(layout.order[1], 1);
    QCOMPARE(layout.order[2], 2);
  }

  void NonContiguousGroupIdsAreNotSilentlyMerged() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 81, 40, 1, 1, true},
        DragRow<int>{3, 135, 40, 0, 0, true},
    };
    snapshot.dragged = 2;
    snapshot.dragged_start_center = 81;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 81);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 1);
    QCOMPARE(layout.order[1], 2);
    QCOMPARE(layout.order[2], 3);
  }

  /**
   * 回归：整块搬移的触发点必须跟随**被拖行**的中心，而不是块的几何中心。
   * 旧实现 moving_center = block.center() + delta，拖住多行块的边缘行时偏差
   * 达半个块高 —— 整组迟迟不跟手，越过阈值后突然跳位，下方所有组同时向上
   * 补齐空档（"拖到组顶以上一段距离引发连锁错乱"）。
   *
   * 几何（生产公式）：组 A={1,2}，组 B={3}，组间距 15：
   *   row1 c27, row2 c81（块 A: top 7, bottom 101, center 54）
   *   row3 c150（块 B: top 130, bottom 170, center 150）
   */
  void WholeBlockTriggerFollowsHandNotBlockCentroid() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 81, 40, 0, 0, true},
        DragRow<int>{3, 150, 40, 1, 1, true},
    };
    snapshot.dragged = 1; // 拖住组 A 的**顶行**：块中心偏差最大的位置
    snapshot.dragged_start_center = 27;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    // 手在组 B 中心(150)之下、块 A 之外（已越过块 A 底部 101）：块未落位，
    // 但被拖行已骑到组尾（运动前缘）。旧实现在这里既不落位也不重排。
    for (int hand = 110; hand <= 149; hand++) {
      const auto layout = pv::view::make_way::layout_snapshot(snapshot, hand);
      QCOMPARE(layout.order[0], 2);
      QCOMPARE(layout.order[1], 1);
      QCOMPARE(layout.order[2], 3);
    }
    // 手越过组 B 中心：整组搬移，B 让到顶上，被拖行仍在组尾。
    const auto moved = pv::view::make_way::layout_snapshot(snapshot, 150);
    QCOMPARE(moved.order[0], 3);
    QCOMPARE(moved.order[1], 2);
    QCOMPARE(moved.order[2], 1);
  }

  /** 对称场景：拖住组 A 的**底行**向上，触发点同样必须跟手而不是跟块中心。 */
  void WholeBlockUpwardTriggerFollowsHand() {
    DragSnapshot<int> snapshot;
    // 视觉顺序：组 Z={1}，组 A={2,3}，组 B={4}
    //   row1 c27（块 Z center 27），row2 c96, row3 c150
    //   （块 A: top 76, bottom 170, center 123），row4 c205
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 1, 1, true},
        DragRow<int>{2, 96, 40, 0, 0, true},
        DragRow<int>{3, 150, 40, 0, 0, true},
        DragRow<int>{4, 205, 40, 2, 2, true},
    };
    snapshot.dragged = 3; // 拖住组 A 的**底行**
    snapshot.dragged_start_center = 150;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    // 手已到块 A 顶部(76)之上但未过组 Z 中心(27)：块未落位，但被拖行（组底
    // 行）已骑到组头（向上 → 运动前缘）。
    for (int hand = 30; hand <= 75; hand++) {
      const auto layout = pv::view::make_way::layout_snapshot(snapshot, hand);
      QCOMPARE(layout.order[0], 1);
      QCOMPARE(layout.order[1], 3);
      QCOMPARE(layout.order[2], 2);
      QCOMPARE(layout.order[3], 4);
    }
    // 手越过组 Z 中心：整组 A 搬到 Z 上面，被拖行仍在组头。
    const auto moved = pv::view::make_way::layout_snapshot(snapshot, 20);
    QCOMPARE(moved.order[0], 3);
    QCOMPARE(moved.order[1], 2);
    QCOMPARE(moved.order[2], 1);
    QCOMPARE(moved.order[3], 4);
  }

  /**
   * 用户场景回归：组 A={1,2,3,4,5}，组 B={6}（"12345--6"）。把 1 向下拖过 6，
   * 结果必须是 "6-23451"（6 到顶、2/3/4/5 保序、1 落到组尾），而不是
   * "6-12345"（1 回到组内原位）。
   */
  void CrossingGroupBoundaryRidesDraggedRowToGroupTail() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 27, 40, 0, 0, true},
        DragRow<int>{2, 81, 40, 0, 0, true},
        DragRow<int>{3, 135, 40, 0, 0, true},
        DragRow<int>{4, 189, 40, 0, 0, true},
        DragRow<int>{5, 243, 40, 0, 0, true},
        DragRow<int>{6, 312, 40, 1, 1, true},
    };
    snapshot.dragged = 1;
    snapshot.dragged_start_center = 27;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    // 手越过组 B 中心(312)：整组 A 搬到 B 之后，1 骑到组尾 → "6-23451"。
    const auto crossed = pv::view::make_way::layout_snapshot(snapshot, 320);
    QCOMPARE(crossed.order[0], 6);
    QCOMPARE(crossed.order[1], 2);
    QCOMPARE(crossed.order[2], 3);
    QCOMPARE(crossed.order[3], 4);
    QCOMPARE(crossed.order[4], 5);
    QCOMPARE(crossed.order[5], 1);
    // 6 让位到最顶端。
    for (const auto &t : crossed.targets)
      if (t.first == 6)
        QCOMPARE(t.second, 27);

    // 手在组 A 之下、组 B 中心之上（组间空隙）：块未落位，但 1 已在组尾。
    const auto gap = pv::view::make_way::layout_snapshot(snapshot, 280);
    QCOMPARE(gap.order[0], 2);
    QCOMPARE(gap.order[4], 1);
    QCOMPARE(gap.order[5], 6);
  }

  /**
   * 回归：奇数高度行的预览目标必须与生产布局 qRound(x.5) 逐像素同口径。
   * 旧实现用整数除法 h/2（向下截断），奇数高度差 1px——预览每帧重发全列
   * 目标，导致远处奇数高度的行每次拖动都被推 1px。
   *
   * 生产几何（margin=7，从顶边 14 起堆叠）：h41 中心 = 顶 + 21，h40 中心 =
   * 顶 + 20；顶边 = 中心 - ceil(h/2)。
   */
  void OddHeightRowsMatchProductionRounding() {
    DragSnapshot<int> snapshot;
    snapshot.rows = {
        DragRow<int>{1, 35, 41, -1, -1, true},   // 顶 14，中心 14+21
        DragRow<int>{2, 89, 40, -1, -2, true},   // 顶 69，中心 69+20
        DragRow<int>{3, 144, 41, -1, -3, true},  // 顶 123，中心 123+21
    };
    snapshot.dragged = 3;
    snapshot.dragged_start_center = 144;
    snapshot.content_top = 14;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 144);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.targets.size(), std::size_t(2));
    for (const auto &t : layout.targets) {
      if (t.first == 1)
        QCOMPARE(t.second, 35);
      else if (t.first == 2)
        QCOMPARE(t.second, 89);
      else
        QFAIL("unexpected target row");
    }
  }

  void VisualGroupDoesNotForceOrdinaryRowsToMoveTogether() {    DragSnapshot<int> snapshot;
    snapshot.rows = {
        // Same visual group, but unique block ids: ordinary rows stay
        // independently draggable.
        DragRow<int>{1, 27, 40, 0, -1, true},
        DragRow<int>{2, 81, 40, 0, -2, true},
        DragRow<int>{3, 150, 40, 1, 1, true},
    };
    snapshot.dragged = 1;
    snapshot.dragged_start_center = 27;
    snapshot.content_top = 7;
    snapshot.margin = 7;
    snapshot.group_gap = 15;

    const auto layout = pv::view::make_way::layout_snapshot(snapshot, 81);
    QCOMPARE(layout.order.size(), std::size_t(3));
    QCOMPARE(layout.order[0], 2);
    QCOMPARE(layout.order[1], 1);
    QCOMPARE(layout.order[2], 3);
    QCOMPARE(layout.targets.size(), std::size_t(2));
    QCOMPARE(layout.targets[0].first, 2);
    QCOMPARE(layout.targets[0].second, 27);
    QCOMPARE(layout.targets[1].first, 3);
    QCOMPARE(layout.targets[1].second, 150);
  }
};

QTEST_MAIN(TestMakeWayDragSim)
#include "test_make_way_drag_sim.moc"
