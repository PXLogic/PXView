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

#include "viewport.h"
#include "viewport_drag.h"
#include "viewport_interaction.h"
#include "viewport_painter.h"
#include "ruler.h"
#include "viewstatus.h"

#include "../sigsession.h"
#include "analogsignal.h"
#include "dsosignal.h"
#include "logicsignal.h"
#include "signal.h"
#include "spectrumtrace.h"

#include <QAction>
#include <QCursor>
#include <QDebug>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QWheelEvent>
#include <math.h>

#include "../config/appconfig.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"
#include "lissajoustrace.h"
#include "mathtrace.h"
#include "waveform_copy_helper.h"
#include "decodetrace.h"

#include <QClipboard>
#include <QGuiApplication>

using namespace std;

namespace pv {
namespace view {

const double Viewport::DragDamping = 1.05;
const double Viewport::MinorDragRateUp = 10;

const QColor Viewport::PROBE_COLORS[8] = {

    QColor(0x75, 0x50, 0x7B), // Violet
    QColor(0x34, 0x65, 0xA4), // Blue
    QColor(0x73, 0xD2, 0x16), // Green
    QColor(0xED, 0xD4, 0x00), // Yellow
    QColor(0xF5, 0x79, 0x00), // Orange
    QColor(0xCC, 0x00, 0x00), // Red
    QColor(0x8F, 0x52, 0x02), // Brown
    QColor(0x50, 0x50, 0x50), // Black

};

Viewport::Viewport(View &parent, View_type type)
    : QWidget(&parent), _view(parent), _type(type), _need_update(false),
      _sample_received(0), _action_type(NO_ACTION), _measure_type(NO_MEASURE),
      _cur_sample(0), _nxt_sample(1), _cur_preX(0), _cur_aftX(1), _cur_midY(0),
      _hover_index(0), _hover_hit(false), _dso_xm_valid(false),
      _dso_ym_valid(false), _waiting_trig(0), _dso_trig_moved(false),
      _resize_trace_upper(NULL), _resize_trace_lower(NULL),
      _resize_mouse_down_y(0), _resize_upper_height(0), _resize_lower_height(0),
      _curs_moved(false), _xcurs_moved(false), _curVOffset(0),
      _max_frame_time(0), _fps(0), _is_idle(true), _drag_frame_pending(false),
      _hover_logic_signal(nullptr), g_drag_active(false),
      _paint_in_this_second(0) {
  _panelBgColor = AppConfig::Instance().GetThemeColor("@panel-bg");
  if (!_panelBgColor.isValid())
    _panelBgColor = QColor("#1a1a1a");
  _panelTextColor = AppConfig::Instance().GetThemeColor("@panel-text");
  if (!_panelTextColor.isValid())
    _panelTextColor = QColor("#f5f0e5");

  setMouseTracking(true);
  setAutoFillBackground(true);
  setBackgroundRole(QPalette::Base);
  setFocusPolicy(Qt::StrongFocus);

  // setFixedSize(QSize(600, 400));
  _mm_width = View::Unknown_Str;
  _mm_period = View::Unknown_Str;
  _mm_freq = View::Unknown_Str;
  _mm_duty = View::Unknown_Str;
  _measure_en = true;
  _edge_hit = false;
  _transfer_started = false;
  _timer_cnt = 0;
  _sample_received = 0;
  _progress_displayed = 0.0;
  _is_checked_trig = false;

  _lst_wait_tigger_time = high_resolution_clock::now();
  _tigger_wait_times = 0;

  // drag inertial
  _drag_strength = 0;
  _drag_timer.setSingleShot(true);

  _cmenu = new QMenu(this);
  QAction *yAction = _cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_Y_CURSOR), "Add Y-cursor"));
  QAction *xAction = _cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_X_CURSOR), "Add X-cursor"));
  _yAction = yAction;
  _xAction = xAction;

  setContextMenuPolicy(Qt::CustomContextMenu);

  connect(&_trigger_timer, &QTimer::timeout, this, &Viewport::on_trigger_timer);
  connect(&_drag_timer, &QTimer::timeout, this, &Viewport::on_drag_timer);

  // Smooth progress animation: 16ms ≈ 60 FPS. Lerps the displayed
  // progress toward the actual _sample_received value so the progress
  // circle animates fluidly instead of jumping in discrete steps at
  // each data-packet arrival (which can be as slow as 10–25 FPS).
  _progress_timer.setInterval(16);
  connect(&_progress_timer, &QTimer::timeout, this, &Viewport::on_progress_timer);
  connect(yAction, &QAction::triggered, this, &Viewport::add_cursor_y);
  connect(xAction, &QAction::triggered, this, &Viewport::add_cursor_x);
  connect(this, &QWidget::customContextMenuRequested, this,
          &Viewport::show_contextmenu);

  // Logic/MSO mode context menu for "copy waveform between cursors"
  _logic_cmenu = new QMenu(this);
  _copy_this_channel_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_THIS_CHANNEL), "Copy Channel Waveform Data"));
  _copy_decoder_track_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_TRACK), "Copy Decoder Waveform Data"));
  _copy_decoder_group_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_GROUP), "Copy Decoder Group Waveform Data"));
  _copy_all_channels_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ALL_CHANNELS), "Copy All Channels Waveform Data"));
  connect(_copy_this_channel_action, &QAction::triggered,
          this, &Viewport::copy_waveform_this_channel);
  connect(_copy_decoder_track_action, &QAction::triggered,
          this, &Viewport::copy_waveform_decoder_track);
  connect(_copy_decoder_group_action, &QAction::triggered,
          this, &Viewport::copy_waveform_decoder_group);
  connect(_copy_all_channels_action, &QAction::triggered,
          this, &Viewport::copy_waveform_all_channels);

  connect(&_fps_timer, &QTimer::timeout, this, [this]() {
    if (_paint_in_this_second > 0) {
      _fps = _max_frame_time;
      _max_frame_time = 0;
      _paint_in_this_second = 0;
    } else {
      _is_idle = true;
    }
  });
  _fps_timer.start(1000);

  _drag_frame_timer.setSingleShot(true);
  connect(&_drag_frame_timer, &QTimer::timeout, this,
          &Viewport::applyDragFrame);

  // Edge navigation buttons
  _prev_edge_btn = new EdgeNavButton(EdgeNavButton::Previous, this);
  _next_edge_btn = new EdgeNavButton(EdgeNavButton::Next, this);
  _prev_edge_btn->hide();
  _next_edge_btn->hide();
  connect(_prev_edge_btn, &EdgeNavButton::clicked, this,
          [this]() { _interaction->navigate_to_edge(EdgeNavButton::Previous); });
  connect(_next_edge_btn, &EdgeNavButton::clicked, this,
          [this]() { _interaction->navigate_to_edge(EdgeNavButton::Next); });

  // Construct delegates (Phase F1/F2/F3). They hold a non-owning back-pointer
  // and access Viewport's private state through friend access.
  _painter = std::make_unique<ViewportPainter>(this);
  _interaction = std::make_unique<ViewportInteraction>(this);
  _drag = std::make_unique<ViewportDrag>(this);

  ADD_UI(this);
}

Viewport::~Viewport() { REMOVE_UI(this); }

int Viewport::get_total_height() {
  int h = 0;
  std::vector<Trace *> traces;
  _view.get_traces(_type, traces);

  if (_view.is_logic_rendering_mode() && _type == TIME_VIEW) {
    const auto &groups = _view.get_signal_groups();
    if (!groups.empty()) {
      // Collect grouped traces so we can identify non-group traces below.
      // signal_groups only contains LOGIC + DECODER traces (see
      // compute_signal_groups). ANALOG/DSO/Math traces are NOT in any
      // group but are still laid out vertically and occupy space.
      // Without accounting for them here, get_total_height() only returns
      // the height of logic/decoder traces — if those fill the viewport,
      // the scrollbar range is 0 and the user cannot scroll to see the
      // analog channels below.
      std::vector<Trace *> grouped_traces;
      for (const auto &group : groups) {
        for (auto gt : group.traces) {
          grouped_traces.push_back(gt);
          h += (int)(gt->get_totalHeight()) + 2 * View::SignalMargin;
        }
        h += View::GroupGap + 5;
      }
      // Add heights for non-group traces (ANALOG, DSO, Math, etc.)
      for (auto t : traces) {
        bool in_group = false;
        for (auto gt : grouped_traces) {
          if (gt == t) {
            in_group = true;
            break;
          }
        }
        if (!in_group) {
          h += (int)(t->get_totalHeight()) + 2 * View::SignalMargin;
        }
      }
      return h;
    }
  }

  for (auto t : traces) {
    h += (int)(t->get_totalHeight()) + 2 * View::SignalMargin;
  }

  return h;
}

QPoint Viewport::get_mouse_point() { return _mouse_point; }

bool Viewport::event(QEvent *event) {
  if (event->type() == QEvent::NativeGesture)
    return gestureEvent(static_cast<QNativeGestureEvent *>(event));
  return QWidget::event(event);
}

//--- Qt override forwarders (Phase F1/F2/F3) -------------------------------

void Viewport::paintEvent(QPaintEvent *event) { _painter->paintEvent(event); }

void Viewport::mousePressEvent(QMouseEvent *event) {
  _interaction->mousePressEvent(event);
}

void Viewport::mouseMoveEvent(QMouseEvent *event) {
  _interaction->mouseMoveEvent(event);
}

void Viewport::mouseReleaseEvent(QMouseEvent *event) {
  _interaction->mouseReleaseEvent(event);
}

void Viewport::mouseDoubleClickEvent(QMouseEvent *event) {
  _interaction->mouseDoubleClickEvent(event);
}

void Viewport::wheelEvent(QWheelEvent *event) {
  _interaction->wheelEvent(event);
}

void Viewport::leaveEvent(QEvent *event) { _interaction->leaveEvent(event); }

void Viewport::keyPressEvent(QKeyEvent *event) {
  _interaction->keyPressEvent(event);
}

bool Viewport::gestureEvent(QNativeGestureEvent *event) {
  return _interaction->gestureEvent(event);
}

void Viewport::applyDragFrame() { _drag->applyDragFrame(); }

void Viewport::on_drag_timer() { _drag->on_drag_timer(); }

//--- Retained state helpers -------------------------------------------------

void Viewport::set_action(ActionType action) { _action_type = action; }

void Viewport::get_captured_progress(double &progress, int &progress100) {
  // Use the smoothly interpolated _progress_displayed (0.0–1.0) for
  // fluid animation instead of the raw _sample_received which jumps
  // in discrete steps at each data-packet arrival.
  progress = -(_progress_displayed * 360 * 16);
  progress100 = ceil(_progress_displayed * 100);
}

void Viewport::resizeEvent(QResizeEvent *e) {
  QWidget::resizeEvent(e);
  ViewStatus *vs = _view.get_viewstatus();
  if (vs) {
    int h = vs->height();
    vs->setGeometry(0, height() - h, width(), h);
  }
  clear_measure();
}

void Viewport::set_receive_len(quint64 length) {
  if (length == 0) {
    _sample_received = 0;
    _progress_displayed = 0.0;
    _progress_timer.stop();
    start_trigger_timer(333);
    _tigger_wait_times = 0;
    _is_checked_trig = false;
  } else {
    stop_trigger_timer();

    if (_sample_received + length > _view.session().cur_samplelimits())
      _sample_received = _view.session().cur_samplelimits();
    else
      _sample_received += length;

    // Start smooth-animation timer on first data arrival. It runs at
    // 60 FPS and lerps _progress_displayed toward the actual
    // _sample_received / sample_limits ratio, giving a fluid progress
    // bar regardless of how often data packets arrive.
    if (!_progress_timer.isActive())
      _progress_timer.start();
  }

  if (_view.is_logic_rendering_mode()) {
    auto *dev = _view.data_source()->device();
    if (dev && !dev->is_file()) {
      if (!_is_checked_trig && _view.session().is_triged()) {
        _view.get_viewstatus()->set_trig_time(_view.session().get_trig_time());
        _view.get_viewstatus()->update();
        _is_checked_trig = true;
      }
    }

    if (_view.session().is_repeat_mode()) {
      double progress = 0;
      int progress100 = 0;
      get_captured_progress(progress, progress100);
      _view.show_captured_progress(_transfer_started, progress100);

      if (_view.session().is_single_buffer()) {
        if (_view.session().have_new_realtime_refresh(true) == false) {
          return;
        }
      } else {
        return;
      }
    } else if (_view.session().is_realtime_refresh()) {
      if (_view.session().have_new_realtime_refresh(true) == false) {
        return;
      }
    }
  }

  if (_view.is_logic_rendering_mode() && AppConfig::Instance().appOptions.autoScrollLatestData &&
      _view.session().is_realtime_refresh()) {
    _view.scroll_to_logic_last_data_time();
  }

  // Received new data, and refresh the view.
  // For LOGIC mode in realtime refresh, we must set _need_update so that
  // paintSignals() rebuilds the pixmap even when scale/offset are unchanged
  // (e.g. full-scale view where auto-scroll doesn't change the offset).
  if (_view.is_logic_rendering_mode() && _view.session().is_realtime_refresh()) {
    _need_update = true;
  }

  // In DSO mode, the async DataUpdated event (broadcast_async from
  // feed_in_dso) already drives ViewDataSync::data_updated() which calls
  // viewport_update(). Calling update() here as well causes a redundant
  // repaint on every data packet (~40/sec). Skip it for DSO running mode;
  // the progress timer still fires for progress-bar animation.
  if (_view.get_work_mode() == DSO && _view.session().is_running_status()) {
    return;
  }

  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::update(int event) {
  QWidget::update();
  (void)event;
}

void Viewport::clear_measure() {
  _measure_type = NO_MEASURE;
  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::clear_dso_xm() {
  _dso_xm_valid = false;
  _mm_width = View::Unknown_Str;
  _mm_period = View::Unknown_Str;
  _mm_freq = View::Unknown_Str;
  _mm_duty = View::Unknown_Str;

  set_action(NO_ACTION);
}

void Viewport::measure() {
  if (_view.session().is_data_lock())
    return;
  if (_view.session().is_loop_mode() && _view.session().is_working())
    return;

  _measure_type = NO_MEASURE;

  if (_type == TIME_VIEW) {
    const uint64_t sample_rate = _view.session().cur_snap_samplerate();

    for (auto s : _view.get_own_signals()) {
      if (s->signal_type() == SR_CHANNEL_LOGIC) {
        view::LogicSignal *logicSig = (view::LogicSignal *)s;

        if (_action_type == NO_ACTION) {
          if (logicSig->measure(_mouse_point, _cur_sample, _nxt_sample,
                                _thd_sample)) {
            _measure_type = LOGIC_FREQ;

            _mm_width = _view.get_ruler()->format_real_time(
                _nxt_sample - _cur_sample, sample_rate);
            _mm_period = _thd_sample != 0
                             ? _view.get_ruler()->format_real_time(
                                   _thd_sample - _cur_sample, sample_rate)
                             : View::Unknown_Str;
            _mm_freq = _thd_sample != 0
                           ? _view.get_ruler()->format_real_freq(
                                 _thd_sample - _cur_sample, sample_rate)
                           : View::Unknown_Str;

            _cur_preX = _view.index2pixel(_cur_sample);
            _cur_aftX = _view.index2pixel(_nxt_sample);
            _cur_thdX = _view.index2pixel(_thd_sample);
            _cur_midY = logicSig->get_y();

            _mm_duty =
                _thd_sample != 0
                    ? QString::number((_nxt_sample - _cur_sample) * 100.0 /
                                          (_thd_sample - _cur_sample),
                                      'f', 2) +
                          "%"
                    : View::Unknown_Str;
            break;
          } else {
            _measure_type = NO_MEASURE;
            _mm_width = View::Unknown_Str;
            _mm_period = View::Unknown_Str;
            _mm_freq = View::Unknown_Str;
            _mm_duty = View::Unknown_Str;
          }
        } else if (_action_type == LOGIC_EDGE) {
          if (logicSig->edges(_view.hover_point(), _edge_start, _edge_rising,
                              _edge_falling)) {
            _cur_preX = _view.index2pixel(_edge_start);
            _cur_aftX = _view.hover_point().x();
            _cur_midY = logicSig->get_y() -
                        qRound(logicSig->get_totalHeight() * 0.5) - 5;

            _em_rising = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RISING), "Rising: ") +
                         QString::number(_edge_rising);
            _em_falling =
                L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FALLING), "Falling: ") +
                QString::number(_edge_falling);
            _em_edges = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_Edges_1), "Edges: ") +
                        QString::number(_edge_rising + _edge_falling);

            break;
          }
        } else if (_action_type == LOGIC_JUMP) {
          if (logicSig->edge(_view.hover_point(), _edge_end, 10)) {
            _cur_aftX = _view.index2pixel(_edge_end);
            _cur_aftY = logicSig->get_y();
            _edge_hit = true;
            break;
          } else {
            _cur_preX = _view.index2pixel(_edge_start);
            _cur_aftX = _view.hover_point().x();
            _cur_aftY = _view.hover_point().y();
            _edge_end = _view.pixel2index(_cur_aftX);
            _edge_hit = false;
          }
        }
      } else if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s;
        if (s->enabled()) {
          if (_measure_en && dsoSig->measure(_view.hover_point())) {
            _measure_type = DSO_VALUE;
          } else {
            _measure_type = NO_MEASURE;
          }
        }
      } else if (s->signal_type() == SR_CHANNEL_ANALOG) {
        view::AnalogSignal *analogSig = (view::AnalogSignal *)s;
        if (s->enabled()) {
          if (_measure_en && analogSig->measure(_view.hover_point())) {
            _measure_type = DSO_VALUE;
          } else {
            _measure_type = NO_MEASURE;
          }
        }
      }
    }
    const auto mathTrace = _view.get_own_math_trace();
    if (mathTrace && mathTrace->enabled()) {
      if (_measure_en && mathTrace->measure(_view.hover_point())) {
        _measure_type = DSO_VALUE;
      } else {
        _measure_type = NO_MEASURE;
      }
    }
  } else if (_type == FFT_VIEW) {
    for (auto t : _view.get_own_spectrum_traces()) {
      if (t->enabled()) {
        t->measure(_mouse_point);
      }
    }
  }

  measure_updated();
}

QString Viewport::get_measure(QString option) {
  if (option.compare("width") == 0)
    return _mm_width;
  else if (option.compare("period") == 0)
    return _mm_period;
  else if (option.compare("frequency") == 0)
    return _mm_freq;
  else if (option.compare("duty") == 0)
    return _mm_duty;
  else
    return View::Unknown_Str;
}

void Viewport::set_measure_en(int enable) {
  if (enable == 0)
    _measure_en = false;
  else
    _measure_en = true;
}

void Viewport::start_trigger_timer(int msec) {
  assert(msec > 0);
  _transfer_started = false;
  _timer_cnt = 0;
  _trigger_timer.start(msec);
}

void Viewport::stop_trigger_timer() {
  _transfer_started = true;
  _timer_cnt = 0;
  _trigger_timer.stop();
}

void Viewport::on_trigger_timer() {
  _timer_cnt++;

  if (!_is_checked_trig) {
    auto *dev = _view.data_source()->device();
    if (_view.is_logic_rendering_mode() &&
        dev && !dev->is_file()) {
      if (_view.session().is_triged()) {
        _is_checked_trig = true;
        _view.get_viewstatus()->set_trig_time(_view.session().get_trig_time());
        _view.get_viewstatus()->update();
      }
    } else {
      _is_checked_trig = true;
    }
  }

  if (_view.get_work_mode() == DSO) {
    update(UpdateEventType::UPDATE_EV_GENERIC);
  }
}

void Viewport::on_progress_timer() {
  const uint64_t sample_limits = _view.session().cur_samplelimits();
  if (sample_limits == 0) {
    _progress_displayed = 0.0;
    _progress_timer.stop();
    return;
  }

  const double target =
      static_cast<double>(_sample_received) / static_cast<double>(sample_limits);
  const double diff = target - _progress_displayed;

  if (qAbs(diff) < 0.0005) {
    // Close enough — snap to target and stop the timer. It will be
    // restarted by set_receive_len() when the next data packet arrives.
    // This prevents the 60 FPS timer from running idle between packets.
    _progress_displayed = target;
    _progress_timer.stop();
    update(UpdateEventType::UPDATE_EV_GENERIC);
    return;
  }

  // Exponential lerp: 25% of remaining distance per frame.
  // At 60 FPS this reaches 99% of target in ~15 frames (250ms),
  // which feels smooth without noticeable lag.
  _progress_displayed += diff * 0.25;
  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::set_need_update(bool update) { _need_update = update; }

void Viewport::set_decode_dirty() { _need_update = true; }

void Viewport::show_wait_trigger() {
  _waiting_trig %= (WaitLoopTime / SigSession::FeedInterval) * 4;
  _waiting_trig++;
  if (_view.get_work_mode() == DSO)
    update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::unshow_wait_trigger() {
  _waiting_trig = 0;
  if (_view.get_work_mode() == DSO)
    update(UpdateEventType::UPDATE_EV_GENERIC);
}

bool Viewport::get_dso_trig_moved() { return _dso_trig_moved; }

void Viewport::show_contextmenu(const QPoint &pos) {
  if (_cmenu && _view.get_work_mode() == DSO) {
    _cur_preX = pos.x();
    _cur_preY = pos.y();
    _cmenu->exec(QCursor::pos());
  }
}

void Viewport::add_cursor_y() {
  uint64_t index;
  index = _view.pixel2index(_cur_preX);
  _view.add_cursor(index);
  _view.show_cursors(true);
}

void Viewport::add_cursor_x() {
  double ypos =
      (_cur_preY - _view.get_view_rect().top()) * 1.0 / _view.get_view_height();
  _view.add_xcursor(ypos, ypos);
  _view.show_xcursors(true);
}

void Viewport::show_logic_contextmenu(const QPoint &pos) {
  if (!_logic_cmenu || !_view.is_logic_rendering_mode())
    return;

  _logic_menu_pos = pos;

  // Show/hide menu items based on hit test (hide unavailable instead of disable)
  LogicSignal *hit_sig = WaveformCopyHelper::hit_test_signal(
      _view, pos.x(), pos.y());
  DecodeTrace *hit_dt = (hit_sig == nullptr) ?
      WaveformCopyHelper::hit_test_decode_trace(_view, pos.x(), pos.y()) : nullptr;

  // "复制该通道波形数据" only visible when hit LogicSignal
  _copy_this_channel_action->setVisible(hit_sig != nullptr);

  // "复制该解码器波形数据" only visible when hit DecodeTrace
  _copy_decoder_track_action->setVisible(hit_dt != nullptr);

  bool has_decoder = false;
  if (hit_sig)
    has_decoder = WaveformCopyHelper::find_decoder_for_signal(_view, hit_sig) != nullptr;
  if (!has_decoder)
    has_decoder = (hit_dt != nullptr);
  _copy_decoder_group_action->setVisible(has_decoder);

  // "复制所有通道波形数据" always visible in logic mode — outputs all logic signals + decoders
  _copy_all_channels_action->setVisible(true);

  _logic_cmenu->exec(QCursor::pos());
}

void Viewport::copy_waveform_this_channel() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::ThisChannel);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::copy_waveform_decoder_track() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::DecoderTrack);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::copy_waveform_decoder_group() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::ThisDecoderGroup);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::copy_waveform_all_channels() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::AllChannels);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::UpdateLanguage() {
  _yAction->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_Y_CURSOR), "Add Y-cursor"));
  _xAction->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_X_CURSOR), "Add X-cursor"));
  _copy_this_channel_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_THIS_CHANNEL), "Copy Channel Waveform Data"));
  _copy_decoder_track_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_TRACK), "Copy Decoder Waveform Data"));
  _copy_decoder_group_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_GROUP), "Copy Decoder Group Waveform Data"));
  _copy_all_channels_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ALL_CHANNELS), "Copy All Channels Waveform Data"));
}

void Viewport::UpdateTheme() {
  _panelBgColor = AppConfig::Instance().GetThemeColor("@panel-bg");
  if (!_panelBgColor.isValid())
    _panelBgColor = QColor("#1a1a1a");
  _panelTextColor = AppConfig::Instance().GetThemeColor("@panel-text");
  if (!_panelTextColor.isValid())
    _panelTextColor = QColor("#f5f0e5");

  _prev_edge_btn->UpdateTheme();
  _next_edge_btn->UpdateTheme();

  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::UpdateFont() {
  QFont font = theme_font_cursor();
  _yAction->setFont(font);
  _xAction->setFont(font);
}

int Viewport::get_fps() { return _fps; }

} // namespace view
} // namespace pv
