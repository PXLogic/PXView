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

#include <cassert>
#include <cmath>
#include <limits.h>
#include <memory>
#include <cstring>

#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QSizeF>
#include <QtGlobal>
#include <algorithm>

#include "pv/api/types.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/component/devmode.h"
#include "pv/view/component/glitchfilterpopup.h"
#include "pv/view/component/header.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/view/trace/mathtrace.h"
#include "pv/view/component/ruler.h"
#include "pv/view/signal/signal.h"
#include "pv/view/trace/spectrumtrace.h"
#include "pv/view/trace/trace.h"
#include "pv/view/view.h"
#include "pv/view/view_cursors.h"
#include "pv/view/view_data_sync.h"
#include "pv/view/view_derived_traces.h"
#include "pv/view/view_glitch_filter.h"
#include "pv/view/view_layout.h"
#include "pv/view/view_signal_sync.h"
#include "pv/view/component/viewstatus.h"
#include "pv/view/viewport/viewport.h"
#include "pv/toolbars/samplingbar.h"

#include "pv/mainwindow/appcontrol.h"
#include "pv/config/appconfig.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/stack/spectrumstack.h"
#include "pv/dialogs/lissajousoptions.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/base/perflog.h"
#include <string>
#include <thread>

#ifdef _WIN32
// P3-D6: process-CPU sampling (GetProcessTimes) for the event-lag timer, to
// tell "main thread starved by decode threads saturating all cores" apart from
// "main thread busy in an uninstrumented function". WIN32_LEAN_AND_MEAN avoids
// winsock shadowing Qt's connect; NOMINMAX avoids the min/max macros.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "pv/session/sigsession.h"
#include "pv/widgets/hoversplitter.h"

using namespace std;

namespace pv {
namespace view {

const int View::LabelMarginWidth = 70;
const int View::RulerHeight = 50;

const int View::MaxScrollValue = INT_MAX / 2;
const int View::MaxHeightUnit = 24;
const int View::MinSignalHeight = 10;
const int View::MaxSignalHeight = 500;

// const int View::SignalHeight = 30;s
const int View::SignalMargin = 7;
const int View::SignalSnapGridSize = 10;

const QColor View::CursorAreaColour(220, 231, 243);
const QSizeF View::LabelPadding(4, 4);
const QString View::Unknown_Str = "########";

View::View(SigSession *session, pv::toolbars::SamplingBar *sampling_bar,
           QWidget *parent)
    : QScrollArea(parent), _sampling_bar(sampling_bar),
      _trig_hoff(0),
      _hover_point(-1, -1), _dso_auto(true), _show_lissajous(false) {
  _session = session;
  _device_agent = session->device();

  // Phase E: initialise the three delegate classes. Must happen before any
  // call that forwards through the inline facades (e.g. headerWidth() →
  // get_traces() → get_derived->_own_decode_traces() → sync_derived_traces()).
  _layout = std::make_unique<ViewLayout>(this);
  _cursors = std::make_unique<ViewCursors>(this);
  _derived = std::make_unique<ViewDerivedTraces>(this);
  // Phase J: initialise the three new delegate classes (signal-sync /
  // glitch-filter / data-sync). Must happen before any call that forwards
  // through the inline facades (e.g. signals_changed → _signal_sync).
  _signal_sync = std::make_unique<ViewSignalSync>(this);
  _glitch_filter = std::make_unique<ViewGlitchFilter>(this);
  _data_sync = std::make_unique<ViewDataSync>(this);

  // The data source must be assigned only after every delegate exists.
  // During construction we set the fields directly instead of going through
  // set_data_source(), because that path calls rebuild_signals() and updates
  // the viewports, none of which are created yet at this point.
  _data_sync->set_data_source_ptr(session);
  _data_sync->set_document_ptr(nullptr);

  // Visible-range debounce timer: coalesce bursts of scale/offset/resize
  // changes into a single visible_range_changed() emission so listeners
  // (e.g. ProtocolDock) don't reset their model on every pixel of a drag.
  _viewport_change_timer = new QTimer(this);
  _viewport_change_timer->setSingleShot(true);
  _viewport_change_timer->setInterval(100);
connect(_viewport_change_timer, &QTimer::timeout, this,
[this]() { emit visible_range_changed(); });

// P1-A: Delayed view-update coalescing timer — merges bursts of
// viewport_update() calls into a single repaint at most once per 16ms
// (~60 FPS).  This prevents UI stutter when the decode thread fires
// many new_decode_data signals in rapid succession.
_delayed_view_update_timer = new QTimer(this);
_delayed_view_update_timer->setSingleShot(true);
_delayed_view_update_timer->setInterval(MaxViewAutoUpdateRateMs);
connect(_delayed_view_update_timer, &QTimer::timeout, this, [this]() {
  if (_delayed_view_update_pending) {
    // A full update was requested (e.g. zoom/scroll/resize/decode-done) —
    // it supersedes any pending decode-only repaint.
    _delayed_view_update_pending = false;
    _decode_only_repaint_pending = false;
#ifdef PXVIEW_DECODE_PERF
    pv::base::perf::record_repaint_delayed(/*full=*/true);
#endif
    viewport_update();
  } else if (_decode_only_repaint_pending) {
    // P2: decode-growth repaint — decode trace layer only, no signal-pixmap
    // rebuild (skips set_decode_dirty()).
    _decode_only_repaint_pending = false;
#ifdef PXVIEW_DECODE_PERF
    pv::base::perf::record_repaint_delayed(/*full=*/false);
#endif
    viewport_update_decode_only();
  }
});

#ifdef PXVIEW_DECODE_PERF
// P3-D4: main-thread event-loop lag detector. A 100ms periodic tick; if the
// GUI thread is blocked (freeze) the tick fires late and the overshoot is
// recorded in the perf log (EVENT_LAG_MAX). This distinguishes "main thread
// blocked" from "decode threads busy" as the cause of perceived freezing.
// P3-D6: also samples the whole-process CPU utilisation per tick (CPU_UTIL_MAX
// in cores) — high util alongside a large EVENT_LAG_MAX means the decode
// threads saturate the machine and starve the GUI thread.
_event_lag_timer = new QTimer(this);
_event_lag_timer->setInterval(100);
connect(_event_lag_timer, &QTimer::timeout, this, []() {
  pv::base::perf::record_event_lag();
#ifdef _WIN32
  static ULARGE_INTEGER _last_kt{}, _last_ut{};
  static std::chrono::steady_clock::time_point _last_cpu_t{};
  FILETIME _ct, _et, _kt, _ut;
  if (GetProcessTimes(GetCurrentProcess(), &_ct, &_et, &_kt, &_ut)) {
    ULARGE_INTEGER k, u;
    k.LowPart = _kt.dwLowDateTime; k.HighPart = _kt.dwHighDateTime;
    u.LowPart = _ut.dwLowDateTime; u.HighPart = _ut.dwHighDateTime;
    const auto now = std::chrono::steady_clock::now();
    if (_last_cpu_t.time_since_epoch().count() != 0) {
      const double sec =
          std::chrono::duration<double>(now - _last_cpu_t).count();
      // 100ns FILETIME units -> ms, delta user+kernel since last tick.
      const double cpu_ms =
          (double)((k.QuadPart - _last_kt.QuadPart) +
                   (u.QuadPart - _last_ut.QuadPart)) /
          10000.0;
      const double util = sec > 0 ? cpu_ms / (sec * 1000.0) : 0.0;
      pv::base::perf::record_cpu_util(util);
    }
    _last_kt = k; _last_ut = u; _last_cpu_t = now;
  }
#endif
});
_event_lag_timer->start();
#endif

setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setStyleSheet(
      QString("QScrollBar:vertical { margin-top: %1px; }").arg(RulerHeight));

  connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
          &View::h_scroll_value_changed);
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          &View::v_scroll_value_changed);

  // trace viewport map
  _trace_view_map[SR_CHANNEL_LOGIC] = TIME_VIEW;
  _trace_view_map[SR_CHANNEL_GROUP] = TIME_VIEW;
  _trace_view_map[SR_CHANNEL_DECODER] = TIME_VIEW;
  _trace_view_map[SR_CHANNEL_ANALOG] = TIME_VIEW;
  _trace_view_map[SR_CHANNEL_DSO] = TIME_VIEW;
  _trace_view_map[SR_CHANNEL_FFT] = FFT_VIEW;
  _trace_view_map[SR_CHANNEL_LISSAJOUS] = TIME_VIEW;
  _trace_view_map[SR_CHANNEL_MATH] = TIME_VIEW;

  _active_viewport = nullptr;
  _header_collapsed = false;
  _ruler = new Ruler(*this);
  _header = new Header(*this);
  _devmode = new DevMode(this, session);

  setViewportMargins(headerWidth(), RulerHeight, 0, 0);

  // windows splitter
  _time_viewport = new Viewport(*this, TIME_VIEW);
  _time_viewport->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  _time_viewport->setMinimumHeight(100);

  _fft_viewport = new Viewport(*this, FFT_VIEW);
  _fft_viewport->setVisible(false);
  _fft_viewport->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  _fft_viewport->setMinimumHeight(100);

  _vsplitter = new pv::widgets::HoverSplitter(this);
  _vsplitter->setOrientation(Qt::Vertical);
  _vsplitter->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

  _viewport_list.push_back(_time_viewport);
  _vsplitter->addWidget(_time_viewport);
  _vsplitter->setCollapsible(0, false);
  _vsplitter->setStretchFactor(0, 2);
  _viewport_list.push_back(_fft_viewport);
  _vsplitter->addWidget(_fft_viewport);
  _vsplitter->setCollapsible(1, false);
  _vsplitter->setStretchFactor(1, 1);

  _viewcenter = new QWidget(this);
  _viewcenter->setContentsMargins(0, 0, 0, 0);
  QGridLayout *layout = new QGridLayout(_viewcenter);
  layout->setSpacing(0);
  layout->setContentsMargins(0, 0, 0, 0);
  _viewcenter->setLayout(layout);
  layout->addWidget(_vsplitter, 0, 0);
  _viewbottom = new ViewStatus(_session, *this);
  _viewbottom->setFixedHeight(StatusHeight);
  _viewbottom->hide();
  _viewbottom->setParent(_time_viewport);
  _viewbottom->setGeometry(0, _time_viewport->height() - StatusHeight,
                           _time_viewport->width(), StatusHeight);

#ifdef Q_OS_DARWIN
  QWidget *lineSpan = new QWidget(this);
  lineSpan->setFixedHeight(10);
  layout->addWidget(lineSpan, 2, 0);
#endif

  setViewport(_viewcenter);

  _time_viewport->installEventFilter(this);
  _fft_viewport->installEventFilter(this);
  _ruler->installEventFilter(this);
  _header->installEventFilter(this);
  _devmode->installEventFilter(this);

  // tr
  _viewcenter->setObjectName("ViewArea_center");
  _ruler->setObjectName("ViewArea_ruler");
  _header->setObjectName("ViewArea_header");

QColor fore(QWidget::palette().color(QWidget::foregroundRole()));
fore.setAlpha(View::BackAlpha);

  _cursors->init_cursors(fore);

  connect(_time_viewport, &Viewport::measure_updated, this,
          &View::on_measure_updated);
  connect(_time_viewport, &Viewport::prgRate, this, &View::prgRate);
  connect(_fft_viewport, &Viewport::measure_updated, this,
          &View::on_measure_updated);

  connect(_vsplitter, &QSplitter::splitterMoved, this, &View::splitterMoved);

  connect(_header, &Header::traces_moved, this, &View::on_traces_moved);
  connect(_header, &Header::header_updated, this, &View::header_updated);
  connect(_header, &Header::show_glitch_filter_popup, this,
          &View::on_show_glitch_filter_popup);
  connect(_header, &Header::clear_glitch_filter_requested, this,
          &View::on_clear_glitch_filter_requested);
  connect(_header, &Header::toggle_signal_invert_requested, this,
          &View::on_toggle_invert_requested);
  connect(_devmode, &DevMode::header_collapse_changed, this,
          &View::on_header_collapse_changed);
  connect(_devmode, &DevMode::mode_change_requested, this,
          [this](int mode) { _data_sync->data_source_ptr()->switch_work_mode(mode); });
  connect(_devmode, &DevMode::stop_capture_requested, this,
          [this]() { _data_sync->data_source_ptr()->stop_capture(); });
  connect(_devmode, &DevMode::save_session_requested, this,
          [this]() { _data_sync->data_source_ptr()->session_save(); });
  connect(_devmode, &DevMode::close_file_requested, this,
          [this](ds_device_handle dev_handle) { _data_sync->data_source_ptr()->close_file(dev_handle); });

  // Glitch filter popup (View-owned). Created up-front and reused via
  // open_for_signal() so the histogram cache persists across open/close.
  auto *popup = new GlitchFilterPopup(*this, this);
  popup->hide();
  _glitch_filter->set_glitch_filter_popup(popup);
  connect(popup, &GlitchFilterPopup::preview_changed, this,
          &View::on_glitch_preview_changed);
  connect(popup, &GlitchFilterPopup::apply_requested, this,
          &View::on_glitch_apply_requested);
  connect(popup, &GlitchFilterPopup::closed, this,
          &View::on_glitch_popup_closed);
  connect(popup, &GlitchFilterPopup::apply_batch_requested, this,
          &View::on_apply_batch_requested);
  connect(popup, &GlitchFilterPopup::preview_batch_changed, this,
          &View::on_preview_batch_changed);

  ADD_UI(this);
}

View::~View() {
  _destroying = true;

  // Disconnect signals and remove event filters before child destruction
  // to prevent callbacks on partially-destroyed View
  disconnect(_header, nullptr, this, nullptr);
  disconnect(_devmode, nullptr, this, nullptr);
  auto *gfp = _glitch_filter->glitch_filter_popup();
  if (gfp) {
    disconnect(gfp, nullptr, this, nullptr);
  }
  _header->removeEventFilter(this);
  _ruler->removeEventFilter(this);
  _devmode->removeEventFilter(this);
  _time_viewport->removeEventFilter(this);
  _fft_viewport->removeEventFilter(this);

// unique_ptr in _own_signals auto-deletes all Signal elements.
_signal_sync->own_signals().clear();
  // Drop preview-range cache keys (LogicSignal pointers now dangling).
  _glitch_filter->clear_preview_ranges();

  // Clean up View-owned wrapper traces (these wrap Core layer Stack/Model
  // objects and are owned by the View, not by the data source).
  _derived->cleanup();

// Destroy the glitch filter popup (View-owned QWidget). Qt would also
// delete it as a child widget, but explicit reset here guarantees the
// closed() signal cannot fire mid-destruction. unique_ptr in ViewGlitchFilter
// handles deletion via set_glitch_filter_popup(nullptr).
_glitch_filter->set_glitch_filter_popup(nullptr);

  // Cursor state (trig/search cursors included) is now fully owned and
  // cleaned up by ViewCursors' destructor. No cross-class deletion needed.
  REMOVE_UI(this);
}

void View::set_data_source(pv::data::DataSource *source) {
  _data_sync->set_data_source(source);
  // Task C2.7: reconcile the View's rendering cursor list with the Core
  // CursorRegistry. This handles the headless -> GUI transition where MCP
  // added cursors to Core before the View existed. Safe to call on every
  // data-source binding (idempotent — only adds cursors that are missing).
  sync_cursors_from_core();
}

void View::clear_signal_data() { _data_sync->clear_signal_data(); }

void View::set_signal_data_from_source(pv::data::DataSource *source) {
  _data_sync->set_signal_data_from_source(source);
}

void View::set_data_document(pv::data::SessionDocument *doc) {
  _data_sync->set_data_document(doc);
}

void View::clone_signals_for_document(pv::data::SessionDocument *doc) {
  _data_sync->clone_signals_for_document(doc);
}

data::DataSource *View::document_snapshot_source() {
  return _data_sync->document_snapshot_source();
}

void View::show_wait_trigger() { _time_viewport->show_wait_trigger(); }

void View::set_device() { _devmode->set_device(); }

void View::capture_init() { _data_sync->capture_init(); }

void View::set_update(Viewport *viewport, bool need_update) {
  viewport->set_need_update(need_update);
}

void View::set_all_update(bool need_update) {
  _time_viewport->set_need_update(need_update);
  _fft_viewport->set_need_update(need_update);
}

double View::get_hori_res() { return _sampling_bar->get_hori_res(); }

void View::update_hori_res() {
  if (get_work_mode() == DSO) {
    _sampling_bar->hori_knob(0);
  }
}

void View::zoom_vertical(double steps) { _signal_sync->zoom_vertical(steps); }

void View::compute_signal_groups() { _signal_sync->compute_signal_groups(); }

QColor View::get_group_card_color() {
  return _signal_sync->get_group_card_color();
}

bool View::is_colored_card_mode() {
  return _signal_sync->is_colored_card_mode();
}

QColor View::get_group_card_color(int group_index) {
  return _signal_sync->get_group_card_color(group_index);
}

QColor View::get_trace_card_color(Trace *trace) {
  return _signal_sync->get_trace_card_color(trace);
}

void View::timebase_changed() { _data_sync->timebase_changed(); }

void View::set_preScale_preOffset() { set_scale_offset(_layout->preScale(), _layout->preOffset()); }

void View::schedule_visible_range_notify() {
  if (_viewport_change_timer) {
    _viewport_change_timer->start();
  }
}

void View::get_traces(int type, std::vector<Trace *> &traces) {
  _signal_sync->get_traces(type, traces);
}

bool View::compare_trace_v_offsets(const Trace *a, const Trace *b) {
  return ViewSignalSync::compare_trace_v_offsets(a, b);
}

bool View::compare_trace_view_index(const Trace *a, const Trace *b) {
  return ViewSignalSync::compare_trace_view_index(a, b);
}

bool View::compare_trace_y(const Trace *a, const Trace *b) {
  return ViewSignalSync::compare_trace_y(a, b);
}

void View::status_clear() {
  _time_viewport->clear_dso_xm();
  _time_viewport->clear_measure();
  _viewbottom->clear();
}

void View::repeat_unshow() { _viewbottom->repeat_unshow(); }

void View::frame_began() { _data_sync->frame_began(); }

void View::receive_end() { _data_sync->receive_end(); }

void View::receive_trigger(quint64 trig_pos1) {
  _data_sync->receive_trigger(trig_pos1);
}

void View::set_trig_pos(int percent) {
  uint64_t index = document_snapshot_source()->cur_samplelimits() * percent / 100;

  if (_data_sync->data_source_ptr()->have_view_data() == false || _data_sync->data_source_ptr()->is_working()) {
    set_trig_cursor_posistion(index);
  }
}

void View::normalize_layout() { _signal_sync->normalize_layout(); }

void View::mode_changed() { _data_sync->mode_changed(); }

void View::signals_changed(const Trace *eventTrace) {
#ifdef PXVIEW_DECODE_PERF
  // P3-D5: timing candidate for the EVENT_LAG_MAX block — full signal
  // relayout (normalize + group + layout_time_signals).
  const auto _op_t0 = std::chrono::steady_clock::now();
#endif
  _signal_sync->signals_changed(eventTrace);
#ifdef PXVIEW_DECODE_PERF
  pv::base::perf::record_op_max(
      "signals_changed",
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - _op_t0).count());
#endif
}

bool View::eventFilter(QObject *object, QEvent *event) {
  return _data_sync->eventFilter(object, event);
}

bool View::viewportEvent(QEvent *e) {
  switch (e->type()) {
  case QEvent::Paint:
  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonRelease:
  case QEvent::MouseButtonDblClick:
  case QEvent::MouseMove:
  case QEvent::Wheel:
  case QEvent::Gesture:
    return false;

  default:
    return QAbstractScrollArea::viewportEvent(e);
  }
}

int View::headerWidth() { return _signal_sync->headerWidth(); }

void View::paintEvent(QPaintEvent *event) { QScrollArea::paintEvent(event); }

void View::scrollContentsBy(int dx, int dy) {
  (void)dx;
  (void)dy;
}

void View::resizeEvent(QResizeEvent *event) {
  _data_sync->resizeEvent(event);
}

void View::v_scroll_value_changed(int value) {
  _layout->set_vOffset(value);
  _header->update();
  viewport_update();
}

void View::data_updated() { _data_sync->data_updated(); }

void View::header_updated() {
  headerWidth();
  update_margins();

  // Update the scroll bars
  update_scroll();

  viewport_update();
  _header->update();
}

void View::on_header_collapse_changed(bool collapsed) {
  _header_collapsed = collapsed;
  headerWidth();
  update_margins();
  update_scroll();
  viewport_update();
  _header->update();
  _ruler->update();
}

void View::on_traces_moved() {
  update_scroll();
  set_update(_time_viewport, true);
  viewport_update();
}

void View::on_measure_updated() {
  _active_viewport = dynamic_cast<Viewport *>(sender());
  measure_updated();
}

QString View::get_measure(QString option) {
  if (_active_viewport) {
    return _active_viewport->get_measure(option);
  }
  return Unknown_Str;
}

QString View::get_index_delta(uint64_t start, uint64_t end) {
  return _data_sync->get_index_delta(start, end);
}

void View::set_measure_en(int enable) {
  _time_viewport->set_measure_en(enable);
  _fft_viewport->set_measure_en(enable);
}

void View::on_state_changed(bool stop) {
  if (stop) {
    _time_viewport->stop_trigger_timer();
    _fft_viewport->stop_trigger_timer();
  }
  update_scale_offset();
}

QRect View::get_view_rect() { return _data_sync->get_view_rect(); }

int View::get_work_mode() const {
  if (_data_sync->document_ptr() && _data_sync->document_ptr()->has_signal_config()) {
    return _data_sync->document_ptr()->get_signal_config().work_mode;
  }
  return _device_agent->get_work_mode();
}

bool View::is_logic_rendering_mode() const {
  // MSO (Mixed Signal Oscilloscope) = LOGIC + analog channels.
  // Core 层 SigSession 已将 LOGIC 与 MSO 合并处理（sigsession.cpp:1605/1612），
  // View 层所有原 `get_work_mode() == LOGIC` 的渲染/交互分支应统一改用本谓词，
  // 让 MSO 模式继承 LOGIC 的全部行为（主题色板注入、滤波浮窗、信号分组等）。
  const int mode = get_work_mode();
  return mode == LOGIC || mode == MSO;
}

int View::get_view_width() { return _data_sync->get_view_width(); }

int View::get_view_height() { return _data_sync->get_view_height(); }

int64_t View::get_logic_lst_data_offset() {
  return _data_sync->get_logic_lst_data_offset();
}

void View::scroll_to_logic_last_data_time() {
  _data_sync->scroll_to_logic_last_data_time();
}

void View::vDial_updated() { _data_sync->vDial_updated(); }

void View::dso_factor_updated() { _data_sync->dso_factor_updated(); }

// -- lissajous figure
void View::show_lissajous(bool show) {
	_show_lissajous = show;
	signals_changed(nullptr);
}

void View::set_decoder_analog_trigger_position(uint64_t sample_position,
                                                int display_position_percent) {
	// If hold is active, stage the trigger position for later commit.
	// Otherwise apply immediately.
	if (_decoder_analog_trigger_hold) {
		_decoder_analog_trigger_pending = true;
		_decoder_analog_trigger_pending_sample = sample_position;
		_decoder_analog_trigger_pending_percent = display_position_percent;
	} else {
		set_trig_pos(display_position_percent);
	}
}

void View::set_decoder_analog_trigger_display_hold(bool hold) {
	if (hold && !_decoder_analog_trigger_hold) {
		// Entering hold: freeze viewport updates until the trigger is committed.
		_decoder_analog_trigger_hold = true;
		_decoder_analog_trigger_pending = false;
	} else if (!hold && _decoder_analog_trigger_hold) {
		// Leaving hold: commit the staged trigger position (if any) and
		// resume normal viewport updates atomically.
		if (_decoder_analog_trigger_pending) {
			set_trig_pos(_decoder_analog_trigger_pending_percent);
			_decoder_analog_trigger_pending = false;
		}
		_decoder_analog_trigger_hold = false;
		viewport_update();
	}
}

void View::force_release_decoder_analog_trigger_display_hold() {
	if (_decoder_analog_trigger_hold) {
		_decoder_analog_trigger_hold = false;
		_decoder_analog_trigger_pending = false;
	}
}

void View::show_region(uint64_t start, uint64_t end, bool keep) {
  _data_sync->show_region(start, end, keep);
}

void View::viewport_update() {
// Suppress viewport updates during decoder analog trigger display-hold.
// This prevents intermediate-frame flicker while a new repeat frame is
// being decoded and aligned to the trigger position.
if (_decoder_analog_trigger_hold)
return;

#ifdef PXVIEW_DECODE_PERF
// P3-D: full viewport_update() entry (any caller).
pv::base::perf::record_repaint_viewport();
#endif

// Mark decode pixmap dirty so it will be rebuilt on next paint.
// This is needed because decode data can change independently of
// view parameters (e.g. new decode data arriving).
if (_time_viewport)
_time_viewport->set_decode_dirty();

_viewcenter->update();
for (QWidget *viewport : _viewport_list)
viewport->update();
}

void View::request_delayed_update() {
  // P1-A: If the timer is already running, the pending request will be
  // serviced when it fires — no need to restart it.  This naturally
  // coalesces all calls within a 16ms window into a single repaint.
  if (!_delayed_view_update_timer->isActive()) {
    _delayed_view_update_pending = true;
    _delayed_view_update_timer->start();
  }
}

void View::request_decode_only_update() {
  // P2: coalesced decode-only repaint. Sets the decode-only pending flag so
  // the coalescing timer drains via viewport_update_decode_only() (no signal-
  // pixmap rebuild). A full request_delayed_update() in the same 16ms window
  // supersedes this (checked first in the timer lambda).
  _decode_only_repaint_pending = true;
  if (!_delayed_view_update_timer->isActive())
    _delayed_view_update_timer->start();
}

void View::viewport_update_decode_only() {
  // P2: suppress during decoder-analog-trigger display-hold, same as the
  // full viewport_update().
  if (_decoder_analog_trigger_hold)
    return;

#ifdef PXVIEW_DECODE_PERF
  // P3-D: decode-only update entry (decode growth path).
  pv::base::perf::record_repaint_decode_only();
#endif

  // Deliberately do NOT call _time_viewport->set_decode_dirty(): decode
  // growth does not change signal waveforms (only the decode trace layer,
  // which DecodeTracePass paints outside the cached signal pixmap). Marking
  // the time viewport for a decode-only paint lets SignalPixmapPass keep
  // blitting the existing (valid) pixmap while DecodeTracePass redraws the
  // new annotations. Any real dirty source (zoom/scroll/resize -> forced
  // _need_update) still falls back to the full rebuild inside that pass.
  if (_time_viewport)
    _time_viewport->set_decode_only_paint();

  _viewcenter->update();
  for (QWidget *viewport : _viewport_list)
    viewport->update();
}

void View::splitterMoved(int pos, int index) {
  (void)pos;
  (void)index;
  signals_changed(nullptr);
}

void View::reload() {
  clear();

  /*
   * if headerwidth not change, viewport height will not be updated
   * lead to a wrong signal height
   */
  reconstruct();
}

void View::clear() {
  show_trig_cursor(false);

  // 设备切换早期（CurrentDeviceChangePrev 阶段）_dev_handle 可能为 nullptr，
  // 此时 work_mode 查询会失败。用 document 配置或默认值（非 DSO）避免警告刷屏。
  int mode = LOGIC;
  if (_data_sync->document_ptr() && _data_sync->document_ptr()->has_signal_config()) {
    mode = _data_sync->document_ptr()->get_signal_config().work_mode;
  } else if (_device_agent && _device_agent->have_instance()) {
    mode = _device_agent->get_work_mode();
  }
  if (mode != DSO) {
    show_xcursors(false);
  } else {
    if (!get_xcursorList().empty())
      show_xcursors(true);
  }
}

void View::reconstruct() {
  // 同上：设备切换早期避免查询设备
  int mode = LOGIC;
  if (_data_sync->document_ptr() && _data_sync->document_ptr()->has_signal_config()) {
    mode = _data_sync->document_ptr()->get_signal_config().work_mode;
  } else if (_device_agent && _device_agent->have_instance()) {
    mode = _device_agent->get_work_mode();
  }
  if (mode == DSO)
    _viewbottom->setFixedHeight(DsoStatusHeight);
  else
    _viewbottom->setFixedHeight(StatusHeight);
  _viewbottom->setGeometry(0, _time_viewport->height() - _viewbottom->height(),
                           _time_viewport->width(), _viewbottom->height());
  _viewbottom->reload();
}

void View::repeat_show() { _viewbottom->update(); }

void View::show_captured_progress(bool triggered, int progress) {
  _viewbottom->set_capture_status(triggered, progress);
  _viewbottom->update();
}

bool View::get_dso_trig_moved() { return _time_viewport->get_dso_trig_moved(); }

double View::index2pixel(uint64_t index, bool has_hoff) {
  return _data_sync->index2pixel(index, has_hoff);
}

uint64_t View::pixel2index(double pixel) {
  return _data_sync->pixel2index(pixel);
}

void View::set_receive_len(uint64_t len) { _data_sync->set_receive_len(len); }

void View::rebuild_signals_from_config(const data::SignalConfig &config) {
  _signal_sync->rebuild_signals_from_config(config);
}

void View::rebuild_signals() { _signal_sync->rebuild_signals(); }

void View::on_signals_changed() { _signal_sync->on_signals_changed(); }

void View::signals_added_layout() { _signal_sync->signals_added_layout(); }

void View::signals_removed_layout() { _signal_sync->signals_removed_layout(); }

void View::signals_modified_refresh() {
  _signal_sync->signals_modified_refresh();
}

void View::auto_set_max_scale() { _data_sync->auto_set_max_scale(); }

int View::get_body_width() {
  if (_time_viewport != nullptr)
    return _time_viewport->width();
  return 0;
}

int View::get_body_height() {
  if (_time_viewport != nullptr)
    return _time_viewport->height();
  return 0;
}

void View::update_view_port() {
  if (_time_viewport)
    _time_viewport->update(UpdateEventType::UPDATE_EV_GENERIC);
}

void View::update_font() { headerWidth(); }

void View::check_measure() {
  _time_viewport->measure();
  _time_viewport->update(UpdateEventType::UPDATE_EV_GENERIC);
}

bool View::header_is_draging() { return _header->mouse_is_down(); }

void View::UpdateLanguage() {}

void View::UpdateTheme() { _signal_sync->UpdateTheme(); }

void View::UpdateFont() { update_font(); }

bool View::view_is_ready() {
  int w = get_view_width();
  return w > 0;
}


// =============================================================================
// Glitch filter popup handlers (Task 7 + 9)
// =============================================================================

void View::on_show_glitch_filter_popup(pv::view::LogicSignal *sig) {
  _glitch_filter->on_show_glitch_filter_popup(sig);
}

void View::on_clear_glitch_filter_requested(bool all_channels) {
  _glitch_filter->on_clear_glitch_filter_requested(all_channels);
}

void View::on_toggle_invert_requested(pv::view::LogicSignal *sig) {
  _glitch_filter->on_toggle_invert_requested(sig);
}

void View::on_glitch_preview_changed(pv::view::LogicSignal *sig,
                                     uint32_t threshold,
                                     GlitchFilterMode mode) {
  _glitch_filter->on_glitch_preview_changed(sig, threshold, mode);
}

void View::on_glitch_apply_requested(pv::view::LogicSignal *sig,
                                     uint32_t threshold,
                                     GlitchFilterMode mode,
                                     bool all_channels) {
  _glitch_filter->on_glitch_apply_requested(sig, threshold, mode,
                                            all_channels);
}

void View::on_glitch_popup_closed() {
  _glitch_filter->on_glitch_popup_closed();
}

void View::on_apply_batch_requested(const std::vector<pv::view::LogicSignal *> &sigs,
                                    uint32_t threshold, GlitchFilterMode mode) {
  _glitch_filter->on_apply_batch_requested(sigs, threshold, mode);
}

void View::on_preview_batch_changed(const std::vector<pv::view::LogicSignal *> &sigs,
                                    uint32_t threshold, GlitchFilterMode mode) {
  _glitch_filter->on_preview_batch_changed(sigs, threshold, mode);
}

const std::vector<pv::data::PulseAnalyzer::Pulse> *
View::get_preview_ranges(LogicSignal *sig) const {
  return _glitch_filter->get_preview_ranges(sig);
}

void View::undo_filter() { _glitch_filter->undo_filter(); }

void View::on_glitch_filter_completed() {
  _glitch_filter->on_glitch_filter_completed();
}

void View::on_glitch_filter_cleared() {
  _glitch_filter->on_glitch_filter_cleared();
}

// Phase K: out-of-line forwarders extracted from view.h (delegate to Phase E/J helpers).
int64_t View::get_min_offset() { return _layout->get_min_offset(); }
int64_t View::get_max_offset() { return _layout->get_max_offset(); }
void View::zoom(double steps) { _layout->zoom(steps); }
bool View::zoom(double steps, int offset) { return _layout->zoom(steps, offset); }
void View::set_scale_offset(double scale, int64_t offset) { _layout->set_scale_offset(scale, offset); }
void View::limit_scale_offset() { _layout->limit_scale_offset(); }
void View::get_scroll_layout(int64_t &length, int64_t &offset) { _layout->get_scroll_layout(length, offset); }
void View::update_scroll() { _layout->update_scroll(); }
void View::update_margins() { _layout->update_margins(); }
void View::set_scale(double scale) { _layout->set_scale(scale); }
void View::update_scale_offset() { _layout->update_scale_offset(); }
void View::h_scroll_value_changed(int value) { _layout->h_scroll_value_changed(value); }
void View::show_cursors(bool show) { _cursors->show_cursors(show); }
void View::show_trig_cursor(bool show) { _cursors->show_trig_cursor(show); }
void View::show_search_cursor(bool show) { _cursors->show_search_cursor(show); }
std::list<std::unique_ptr<Cursor>> &View::get_cursorList() { return _cursors->get_cursorList(); }
void View::add_cursor(QColor color, uint64_t sampleIndex) { _cursors->add_cursor(color, sampleIndex); }
void View::add_cursor(uint64_t sampleIndex) { _cursors->add_cursor(sampleIndex); }
void View::del_cursor(Cursor *cursor) { _cursors->del_cursor(cursor); }
void View::add_xcursor(double value0, double value1) { _cursors->add_xcursor(value0, value1); }
void View::del_xcursor(XCursor *xcursor) { _cursors->del_xcursor(xcursor); }
void View::clear_cursors() { _cursors->clear_cursors(); }
void View::set_cursor_middle(int index) { _cursors->set_cursor_middle(index); }
Cursor *View::get_cursor_by_index(int index) { return _cursors->get_cursor_by_index(index); }
// Task C2.7: forward drag-position write-back and Core list reconciliation
// to the ViewCursors delegate.
void View::sync_cursor_position(Cursor *cursor) { _cursors->sync_cursor_position_to_core(cursor); }
void View::sync_cursors_from_core() { _cursors->sync_cursors_from_core(); }
void View::set_search_pos(uint64_t search_pos, bool hit) { _cursors->set_search_pos(search_pos, hit); }
uint64_t View::get_cursor_samples(int index) { return _cursors->get_cursor_samples(index); }
QString View::get_cm_time(int index) { return _cursors->get_cm_time(index); }
QString View::get_cm_delta(int index1, int index2) { return _cursors->get_cm_delta(index1, index2); }
int View::get_cursor_index_by_key(uint64_t key) { return _cursors->get_cursor_index_by_key(key); }
void View::set_trig_cursor_posistion(uint64_t percent) { _cursors->set_trig_cursor_posistion(percent); }
void View::make_cursors_order() { _cursors->make_cursors_order(); }
bool View::add_decoder(srd_decoder *const dec, bool silent, DecoderStatus *dstatus, std::list<pv::data::decode::Decoder *> &sub_decoders, std::shared_ptr<pv::data::DecoderStack> &out_stack) {
  return _derived->add_decoder(dec, silent, dstatus, sub_decoders, out_stack);
}
void View::remove_decoder(DecodeTrace *trace) { _derived->remove_decoder(trace); }
void View::remove_decoder(int index) { _derived->remove_decoder(index); }
void View::remove_decoder_by_key_handel(void *key_handel) { _derived->remove_decoder_by_key_handel(key_handel); }
void View::clear_all_decoders() { _derived->clear_all_decoders(); }
bool View::rst_decoder_by_key_handel(void *handel, QPoint anchor) { return _derived->rst_decoder_by_key_handel(handel, anchor); }
void View::sync_derived_traces() { _derived->sync_derived_traces(); }
void View::mark_derived_traces_dirty() { _derived->mark_derived_traces_dirty(); }
} // namespace view
} // namespace pv
