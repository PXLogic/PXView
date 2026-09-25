// 让位拖动"整列平移"复现器：按生产代码的**真实调用顺序**逐帧模拟一次拖动，
// 逐帧打印每个通道的 layout/visual 坐标，用于定位"拖动最上面的通道时其余通道
// 跟着整列平移"的根因。
//
// 与 test_v_offset_animation 的区别：那个只测"给定锚点，layout() 算出什么"；
// 本用例把 Header::mousePressEvent 的锚点采集 + mouseMoveEvent 的
// force_to_v_offset + animate_make_way_for_drag 三段串起来跑，因此能暴露
// "锚点采集时机/被拖项 layout 被改写"这类**跨帧状态**问题。

#include <QtTest/QtTest>

// ── 标准库头必须在目标头之前 include ──
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

// ── xlog stub: trace.cpp 经 log.h 引用 pxv_log + xlog_* ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/view/trace/make_way.h"
#include "pv/view/trace/trace.h"
// IRenderView 只前向声明 Cursor/XCursor/Signal，而 Trace 按引用返回它们的容器，
// 因此这些类型必须是完整类型。
#include "pv/view/cursor/cursor.h"
#include "pv/view/cursor/xcursor.h"
#include "pv/view/signal/signal.h"

using pv::view::Trace;

// Trace 的构造函数 protected 且带参；唯一纯虚成员是 enabled()。
namespace {

const int kH = 40;
const int kRowGap = 2 * 7; // 2 * SignalMargin

class SimTrace : public Trace {
public:
  explicit SimTrace(int idx) : Trace(QStringLiteral("ch%1").arg(idx), idx, 10000) {
    set_totalHeight(kH);
  }
  bool enabled() override { return true; }
  static int pitch() { return kH + 2 * 7; }
};

/** 复刻 Header::mousePressEvent 采集锚点的逻辑。 */
int capture_anchor(const std::vector<Trace *> &all) {
  int anchor = INT_MAX;
  for (auto *t : all) {
    const int y = t->get_v_offset();
    if (y != INT_MAX)
      anchor = std::min(anchor, y);
  }
  return anchor;
}

/**
 * 复刻 Header::mousePressEvent 的**真实顺序**，返回本次拖动用的锚点。
 *
 * 关键：`_drag_traces` 在"用户点的那一项被 select(true)"之后才填充。
 * 若在 select 之前读 `t->selected()` 去算锚点，此刻什么也收不到 → 锚点退化成
 * INT_MAX → 整个拖动期间每帧现算 → 整列跟着手指平移。
 * 本函数按正确顺序（先 select，再按 `_drag_traces` 非空采集锚点）实现。
 */
int press_and_capture_anchor(const std::vector<Trace *> &all, Trace *clicked,
                             std::vector<Trace *> &drag_traces) {
  // --- (1) 早期循环：读的是**上一次**的选中态，此时什么都没选中 ---
  for (auto *t : all) {
    if (t->selected())
      drag_traces.push_back(t);
  }

  // --- (2) NAME/LABEL 分支：选中被点击项并填充 _drag_traces ---
  clicked->select(true);
  for (auto *t : all)
    if (t != clicked)
      t->select(false);
  drag_traces.clear();
  drag_traces.push_back(clicked);

  // --- (3) ★ 锚点采集（必须在这里，_drag_traces 已就绪）---
  if (drag_traces.empty())
    return INT_MAX;
  return capture_anchor(all);
}

/** 复刻 ViewSignalSync::animate_make_way_for_drag 的核心（去掉绘图/动画）。 */
void make_way_step(std::vector<Trace *> &all, Trace *drag, int anchor_y) {
  std::vector<Trace *> others;
  for (auto *t : all)
    if (t != drag)
      others.push_back(t);
  if (others.empty())
    return;

  // 锚点优先用记下来的常量；排序 + 破并结全部由 layout() 内部统一完成。
  const int anchor = pv::view::make_way::resolve_anchor(anchor_y, all);
  // 行进方向：与生产代码同一条判据（见 view_signal_sync.cpp）。
  const bool downward = drag->get_v_offset() >= anchor;

  std::vector<Trace *> ordered;
  std::vector<std::pair<Trace *, int>> targets;
  pv::view::make_way::layout(others, drag, anchor, kRowGap, downward, ordered,
                             targets);

  for (auto &nt : targets) {
    if (nt.second < 0)
      continue;
    nt.first->set_v_offset_no_visual_sync(nt.second);
    nt.first->animate_to_layout_v_offset();
  }
}

} // namespace

class TestMakeWayDragSim : public QObject {
  Q_OBJECT

private slots:
  void SimDraggingTopRowDownward() {
    std::vector<std::unique_ptr<SimTrace>> rows;
    for (int i = 0; i < 4; i++) {
      rows.push_back(std::make_unique<SimTrace>(i));
      rows.back()->force_to_v_offset(i * SimTrace::pitch());
    }
    std::vector<Trace *> all;
    for (auto &r : rows)
      all.push_back(r.get());

    Trace *drag = rows[0].get(); // 拖最上面那一行

    // ---- mousePressEvent：按真实顺序（先选中、再采锚点）----
    std::vector<Trace *> drag_traces;
    const int anchor = press_and_capture_anchor(all, drag, drag_traces);
    QVERIFY(anchor != INT_MAX); // 锚点必须真的采到，否则拖到哪都整列平移
    QCOMPARE(anchor, 0);

    // ---- 逐帧 mouseMoveEvent ----
    // 每帧落点 = 邻行格位中心 → 被拖项 layout 与邻居 layout **精确撞值**，
    // 这正是"必须按行进方向破并结"的触发条件（见 order_by_layout_offset）。
    for (int step = 1; step <= 3; step++) {
      // (a) 被拖项跟手：force_to_v_offset（生产在 header.cpp 用 y_snap 对齐）
      drag->force_to_v_offset(step * SimTrace::pitch());
      // (b) 其余通道让位（生产在 header.cpp → animate_make_way_for_drag）
      make_way_step(all, drag, anchor);
    }

    // ★ 核心判据（注意：**不是**要求其余行占满全部槽位 —— 那是 drop handler
    //   signals_changed() 才做的权威重排；拖动预览只保证"槽位网格固定 + 被拖项
    //   空出的那一格由邻居顶上"）：
    //   1. 其余通道必须两两不重叠、且全部落在固定网格 {anchor + k*pitch} 上；
    //   2. 被拖项原占的 0 号槽必须被邻居接管（否则就是"整列平移"导致换不了位）。
    std::vector<int> slot;
    for (int i = 1; i < 4; i++)
      slot.push_back(rows[i]->get_v_offset());
    std::stable_sort(slot.begin(), slot.end());
    const int p = SimTrace::pitch();
    for (int y : slot) {
      QCOMPARE((y - anchor) % p, 0); // 严格落在固定网格上 → 没有被整体平移
      QVERIFY(y >= anchor);
    }
    QCOMPARE(slot[0], anchor); // 0 号槽被邻居接管 = 真的发生了让位
    QVERIFY(slot[1] > slot[0] && slot[2] > slot[1]); // 不重叠
  }

  void SimDraggingBottomRowUpward() {
    // 反向场景：从最底往上拖。这是"并结破错方向"必然失败的那个方向 ——
    // stable_sort 的插入序在向下方向碰巧对、向上方向必错（邻居不让位）。
    std::vector<std::unique_ptr<SimTrace>> rows;
    for (int i = 0; i < 4; i++) {
      rows.push_back(std::make_unique<SimTrace>(i));
      rows.back()->force_to_v_offset(i * SimTrace::pitch());
    }
    std::vector<Trace *> all;
    for (auto &r : rows)
      all.push_back(r.get());

    Trace *drag = rows[3].get(); // 拖最下面那一行往上
    std::vector<Trace *> drag_traces;
    const int anchor = press_and_capture_anchor(all, drag, drag_traces);
    QVERIFY(anchor != INT_MAX);
    QCOMPARE(anchor, 0);

    const int p = SimTrace::pitch();
    for (int step = 1; step <= 3; step++) {
      drag->force_to_v_offset((3 - step) * p); // 3→0 槽，逐格上移（同样撞格位中心）
      make_way_step(all, drag, anchor);
    }

    // 被拖项占了 0 号槽，其余三行随之各自落在一个**固定网格点**上（互不重叠），
    // 且整列**没有**被向上拽走 —— 若锚点漂移，它们会整体上移、脱离网格。
    std::vector<int> slot;
    for (int i = 0; i < 3; i++)
      slot.push_back(rows[i]->get_v_offset());
    std::stable_sort(slot.begin(), slot.end());
    for (int y : slot) {
      QCOMPARE((y - anchor) % p, 0); // 落在固定网格上 → 整体未被平移
      QVERIFY(y >= anchor);
      QVERIFY(y <= anchor + 3 * p);
    }
    QVERIFY(slot[0] > anchor); // 0 号槽让给了被拖项
    QVERIFY(slot[1] > slot[0] && slot[2] > slot[1]); // 不重叠
  }
};

QTEST_MAIN(TestMakeWayDragSim)
#include "test_make_way_drag_sim.moc"
