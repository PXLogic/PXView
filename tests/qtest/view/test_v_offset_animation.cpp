/*
 * test_v_offset_animation.cpp — 通道拖动动画的双层 y 坐标契约
 *
 * 背景
 * ----
 * PXView 原先只有单层 v_offset（"通道画在哪儿" == "通道应该在哪儿"），因此
 * 拖动重排时其余通道只能瞬间跳变。引入 PulseView 同构的**双层坐标**后：
 *
 *     _v_offset        = layout 目标（布局算法写它、排序/持久化读它）
 *     _visual_v_offset = 实际绘制位置（绘制/命中测试读它）
 *
 * 拖动时布局先把目标移到新位置，再由 QPropertyAnimation 把 visual 值平滑推
 * 过去，于是"被拖项跟手 + 其余项滑动让位"才成立。
 *
 * 这个测试锁死双层坐标的几条**必须成立**的不变量。它们全都在运行时才会被
 * 违反（编译期毫无提示），一旦破坏就是"通道瞬移 / 动画永远到不了终点 /
 * 命中测试与绘制错位"这类肉眼可见但不崩的缺陷：
 *
 *   1. 无动画时 set_v_offset() 必须同步两层 —— 否则正常布局（layout_time_
 *      signals / 配置恢复 / FFT 放置）会把 visual 落在旧值上，通道画错位置。
 *   2. 有动画在跑时 set_v_offset() 必须**不**碰 visual —— 否则目标一改就把
 *      visual 拽到终点，动画无事可做（让位变成瞬移）。
 *   3. set_v_offset_no_visual_sync() 只写 layout：这是拖动让位"先移目标、
 *      再启动动画"的前置条件，写错就成了瞬移。
 *   4. force_to_v_offset() 必须同时把两层硬设到同一点，且取消在跑的动画 ——
 *      被拖项要严格跟手，不能有插值延迟。
 *   5. animate_to_layout_v_offset() 在"已在目标"时不得创建/启动动画 —— 它是
 *      每帧都会被调用的热路径，无脑起动画会让整帧成本暴涨。
 *   6. INT_MAX（"尚未布局"哨兵）绝不能被插值 —— 从/到哨兵做动画会产生天文
 *      数字的中间帧。必须硬设到位。
 *
 * 依赖链：trace.cpp + iview_delegates.h(Qt Core/Gui)。用最小的 MockRenderView
 * 承接 repaint 回调，不构造任何 Widget，无需 SignalModel / 数据源。
 * 与 test_rasterize 一样内部 stub xlog。
 */

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
// IRenderView 只前向声明 Cursor/XCursor/Signal，而 MockRenderView 按引用返回
// 它们的容器（getter 签名带 std::list<unique_ptr<...>>&），因此这些类型必须是
// 完整类型。trace.h 的 moc 由本目标的 AUTOMOC 生成（见 CMakeLists 说明）。
#include "pv/view/cursor/cursor.h"
#include "pv/view/cursor/xcursor.h"
#include "pv/view/signal/signal.h"

using pv::view::Trace;

namespace {

// ---------------------------------------------------------------------------
// MockRenderView — 只为给 Trace::on_visual_v_offset_changed() 提供一个可记录的
// repaint 落点，并满足 IRenderView 的纯虚接口。
//
// IRenderView 有 ~60 个纯虚函数，且大部分返回 View 层生态类型（SigSession /
// DataSource / SignalGroup / LissajousTrace ...）。逐一 include 那些头会把
// 整条 View 依赖链拖进测试，因此这里用**按值返回的轻量替身** = 仅在真正被调
// 用到的成员上给出语义；其余返回空指针/零值。测试只走 Trace 的 y 坐标路径，
// 除了 request_animation_repaint() 之外没有任何成员会被触达。
//
// 注意 group/theme 等引用返回的成员用函数内静态对象兜底，避免悬垂引用。
// ---------------------------------------------------------------------------
class MockRenderView : public pv::view::IRenderView {
public:
  int repaint_count = 0;

  // —— 唯一测试关心的成员 ——
  void request_animation_repaint() override { repaint_count++; }

  // ---- scale / offset / layout ----
  double scale() override { return 1.0; }
  int64_t offset() override { return 0; }
  int get_signalHeight() override { return 24; }
  int get_vOffset() override { return 0; }
  double trig_hoff() override { return 0.0; }
  void set_trig_hoff(double) override {}

  // ---- cursors ----
  bool cursors_shown() override { return false; }
  bool trig_cursor_shown() override { return false; }
  bool search_cursor_shown() override { return false; }
  bool xcursors_shown() override { return false; }
  std::list<std::unique_ptr<pv::view::Cursor>> &get_cursorList() override {
    return _cursors;
  }
  std::list<std::unique_ptr<pv::view::XCursor>> &get_xcursorList() override {
    return _xcursors;
  }
  pv::view::Cursor *get_trig_cursor() override { return nullptr; }
  pv::view::Cursor *get_search_cursor() override { return nullptr; }
  uint64_t get_cursor_samples(int) override { return 0; }
  int get_cursor_index_by_key(uint64_t) override { return -1; }

  // ---- geometry ----
  QRect get_view_rect() override { return QRect(); }
  int get_view_width() override { return 0; }
  int get_view_height() override { return 0; }
  int scroll_viewport_width() override { return 0; }
  int scroll_viewport_height() override { return 0; }
  QPoint &hover_point() override { return _hover; }
  double index2pixel(uint64_t, bool = true) override { return 0.0; }
  uint64_t pixel2index(double) override { return 0; }

  // ---- traces / groups (未触达；引用返回用静态兜底) ----
  void get_traces(int, std::vector<Trace *> &) override {}
  std::vector<std::unique_ptr<pv::view::Signal>> &get_own_signals() override {
    return _own_signals;
  }
  pv::view::LissajousTrace *get_own_lissajous_trace() override {
    return nullptr;
  }
  const std::vector<pv::view::SignalGroup> &get_signal_groups() override {
    static const std::vector<pv::view::SignalGroup> empty;
    return empty;
  }

  // ---- colors ----
  bool is_colored_card_mode() override { return false; }
  QColor get_group_card_color() override { return QColor(); }
  QColor get_trace_card_color(Trace *) override { return QColor(); }
  QColor theme_red() override { return QColor(Qt::red); }
  QColor theme_orange() override { return QColor(255, 165, 0); }
  QColor theme_blue() override { return QColor(Qt::blue); }
  QColor theme_green() override { return QColor(Qt::green); }
  QColor theme_purple() override { return QColor(128, 0, 128); }
  QColor theme_lightblue() override { return QColor(173, 216, 230); }

  // ---- session / data source（未触达，返回 nullptr）----
  pv::SigSession &session() override { return *_session; }
  pv::data::DataSource *data_source() override { return nullptr; }
  pv::data::DataSource *document_snapshot_source() override {
    return nullptr;
  }
  bool display_source_is_document() override { return false; }

  // ---- status ----
  bool is_stopped_status() const override { return true; }
  bool is_running_status() const override { return false; }
  bool is_init_status() const override { return false; }
  int get_work_mode() const override { return 0; }
  bool is_logic_rendering_mode() const override { return true; }
  const std::vector<pv::data::PulseAnalyzer::Pulse> *
  get_preview_ranges(pv::view::LogicSignal *) const override {
    return nullptr;
  }

  // ---- formatting ----
  QString format_real_time(uint64_t, uint64_t) override { return QString(); }
  QString format_real_freq(uint64_t, uint64_t) override { return QString(); }
  QString format_freq(double) override { return QString(); }

  // ---- mutation entry points ----
  void set_back(bool) override {}
  bool back_ready() override { return true; }
  bool get_dso_trig_moved() override { return false; }
  void set_update_viewport(pv::view::IRenderViewport *, bool) override {}
  void request_repaint() override {}
  void request_delayed_update() override {}
  void request_decode_only_update() override {}
  void header_updated() override {}
  void repeat_unshow() override {}
  void show_captured_progress(bool, int) override {}
  void update_hori_res() override {}
  void vDial_updated() override {}
  void dso_factor_updated() override {}
  void signals_changed(const Trace *) override {}
  QString get_index_delta(uint64_t, uint64_t) override { return QString(); }
  void subscribe_resize(Trace *) override {}
  QObject *qt_object() override { return nullptr; }

private:
  std::list<std::unique_ptr<pv::view::Cursor>> _cursors;
  std::list<std::unique_ptr<pv::view::XCursor>> _xcursors;
  std::vector<std::unique_ptr<pv::view::Signal>> _own_signals;
  QPoint _hover;
  pv::SigSession *_session = nullptr; // 测试永不调用 session()
};

// 具体 Trace 子类：Trace 的构造函数是 protected 且带参，唯一纯虚成员是
// enabled()。本测试只关心 y 坐标，故这里给出最小可用实现。
class ProbeTrace : public Trace {
public:
  ProbeTrace() : Trace(QStringLiteral("probe"), 0, 10000) {
    set_totalHeight(kH);
  }

  bool enabled() override { return true; }

  /** 所有 ProbeTrace 等高，便于手算期望的相邻行中心距。 */
  static const int kH = 40;
  static int rowPitch() { return kH + 2 * pv::view::IRenderView::SignalMargin; }
};

/** 让位布局的相邻行额外间隙，与生产代码一致（两个 SignalMargin）。 */
static const int kRowGap = 2 * pv::view::IRenderView::SignalMargin;

} // namespace

class TestVOffsetAnimation : public QObject {
  Q_OBJECT

private slots:
  // -- 1. 无动画时两层同步 ----------------------------------------------
  void SetVOffsetSyncsVisualWhenNotAnimating();
  void SetVOffsetKeepsVisualWhenAnimating();

  // -- 2. 只写 layout 的入口 --------------------------------------------
  void SetVOffsetNoVisualSyncLeavesVisualUntouched();
  void SetVOffsetNoVisualSyncGivesAnimationSomethingToChase();

  // -- 3. force_to_v_offset（拖动跟手）----------------------------------
  void ForceSetsBothCoordinates();
  void ForceCancelsRunningAnimation();
  void ForceRepaintsView();

  // -- 4. animate_to_layout_v_offset ------------------------------------
  void AnimateIsNoOpWhenAlreadyThere();
  void AnimateIsNoOpWhenNoViewBound();
  void AnimateMovesVisualTowardTarget();
  void AnimateDoesNotInterpolateFromSentinel();
  void AnimateDoesNotInterpolateToSentinel();
  void IsAnimatingReflectsState();

  // -- 5. 默认值 / 常量 --------------------------------------------------
  void VisualOffsetStartsAtSentinel();
  void AnimationDurationMatchesPulseView();

  // -- 6. 让位布局：真正的交换语义（回归：整列跟着平移）------------------
  void MakeWayInsertsDraggedBelowNeighbor();
  void MakeWayInsertsDraggedAboveNeighbor();
  void MakeWaySwapsTwoRowsExactly();
  void MakeWayDoesNotTranslateWholeColumn();
  void MakeWayRealSwapNeedsOvershoot();
  void MakeWayAnchorMustStayFixedDuringDrag();
  void MakeWayAnchorResolutionIgnoresDraggedRow();
  void MakeWayAnchorFallsBackOnlyWhenAbsent();
  void MakeWayGivesDraggedNoTarget();
  void MakeWayKeepsOthersContiguousAndOrdered();
  void MakeWayAtTopAndBottomEdges();
  void MakeWayIsPermutation();
};

// ===========================================================================
// 1. 无动画时 set_v_offset() 必须同步两层
// ===========================================================================
void TestVOffsetAnimation::SetVOffsetSyncsVisualWhenNotAnimating() {
  ProbeTrace t;
  t.force_to_v_offset(100);
  QCOMPARE(t.get_v_offset(), 100);
  QCOMPARE(t.visual_v_offset(), 100);

  // 正常布局路径：没有动画在跑，两个坐标必须一起走，否则通道会画在旧位置。
  t.set_v_offset(250);
  QCOMPARE(t.get_v_offset(), 250);
  QCOMPARE(t.visual_v_offset(), 250);
  QCOMPARE(t.get_y(), 250);
  QCOMPARE(t.get_zero_vpos(), 250);
}

void TestVOffsetAnimation::SetVOffsetKeepsVisualWhenAnimating() {
  MockRenderView view;
  ProbeTrace t;
  t.set_view(&view);
  t.force_to_v_offset(0);

  // 起一段动画，visual 此时停在 0、目标是 300。
  t.set_v_offset_no_visual_sync(300);
  t.animate_to_layout_v_offset();
  QVERIFY(t.is_v_offset_animating());
  QCOMPARE(t.visual_v_offset(), 0);

  // 动画途中改目标：visual 必须留在原处，不能跟着 layout 瞬移过去。
  t.set_v_offset(400);
  QCOMPARE(t.get_v_offset(), 400);
  QCOMPARE(t.visual_v_offset(), 0);

  t.stop_v_offset_animation();
}

// ===========================================================================
// 2. set_v_offset_no_visual_sync —— 让位动画的前置条件
// ===========================================================================
void TestVOffsetAnimation::SetVOffsetNoVisualSyncLeavesVisualUntouched() {
  ProbeTrace t;
  t.force_to_v_offset(120);

  t.set_v_offset_no_visual_sync(999);
  QCOMPARE(t.get_v_offset(), 999);
  QCOMPARE(t.visual_v_offset(), 120); // 关键：visual 不动
}

void TestVOffsetAnimation::SetVOffsetNoVisualSyncGivesAnimationSomethingToChase() {
  MockRenderView view;
  ProbeTrace t;
  t.set_view(&view);
  t.force_to_v_offset(50);

  // 用错入口（set_v_offset，无动画时同步两层）就没有可动画的距离了 ——
  // 这正是让位动画必须走 no_visual_sync 的原因。
  t.set_v_offset(150);
  QCOMPARE(t.visual_v_offset(), 150); // 已经瞬移到位

  t.set_v_offset_no_visual_sync(300);
  QVERIFY(t.visual_v_offset() != t.get_v_offset()); // 有距离可动画
  t.animate_to_layout_v_offset();
  QVERIFY(t.is_v_offset_animating());
  t.stop_v_offset_animation();
}

// ===========================================================================
// 3. force_to_v_offset —— 被拖项严格跟手
// ===========================================================================
void TestVOffsetAnimation::ForceSetsBothCoordinates() {
  ProbeTrace t;
  t.force_to_v_offset(777);
  QCOMPARE(t.get_v_offset(), 777);
  QCOMPARE(t.visual_v_offset(), 777);
  QCOMPARE(t.get_y(), 777);
}

void TestVOffsetAnimation::ForceCancelsRunningAnimation() {
  MockRenderView view;
  ProbeTrace t;
  t.set_view(&view);
  t.force_to_v_offset(0);
  t.set_v_offset_no_visual_sync(500);
  t.animate_to_layout_v_offset();
  QVERIFY(t.is_v_offset_animating());

  // 拖动跟手：硬设必须把在跑的动画停掉，否则动画帧会覆盖 visual。
  t.force_to_v_offset(200);
  QVERIFY(!t.is_v_offset_animating());
  QCOMPARE(t.visual_v_offset(), 200);
  QCOMPARE(t.get_v_offset(), 200);
}

void TestVOffsetAnimation::ForceRepaintsView() {
  MockRenderView view;
  ProbeTrace t;
  t.set_view(&view);
  // force 不经过动画，故不触发 per-frame valueChanged 回调；它由 Header 在
  // 拖动循环里自行 update()。这里只确认强设不会误触发动画回调。
  const int before = view.repaint_count;
  t.force_to_v_offset(10);
  QCOMPARE(view.repaint_count, before);
}

// ===========================================================================
// 4. animate_to_layout_v_offset —— 终点/哨兵/热路径
// ===========================================================================
void TestVOffsetAnimation::AnimateIsNoOpWhenAlreadyThere() {
  ProbeTrace t;
  t.force_to_v_offset(321);

  // 每帧都会调用它：已在目标时必须直接返回，连动画对象都不该创建。
  t.animate_to_layout_v_offset();
  QVERIFY(!t.is_v_offset_animating());
  QCOMPARE(t.visual_v_offset(), 321);
}

void TestVOffsetAnimation::AnimateIsNoOpWhenNoViewBound() {
  ProbeTrace t; // 未 set_view
  t.force_to_v_offset(0);
  t.set_v_offset_no_visual_sync(200);

  // 无 View 时仍应推进（不崩），只是没有 repaint 回调。
  t.animate_to_layout_v_offset();
  QCOMPARE(t.get_v_offset(), 200);
  t.stop_v_offset_animation();
}

void TestVOffsetAnimation::AnimateMovesVisualTowardTarget() {
  MockRenderView view;
  ProbeTrace t;
  t.set_view(&view);
  t.force_to_v_offset(0);
  t.set_v_offset_no_visual_sync(400);

  const int before = view.repaint_count;
  t.animate_to_layout_v_offset();
  QVERIFY(t.is_v_offset_animating());

  // 跑完动画：visual 必须收敛到 layout 目标，且过程中触发过 repaint。
  QTRY_VERIFY_WITH_TIMEOUT(!t.is_v_offset_animating(), 2000);
  QCOMPARE(t.visual_v_offset(), 400);
  QCOMPARE(t.get_v_offset(), 400);
  QVERIFY(view.repaint_count > before);
}

void TestVOffsetAnimation::AnimateDoesNotInterpolateFromSentinel() {
  ProbeTrace t;
  // visual 停在 INT_MAX（尚未布局），目标有效：必须硬设，不能插值。
  QCOMPARE(t.visual_v_offset(), INT_MAX);
  t.set_v_offset_no_visual_sync(180);
  t.animate_to_layout_v_offset();
  QVERIFY(!t.is_v_offset_animating());
  QCOMPARE(t.visual_v_offset(), 180);
}

void TestVOffsetAnimation::AnimateDoesNotInterpolateToSentinel() {
  ProbeTrace t;
  t.force_to_v_offset(180);
  // 目标是 INT_MAX（尚未布局）：同样必须硬设回哨兵，不能插值。
  t.set_v_offset_no_visual_sync(INT_MAX);
  t.animate_to_layout_v_offset();
  QVERIFY(!t.is_v_offset_animating());
  QCOMPARE(t.visual_v_offset(), INT_MAX);
}

void TestVOffsetAnimation::IsAnimatingReflectsState() {
  MockRenderView view;
  ProbeTrace t;
  t.set_view(&view);
  QVERIFY(!t.is_v_offset_animating()); // 从未起过动画

  t.force_to_v_offset(0);
  t.set_v_offset_no_visual_sync(600);
  t.animate_to_layout_v_offset();
  QVERIFY(t.is_v_offset_animating());

  t.stop_v_offset_animation();
  QVERIFY(!t.is_v_offset_animating());
  QCOMPARE(t.get_v_offset(), 600); // 目标不变
}

// ===========================================================================
// 5. 默认值 / 常量
// ===========================================================================
void TestVOffsetAnimation::VisualOffsetStartsAtSentinel() {
  ProbeTrace t;
  // 新建 Trace 尚未参与布局：visual 是 INT_MAX 哨兵，而不是 0 ——
  // 若默认成 0，未布局的通道会全部叠在 y=0 上。
  QCOMPARE(t.visual_v_offset(), INT_MAX);
}

void TestVOffsetAnimation::AnimationDurationMatchesPulseView() {
  // 与 PulseView TraceTreeItem 的 100ms 保持一致，改这个值会同时改观感，
  // 故锁死以免被无意调整。
  QCOMPARE(Trace::kVOffsetAnimationMs, 100);
}

// ===========================================================================
// 6. 让位布局：交换语义
// ---------------------------------------------------------------------------
// 回归一个真实缺陷：早先的实现把"其余通道围绕被拖项对称摊开"，于是拖动一格时
// 整列一起平移、两个通道永远换不了位。下面这些用例锁死"必须发生真正的交换"。
// ===========================================================================

namespace {

/** 造 N 个等高的通道，中心依次落在 0, pitch, 2*pitch, … */
std::vector<std::unique_ptr<ProbeTrace>> make_rows(int n) {
  std::vector<std::unique_ptr<ProbeTrace>> rows;
  for (int i = 0; i < n; i++) {
    rows.push_back(std::make_unique<ProbeTrace>());
    rows.back()->force_to_v_offset(i * ProbeTrace::rowPitch());
  }
  return rows;
}

/** others = 除被拖项外的全部（按行中心升序）；被拖项用 visual 表达"现在拖到哪" */
std::vector<Trace *> others_of(
    const std::vector<std::unique_ptr<ProbeTrace>> &rows, Trace *dragged) {
  std::vector<Trace *> others;
  for (auto &r : rows)
    if (r.get() != dragged)
      others.push_back(r.get());
  return others;
}

/** 取 out_targets 里某通道的目标中心；没有则返回 INT_MIN */
int target_of(const std::vector<std::pair<Trace *, int>> &targets, Trace *t) {
  for (auto &p : targets)
    if (p.first == t)
      return p.second;
  return INT_MIN;
}

} // namespace

void TestVOffsetAnimation::MakeWayInsertsDraggedBelowNeighbor() {
  auto rows = make_rows(3);
  Trace *drag = rows[0].get();      // 拖第 0 行
  drag->set_visual_v_offset(ProbeTrace::rowPitch()); // 拖到第 1 行的位置上

  std::vector<Trace *> order;
  std::vector<std::pair<Trace *, int>> targets;
  pv::view::make_way::layout(others_of(rows, drag), drag,
                                rows[0]->get_v_offset(), kRowGap, order, targets);

  // 顺序应为 rows[1], rows[0], rows[2]（被拖项插到原第 1 行之前）
  QCOMPARE(order.size(), size_t(3));
  QCOMPARE(order[0], rows[1].get());
  QCOMPARE(order[1], drag);
  QCOMPARE(order[2], rows[2].get());
}

void TestVOffsetAnimation::MakeWayInsertsDraggedAboveNeighbor() {
  auto rows = make_rows(3);
  Trace *drag = rows[2].get();                        // 拖第 2 行
  // 插位探针是严格的 `>`：只有视觉位置**小于**第 1 行的中心，才算"插入其上方"。
  // 正好相等时被拖项还排在第 1 行之后（它尚未越过邻居），那是正确语义。
  drag->set_visual_v_offset(ProbeTrace::rowPitch() - 1);

  std::vector<Trace *> order;
  std::vector<std::pair<Trace *, int>> targets;
  pv::view::make_way::layout(others_of(rows, drag), drag,
                                rows[0]->get_v_offset(), kRowGap, order, targets);

  // 顺序应为 rows[0], rows[2], rows[1]（被拖项插到原第 1 行之前）
  QCOMPARE(order.size(), size_t(3));
  QCOMPARE(order[0], rows[0].get());
  QCOMPARE(order[1], drag);
  QCOMPARE(order[2], rows[1].get());
}

void TestVOffsetAnimation::MakeWaySwapsTwoRowsExactly() {
  // 两行互换是最小可复现：把第 0 行拖过第 1 行。
  auto rows = make_rows(2);
  Trace *drag = rows[0].get();
  drag->set_visual_v_offset(ProbeTrace::rowPitch());

  std::vector<Trace *> order;
  std::vector<std::pair<Trace *, int>> targets;
  pv::view::make_way::layout(others_of(rows, drag), drag,
                                rows[0]->get_v_offset(), kRowGap, order, targets);

  // ★ 核心断言：第 1 行必须被移到"第 0 行原来的格位"，也就是 0 号中心。
  // 旧实现下这一项会保持不动（整列平移），或被推到更远 —— 两种情况都失败。
  QCOMPARE(target_of(targets, rows[1].get()), 0);
  // 被拖项继续跟手，所以**不该**有目标。
  QCOMPARE(target_of(targets, drag), INT_MIN);
}

void TestVOffsetAnimation::MakeWayDoesNotTranslateWholeColumn() {
  // 三行：拖第 0 行到第 1 行的位置上。
  //
  // 物理上此刻的绘制顺序是 rows[1] / rows[0]（跟手，y=54）/ rows[2]：被拖项**仍然
  // 占着一个槽位**（它没有消失，只是下移了一格），所以第 2 行理应留在槽 2（y=108）
  // 不动。判据是"谁动了、谁没动"：
  //   - rows[1] 从 54 顶到 0（让出了上面一格）
  //   - rows[2] **原地不动**（整列没有被平移）
  // 旧实现下 rows[2] 会跟着挪到 54，即"整列平移" —— 那才是用户报的缺陷。
  auto rows = make_rows(3);
  Trace *drag = rows[0].get();
  drag->set_visual_v_offset(ProbeTrace::rowPitch());

  std::vector<Trace *> order;
  std::vector<std::pair<Trace *, int>> targets;
  const int top = rows[0]->get_v_offset();
  pv::view::make_way::layout(others_of(rows, drag), drag, top, kRowGap,
                                order, targets);

  const int pitch = ProbeTrace::rowPitch();
  // 第 1 行顶到第 0 行原位。
  QCOMPARE(target_of(targets, rows[1].get()), top);
  // 第 2 行**留在原位**（top + 2*pitch），不是被整体平移下去、也不是被顶上来。
  QCOMPARE(target_of(targets, rows[2].get()), top + 2 * pitch);
}

void TestVOffsetAnimation::MakeWayRealSwapNeedsOvershoot() {
  // 说明插位阈值：探针是 `others[i]->get_v_offset() > drag_visual`，即**行中心连
  // 线**就是阈值。
  //   - 被拖项中心仍在邻居中心**之上**（严格小于）→ 排邻居之前，不交换；
  //   - 一旦到达/越过邻居中心（>=）→ 插到邻居之后，邻居被顶上去 → 交换成立。
  // 也就是说交换不需要"拖过整整一格再多"，到达邻居中心线即触发。
  auto rows = make_rows(2);
  const int pitch = ProbeTrace::rowPitch();
  Trace *drag = rows[0].get();
  const int top = drag->get_v_offset();

  // (a) 中心仍在邻居中心之上（差 1px）→ 尚未到达中心线，顺序不变、谁都不动。
  drag->set_visual_v_offset(pitch - 1);
  {
    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others_of(rows, drag), drag, top, kRowGap, order,
                               targets);
    QCOMPARE(order[0], drag);                                 // 仍排第一
    QCOMPARE(target_of(targets, rows[1].get()), top + pitch); // 原位不动
    QCOMPARE(target_of(targets, drag), INT_MIN);
  }

  // (b) 到达邻居中心线 → rows[1] 被顶到槽 0，被拖项排到其后 → 真正的交换。
  drag->set_visual_v_offset(pitch);
  {
    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others_of(rows, drag), drag, top, kRowGap, order,
                               targets);
    QCOMPARE(order[0], rows[1].get());
    QCOMPARE(order[1], drag);
    QCOMPARE(target_of(targets, rows[1].get()), top);
    QCOMPARE(target_of(targets, drag), INT_MIN);
  }
}

void TestVOffsetAnimation::MakeWayAnchorMustStayFixedDuringDrag() {
  // ★ 用户报的原始缺陷：拖动**最上面**那行时，其余通道跟着整列平移，永远换不了位。
  //
  // 根因不是插位，而是锚点：若锚点取"当前其它通道的最小 y"，被拖项一离开首槽，
  // 锚点就从 0 跳到 pitch，整列下移一格。因此生产代码在 mousePressEvent 里把锚点
  // 固定下来整段拖动复用。
  //
  // 不变量：**槽位网格恒为 {anchor + k*pitch | k = 0..n-1}**。每帧里被拖项占一个
  // 槽、其余 n-1 行占其余槽，两者并起来正好覆盖全部 n 个槽、位置逐帧不变。
  // 若锚点漂移，网格会整体平移 → 该断言立刻失败。
  auto rows = make_rows(4);
  const int pitch = ProbeTrace::rowPitch();
  const int n = static_cast<int>(rows.size());
  const int anchor = rows[0]->get_v_offset(); // == 0，拖动开始时算一次

  Trace *drag = rows[0].get();
  for (int step = 0; step <= 3; step++) { // 逐帧：拖过 0、1、2、3 格
    drag->set_visual_v_offset(step * pitch + (step > 0 ? 1 : 0)); // 严格越过

    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others_of(rows, drag), drag, anchor, kRowGap,
                               order, targets);

    // 被拖项插到了索引 step 处（越过 step 个邻居之后）。
    QCOMPARE(order[static_cast<size_t>(step)], drag);

    // 其余 n-1 行的目标必须是**网格上互不相同的槽**，且全部落在 [anchor, anchor+(n-1)*pitch]。
    std::vector<int> slot_y;
    for (auto &p : targets)
      slot_y.push_back(p.second);
    std::stable_sort(slot_y.begin(), slot_y.end());
    QCOMPARE(slot_y.size(), static_cast<size_t>(n - 1));
    for (size_t i = 0; i < slot_y.size(); i++) {
      // 每个目标都精确落在网格点上（不是被整体平移的浮点位置）。
      QCOMPARE((slot_y[i] - anchor) % pitch, 0);
      QVERIFY(slot_y[i] >= anchor);
      QVERIFY(slot_y[i] <= anchor + (n - 1) * pitch);
    }
    for (size_t i = 1; i < slot_y.size(); i++)
      QVERIFY(slot_y[i] > slot_y[i - 1]); // 严格递增 = 各占不同槽
  }
}

void TestVOffsetAnimation::MakeWayGivesDraggedNoTarget() {
  auto rows = make_rows(4);
  Trace *drag = rows[1].get();
  drag->set_visual_v_offset(2 * ProbeTrace::rowPitch());

  std::vector<Trace *> order;
  std::vector<std::pair<Trace *, int>> targets;
  pv::view::make_way::layout(others_of(rows, drag), drag,
                                rows[0]->get_v_offset(), kRowGap, order, targets);

  // 被拖项必须始终由 force_to_v_offset 跟手，绝不能出现在动画目标里。
  QCOMPARE(target_of(targets, drag), INT_MIN);
  QCOMPARE(targets.size(), size_t(3)); // 其余 3 个各有目标
}

void TestVOffsetAnimation::MakeWayKeepsOthersContiguousAndOrdered() {
  auto rows = make_rows(4);
  Trace *drag = rows[3].get();                    // 从最底拖到最顶
  drag->set_visual_v_offset(-ProbeTrace::rowPitch());

  std::vector<Trace *> order;
  std::vector<std::pair<Trace *, int>> targets;
  const int top = rows[0]->get_v_offset();
  pv::view::make_way::layout(others_of(rows, drag), drag, top, kRowGap,
                                order, targets);

  // 其余通道按顺序排在 top, top+pitch, top+2*pitch —— 间隔恒定 = 真让位。
  const int pitch = ProbeTrace::rowPitch();
  QCOMPARE(order[0], drag); // 拖到最顶后应排第一
  QCOMPARE(target_of(targets, rows[0].get()), top + pitch);
  QCOMPARE(target_of(targets, rows[1].get()), top + 2 * pitch);
  QCOMPARE(target_of(targets, rows[2].get()), top + 3 * pitch);
}

void TestVOffsetAnimation::MakeWayAtTopAndBottomEdges() {
  auto rows = make_rows(3);
  const int pitch = ProbeTrace::rowPitch();
  const int top = rows[0]->get_v_offset();

  // 边界 1：拖过头顶（visual 远在顶部之上）→ 插到最前。
  {
    Trace *drag = rows[1].get();
    drag->set_visual_v_offset(-10000);
    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others_of(rows, drag), drag, top, kRowGap,
                                    order, targets);
    QCOMPARE(order[0], drag);
    QCOMPARE(target_of(targets, rows[0].get()), top + pitch);
  }

  // 边界 2：拖过底部（visual 远在底部之下）→ 插到最后。
  {
    Trace *drag = rows[1].get();
    drag->set_visual_v_offset(10000);
    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others_of(rows, drag), drag, top, kRowGap,
                                    order, targets);
    QCOMPARE(order.back(), drag);
    QCOMPARE(target_of(targets, rows[2].get()), top + pitch);
  }
}

void TestVOffsetAnimation::MakeWayIsPermutation() {
  // 无论插到哪，结果都必须是"输入集合的一个排列"：不丢项、不重复。
  auto rows = make_rows(5);
  const int pitch = ProbeTrace::rowPitch();
  for (int k = -2; k <= 6; k++) {
    Trace *drag = rows[2].get();
    drag->set_visual_v_offset(k * pitch);

    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others_of(rows, drag), drag,
                                    rows[0]->get_v_offset(), kRowGap, order,
                                    targets);

    QCOMPARE(order.size(), rows.size());
    auto sorted = order;
    std::stable_sort(sorted.begin(), sorted.end());
    std::vector<Trace *> expect;
    for (auto &r : rows)
      expect.push_back(r.get());
    std::stable_sort(expect.begin(), expect.end());
    QCOMPARE(sorted, expect);

    // 目标中心必须严格单调递增（其余通道不得互相重叠）。
    std::vector<std::pair<Trace *, int>> byY = targets;
    std::stable_sort(byY.begin(), byY.end(),
                     [](const auto &a, const auto &b) { return a.second < b.second; });
    for (size_t i = 1; i < byY.size(); i++)
      QVERIFY(byY[i].second > byY[i - 1].second);
  }
}

void TestVOffsetAnimation::MakeWayAnchorResolutionIgnoresDraggedRow() {
  // ★★ 复现"拖最顶上那行 → 整列跟着平移"的缺陷根因，且**必须打到生产锚点选择**。
  //
  // 生产不再现算锚点，而是用 make_way::resolve_anchor(remembered, all_rows)：
  //   remembered != INT_MAX → 返回 remembered（拖动期间恒定，正确）
  //   remembered == INT_MAX → 回退到"全部行的最小 y"（非拖动调用方）
  // 缺陷版本等价于"拿 others（剔除被拖项）的最小 y 当锚点"，于是被拖项一离开
  // 首槽，锚点就 +1 个 pitch，整列下移 —— 这正是用户看到的整列平移。
  auto rows = make_rows(4);
  const int pitch = ProbeTrace::rowPitch();
  std::vector<Trace *> all;
  for (auto &r : rows)
    all.push_back(r.get());

  // 拖动开始时算一次 → 必须是列顶（0），与"谁被拖"无关。
  const int remembered = pv::view::make_way::anchor_from_rows(all);
  QCOMPARE(remembered, 0);

  // 逐帧：被拖项是**最顶**那一行，它的 layout 已被 force_to_v_offset 改写成跟手值。
  Trace *drag = rows[0].get();
  for (int step = 1; step <= 3; step++) {
    drag->force_to_v_offset(step * pitch); // 模拟跟手（同步改写 layout + visual）

    // 缺陷路径：锚点取自"剔除被拖项后的最小 y" → 随 step 漂移（54/108/162）。
    std::vector<Trace *> others = others_of(rows, drag);
    const int drifting = pv::view::make_way::top_center(others);
    QVERIFY(drifting != remembered); // 证明：这个量**确实**不是稳定锚点

    // 生产路径：resolve_anchor 优先用 remembered，全程恒为 0。
    const int anchor =
        pv::view::make_way::resolve_anchor(remembered, all);
    QCOMPARE(anchor, remembered);

    // 用生产锚点布局：槽位网格不动，整列不会随 step 平移。
    std::vector<Trace *> order;
    std::vector<std::pair<Trace *, int>> targets;
    pv::view::make_way::layout(others, drag, anchor, kRowGap, order, targets);

    std::vector<int> slot_y;
    for (auto &p : targets)
      slot_y.push_back(p.second);
    std::stable_sort(slot_y.begin(), slot_y.end());
    QCOMPARE(slot_y.size(), rows.size() - 1);
    for (size_t i = 0; i < slot_y.size(); i++) {
      QCOMPARE((slot_y[i] - remembered) % pitch, 0);
      QVERIFY(slot_y[i] >= remembered);
      QVERIFY(slot_y[i] <= remembered + static_cast<int>(rows.size() - 1) * pitch);
    }
    // 被拖项按 step 插到正确索引。
    QCOMPARE(order[static_cast<size_t>(step)], drag);
  }
}

void TestVOffsetAnimation::MakeWayAnchorFallsBackOnlyWhenAbsent() {
  // 回退分支：非拖动调用方（无记忆锚点）才允许现算，且结果等于全部行的最小 y。
  auto rows = make_rows(3);
  std::vector<Trace *> all;
  for (auto &r : rows)
    all.push_back(r.get());

  QCOMPARE(pv::view::make_way::resolve_anchor(INT_MAX, all),
           pv::view::make_way::anchor_from_rows(all));
  QCOMPARE(pv::view::make_way::resolve_anchor(INT_MAX, all), 0);

  // 有记忆锚点时**必须**原样返回，即使当前布局的最小 y 已经不同（拖动中就是如此）。
  Trace *drag = rows[0].get();
  drag->force_to_v_offset(pv::view::make_way::anchor_from_rows(all) - 500);
  QCOMPARE(pv::view::make_way::resolve_anchor(0, all), 0);
}

QTEST_MAIN(TestVOffsetAnimation)
#include "test_v_offset_animation.moc"
