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

#ifndef PXVIEW_PV_VIEW_IVIEW_DELEGATES_H
#define PXVIEW_PV_VIEW_IVIEW_DELEGATES_H

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include "pv/base/pxvdef.h"
#include "pv/data/decoderanalogdata.h"
#include "pv/data/pulse_analyzer.h"

class QPainter;
class QPaintDevice;
class QObject;

namespace pv {

class SigSession;

namespace data {
class DataSource;
} // namespace data

namespace view {

class Trace;
class Cursor;
class XCursor;
class Signal;
class LogicSignal;
class LissajousTrace;

class IRenderViewport; // declared below; referenced by IRenderView

/**
 * @brief Read-only interface to ViewLayout state.
 *
 * Phase 8 (Testability): extracted so that ViewCursors, ViewSignalSync,
 * ViewDerivedTraces, ViewDataSync, ViewportPainter, and RenderPasses can
 * depend on this abstract interface instead of the concrete ViewLayout /
 * View classes. In unit tests, a mock implementation (MockViewLayout)
 * can be substituted — eliminating the need for a real QWidget-based View.
 *
 * ViewLayout already implements all these methods; the interface simply
 * formalizes the contract. Delegates that only need layout state should
 * accept IViewLayout* instead of View*.
 */
class IViewLayout {
public:
  virtual ~IViewLayout() = default;

  // -- Scale / offset state (read) --
  virtual double scale() const = 0;
  virtual int64_t offset() const = 0;
  virtual double maxscale() const = 0;
  virtual double minscale() const = 0;

  // -- Signal height state (read) --
  virtual int spanY() const = 0;
  virtual int signalHeight() const = 0;
  virtual int signalHeightScale() const = 0;

  // -- DSO zoom state (read) --
  virtual double dso_zoom_factor() const = 0;

  // -- Scale / offset mutation --
  virtual void set_scale_offset(double scale, int64_t offset) = 0;

  // -- Offset bounds (read) --
  virtual int64_t get_max_offset() = 0;
  virtual int64_t get_min_offset() = 0;

  // -- Scroll layout (read) --
  virtual void get_scroll_layout(int64_t &length, int64_t &offset) = 0;
};

/**
 * @brief Read-only interface to ViewCursors state.
 *
 * Phase 8 (Testability): allows other delegates and rendering code
 * to query cursor state without depending on the concrete ViewCursors
 * class.
 */
class IViewCursors {
public:
  virtual ~IViewCursors() = default;

  virtual bool cursors_shown() const = 0;
  virtual bool trig_cursor_shown() const = 0;
  virtual bool search_cursor_shown() const = 0;
  virtual bool xcursors_shown() const = 0;
};

/**
 * @brief Read-only interface to ViewSignalSync state.
 *
 * Phase 8 (Testability): allows rendering code and tests to query
 * the signal list without depending on the concrete ViewSignalSync
 * class.
 */
class IViewSignalStore {
public:
  virtual ~IViewSignalStore() = default;

  virtual size_t signal_count() const = 0;
  virtual bool rebuild_in_progress() const = 0;
};

/**
 * @brief Mock implementation of IViewLayout for unit testing.
 *
 * Phase 8 (Testability): provides a simple in-memory implementation
 * that can be used in unit tests for delegates (ViewportPainter,
 * RenderPasses, etc.) without creating a real View widget.
 *
 * Usage:
 *   MockViewLayout layout;
 *   layout.set_scale(10.0);
 *   layout.set_offset(0);
 *   // pass &layout to delegate under test
 */
class MockViewLayout : public IViewLayout {
public:
  MockViewLayout() = default;

  // -- Test setters --
  void set_scale(double s) { _scale = s; }
  void set_offset(int64_t o) { _offset = o; }
  void set_maxscale(double s) { _maxscale = s; }
  void set_minscale(double s) { _minscale = s; }
  void set_spanY(int s) { _spanY = s; }
  void set_signalHeight(int h) { _signalHeight = h; }
  void set_signalHeightScale(int s) { _signalHeightScale = s; }
  void set_dso_zoom_factor(double f) { _dso_zoom_factor = f; }
  void set_max_offset(int64_t v) { _max_offset = v; }
  void set_min_offset(int64_t v) { _min_offset = v; }

  // -- IViewLayout overrides --
  double scale() const override { return _scale; }
  int64_t offset() const override { return _offset; }
  double maxscale() const override { return _maxscale; }
  double minscale() const override { return _minscale; }
  int spanY() const override { return _spanY; }
  int signalHeight() const override { return _signalHeight; }
  int signalHeightScale() const override { return _signalHeightScale; }
  double dso_zoom_factor() const override { return _dso_zoom_factor; }
  void set_scale_offset(double scale, int64_t offset) override {
    _scale = scale;
    _offset = offset;
  }
  int64_t get_max_offset() override { return _max_offset; }
  int64_t get_min_offset() override { return _min_offset; }
  void get_scroll_layout(int64_t &length, int64_t &offset) override {
    length = _scroll_length;
    offset = _offset;
  }

  void set_scroll_length(int64_t l) { _scroll_length = l; }

private:
  double _scale = 10.0;
  int64_t _offset = 0;
  double _maxscale = 1e9;
  double _minscale = 1e-15;
  int _spanY = 0;
  int _signalHeight = 0;
  int _signalHeightScale = 24;
  double _dso_zoom_factor = 1.0;
  int64_t _max_offset = 0;
  int64_t _min_offset = 0;
  int64_t _scroll_length = 0;
};

// ---------------------------------------------------------------------------
// Widget-free render interfaces (QML migration Phase 3, Task 3.1).
//
// IRenderView / IRenderViewport formalize the *rendering* subset of the
// View (QScrollArea) / Viewport (QWidget) surfaces so that pxview-render
// translation units (render_pass.cpp, viewport_painter.cpp, trace/signal
// paint code) never include view.h / viewport.h. View and Viewport
// implement them; RenderContext holds IRenderView* / IRenderViewport*.
//
// Method signatures intentionally mirror the existing View/Viewport
// accessors (same names, same reference-returning state semantics) so the
// render call sites keep their shape — only the static type changes.
// ---------------------------------------------------------------------------

// Action / measure enumerators — promoted from Viewport's header to this
// widget-free header so RenderPass code can use NO_ACTION / LOGIC_FREQ ...
// without including viewport.h (QWidget).
enum ActionType {
  NO_ACTION,
  CURS_MOVE,
  LOGIC_EDGE,
  LOGIC_MOVE,
  LOGIC_ZOOM,
  LOGIC_JUMP,
  RESIZE_SIGNAL,
  DSO_XM_STEP0,
  DSO_XM_STEP1,
  DSO_XM_STEP2,
  DSO_YM,
  DSO_TRIG_MOVE,
  ANALOG_RANGE_DRAG
};

enum MeasureType { NO_MEASURE, LOGIC_FREQ, LOGIC_EDGE_CNT, DSO_VALUE };

struct AnalogMeasurementV2Options {
  bool show_channel = true;
  bool show_time = true;
  bool show_normalized = true;
  bool show_engineering_value = true;
  bool rise_time = true;
  bool fall_time = true;
  bool positive_overshoot = true;
  bool negative_overshoot = true;
  bool period = true;
  bool frequency = true;
  bool positive_width = true;
  bool negative_width = true;
  bool positive_duty_cycle = true;
  bool negative_duty_cycle = true;
  bool cycle_rms = true;
};

// Trace grouping (moved verbatim from view.h so render passes can consume
// group geometry without the QScrollArea header).
struct SignalGroup {
  int group_id;
  std::vector<Trace *> traces;
  SignalGroup() : group_id(-1) {}
};

/**
 * @brief Widget-free rendering services of the View (QScrollArea).
 *
 * Implemented by pv::view::View. Everything the render pipeline queries
 * from the view (scale/offset state, cursor lists, card colors, session
 * status queries, ruler time formatting) goes through this interface.
 */
class IRenderView {
public:
  virtual ~IRenderView() = default;

  // ---- Canonical render constants (single source of truth) ----
  // View re-exposes these under View:: names via aliases.
  static constexpr int ForeAlpha = 200;
  static constexpr int BackAlpha = 100;
  static constexpr int GroupGap = 10;
  static constexpr int GroupCardRadius = 6;
  static constexpr int SignalMargin = 7;

  // ---- Scale / offset / layout state ----
  virtual double scale() = 0;
  virtual int64_t offset() = 0;
  virtual int get_signalHeight() = 0;
  virtual int get_vOffset() = 0;
  virtual double trig_hoff() = 0;
  virtual void set_trig_hoff(double hoff) = 0;

  // ---- Cursor visibility ----
  virtual bool cursors_shown() = 0;
  virtual bool trig_cursor_shown() = 0;
  virtual bool search_cursor_shown() = 0;
  virtual bool xcursors_shown() = 0;

  // ---- Cursor / xcursor lists ----
  virtual std::list<std::unique_ptr<Cursor>> &get_cursorList() = 0;
  virtual std::list<std::unique_ptr<XCursor>> &get_xcursorList() = 0;
  virtual Cursor *get_trig_cursor() = 0;
  virtual Cursor *get_search_cursor() = 0;
  virtual uint64_t get_cursor_samples(int index) = 0;
  virtual int get_cursor_index_by_key(uint64_t key) = 0;

  // ---- Geometry / coordinate conversion ----
  virtual QRect get_view_rect() = 0;
  virtual int get_view_width() = 0;
  virtual int get_view_height() = 0;
  // QScrollArea inner-viewport widget geometry (Trace::get_view_rect uses it;
  // floating measurement panels clamp against it).
  virtual int scroll_viewport_width() = 0;
  virtual int scroll_viewport_height() = 0;
  virtual QPoint &hover_point() = 0;
  virtual double index2pixel(uint64_t index, bool has_hoff = true) = 0;
  virtual uint64_t pixel2index(double pixel) = 0;

  // ---- Trace / signal access ----
  virtual void get_traces(int type, std::vector<Trace *> &traces) = 0;
  virtual std::vector<std::unique_ptr<Signal>> &get_own_signals() = 0;
  virtual LissajousTrace *get_own_lissajous_trace() = 0;
  virtual const std::vector<SignalGroup> &get_signal_groups() = 0;

  // ---- Card / theme colors ----
  virtual bool is_colored_card_mode() = 0;
  virtual QColor get_group_card_color() = 0;
  virtual QColor get_trace_card_color(Trace *trace) = 0;
  // Live theme signal colors (View::Orange/Purple/LightBlue/Green/Blue are
  // refreshed at runtime by refreshSignalColors — read through accessors so
  // render code sees theme switches without including view.h).
  virtual QColor theme_red() = 0;
  virtual QColor theme_orange() = 0;
  virtual QColor theme_blue() = 0;
  virtual QColor theme_green() = 0;
  virtual QColor theme_purple() = 0;
  virtual QColor theme_lightblue() = 0;

  // ---- Session / data source binding ----
  virtual SigSession &session() = 0;
  virtual pv::data::DataSource *data_source() = 0;
  virtual pv::data::DataSource *document_snapshot_source() = 0;
  virtual bool display_source_is_document() = 0;
  // per-tab 显示状态（数据模型重构澄清）：渲染管线的显示判定走本视图状态，
  // 全局 session 状态保留为执行层语义。View 转发 ViewDataSync 的字段。
  virtual bool is_stopped_status() const = 0;
  virtual bool is_running_status() const = 0;
  virtual bool is_init_status() const = 0;
  virtual int get_work_mode() const = 0;
  virtual bool is_logic_rendering_mode() const = 0;

  // ---- Glitch filter preview ranges (LogicSignal overlay) ----
  virtual const std::vector<pv::data::PulseAnalyzer::Pulse> *
  get_preview_ranges(LogicSignal *sig) const = 0;

  // ---- Time formatting (Ruler statics, stateless) ----
  virtual QString format_real_time(uint64_t delta_index,
                                   uint64_t sample_rate) = 0;
  virtual QString format_real_freq(uint64_t delta_index,
                                  uint64_t sample_rate) = 0;
  virtual QString format_freq(double period) = 0;

  // ---- View mutation entry points used by paint paths ----
  virtual void set_back(bool ready) = 0;
  virtual bool back_ready() = 0;
  virtual bool get_dso_trig_moved() = 0;
  virtual void set_update_viewport(IRenderViewport *viewport,
                                   bool need_update) = 0;
  virtual void request_repaint() = 0;
  virtual void request_delayed_update() = 0;
  virtual void request_decode_only_update() = 0;
  virtual void header_updated() = 0;
  virtual void repeat_unshow() = 0;
  virtual void show_captured_progress(bool triggered, int progress) = 0;
  virtual void update_hori_res() = 0;
  virtual void vDial_updated() = 0;
  virtual void dso_factor_updated() = 0;
  virtual void signals_changed(const Trace *eventTrace) = 0;
  virtual QString get_index_delta(uint64_t start, uint64_t end) = 0;
  // Connect a trace's resize() slot to the view's resize() signal.
  // (String-based so the widget-free side needs no &View::resize member
  // pointer; implemented in view.cpp with the concrete types complete.)
  virtual void subscribe_resize(Trace *trace) = 0;
  // Escape hatch for GUI-side consumers that need the concrete widget
  // identity (qobject_cast target). Returns the View object itself.
  virtual QObject *qt_object() = 0;

  // ---- DSO autoset toolbar linkage (Task 3.2) ----
  // DsoMeasure::auto_set/autoV_end/auto_start drive the GUI View's sampling
  // toolbar (hori-res knob, zoom, auto-trigger status). View implements
  // these with its existing same-signature members; front-ends without a
  // toolbar (QML shell) inherit the no-op defaults — the autoset paths are
  // armed only from GUI interactions. Signatures MUST match View's members
  // exactly so the implicit-override wiring keeps working.
  virtual double get_hori_res() { return 0; }
  virtual void zoom(double steps) { (void)steps; }
  virtual void auto_trig(int index) { (void)index; }
};

/**
 * @brief Widget-free rendering state of the Viewport (QWidget).
 *
 * Implemented by pv::view::Viewport. Covers the cached signal pixmap,
 * per-frame paint bookkeeping, measurement/measure-overlay state and the
 * paint-device bridge. Interaction-only state (menus, timers for drag,
 * hover signals...) stays on the concrete Viewport.
 */
class IRenderViewport {
public:
  virtual ~IRenderViewport() = default;

  // ---- Canonical render constants (single source of truth) ----
  static constexpr int HitCursorMargin = 10;
  static constexpr int DsoMeasureStages = 3;

  virtual View_type type() const = 0;
  virtual IRenderView &view() = 0;

  // ---- Widget bridge (paint device / geometry / style) ----
  virtual int widget_width() const = 0;
  virtual int widget_height() const = 0;
  virtual QSize widget_size() const = 0;
  virtual double device_pixel_ratio() const = 0;
  virtual QPaintDevice *paint_device() = 0;
  // Draws the widget's styled background (QStyle PE_Widget primitive).
  virtual void paint_widget_background(QPainter &p) = 0;
  virtual QColor fore_color() const = 0;
  virtual QColor back_color() const = 0;
  virtual QFont application_font() const = 0;

  // ---- Cached signal pixmap (SignalPixmapPass) ----
  virtual QPixmap &pixmap() = 0;
  virtual bool &need_update() = 0;
  virtual bool take_decode_only_paint() = 0;
  virtual double &curScale() = 0;
  virtual int64_t &curOffset() = 0;
  virtual int &curSignalHeight() = 0;
  virtual int &curVOffset() = 0;

  // ---- Frame timing bookkeeping (paintEvent) ----
  virtual QElapsedTimer &frame_interval_timer() = 0;
  virtual bool &is_idle() = 0;
  virtual int &paint_in_this_second() = 0;
  virtual int &max_frame_time() = 0;

  // ---- Drag snapshot state (paintEvent fast path) ----
  virtual bool &drag_active() = 0;
  virtual QPixmap &drag_snapshot() = 0;
  virtual QPoint &mouse_point() = 0;
  virtual QPoint &mouse_down_point() = 0;

  // ---- Measure / hover state (MeasureOverlayPass) ----
  virtual ActionType &action_type() = 0;
  virtual MeasureType &measure_type() = 0;
  virtual bool &measure_en() = 0;
  virtual bool &dso_xm_valid() = 0;
  virtual int &dso_xm_y() = 0;
  virtual uint64_t *dso_xm_indices() = 0;
  virtual bool &dso_ym_valid() = 0;
  virtual uint16_t &dso_ym_sig_index() = 0;
  virtual double &dso_ym_sig_value() = 0;
  virtual uint64_t &dso_ym_index() = 0;
  virtual int &dso_ym_start() = 0;
  virtual int &dso_ym_end() = 0;
  virtual double &cur_preX() = 0;
  virtual double &cur_aftX() = 0;
  virtual double &cur_thdX() = 0;
  virtual int &cur_midY() = 0;
  virtual int &cur_preY() = 0;
  virtual int &cur_aftY() = 0;
  virtual bool &edge_hit() = 0;
  virtual QString &mm_width() = 0;
  virtual QString &mm_period() = 0;
  virtual QString &mm_freq() = 0;
  virtual QString &mm_duty() = 0;
  virtual uint64_t &thd_sample() = 0;
  virtual uint64_t &edge_start() = 0;
  virtual uint64_t &edge_end() = 0;
  virtual QString &em_edges() = 0;
  virtual QString &em_rising() = 0;
  virtual QString &em_falling() = 0;
  virtual bool &hover_hit() = 0;
  virtual std::shared_ptr<pv::data::DecoderAnalogData> &
  analog_measure_data() = 0;
  virtual int &analog_measure_channel() = 0;
  virtual uint64_t &analog_measure_start() = 0;
  virtual uint64_t &analog_measure_end() = 0;
  virtual pv::data::DecoderAnalogStatistics &analog_measure_stats() = 0;
  virtual pv::data::DecoderAnalogCycleMetrics &analog_measure_cycle() = 0;
  virtual bool &analog_measure_valid() = 0;
  virtual AnalogMeasurementV2Options &analog_measure_options() = 0;
  virtual QColor panelBgColor() const = 0;
  virtual QColor panelTextColor() const = 0;

  // ---- Trigger info state (TriggerInfoPass) ----
  virtual int &waiting_trig() = 0;
  virtual int &tigger_wait_times() = 0;
  virtual std::chrono::high_resolution_clock::time_point &
  lst_wait_tigger_time() = 0;
  virtual bool &transfer_started() = 0;
  virtual int &timer_cnt() = 0;

  // ---- Notifications / misc ----
  virtual void get_captured_progress(double &progress, int &progress100) = 0;
  virtual void notify_prg_rate(int progress) = 0;
  virtual void notify_measure_updated() = 0;
  virtual const QColor &probe_color(int idx) const = 0;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_IVIEW_DELEGATES_H
