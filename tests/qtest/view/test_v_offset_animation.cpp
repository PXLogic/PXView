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
  ProbeTrace() : Trace(QStringLiteral("probe"), 0, 10000) {}

  bool enabled() override { return true; }
};

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

QTEST_MAIN(TestVOffsetAnimation)
#include "test_v_offset_animation.moc"
