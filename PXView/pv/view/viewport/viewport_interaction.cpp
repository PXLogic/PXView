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

#include "pv/view/viewport/viewport_interaction.h"
#include "pv/view/viewport/viewport.h"
#include "pv/view/component/ruler.h"
#include "pv/view/component/viewstatus.h"

#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/dialogs/dsomeasure.h"
#include "pv/session/sigsession.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/signal.h"
#include "pv/view/trace/spectrumtrace.h"

#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyleOption>
#include <QWheelEvent>
#include <QDateTime>
#include <cmath>
#include <set>

#include "pv/config/appconfig.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/fn.h"
#include "pv/ui/langresource.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/view/trace/mathtrace.h"
#include "pv/view/component/edge_nav_button.h"

using namespace std;

namespace pv {
namespace view {

ViewportInteraction::ViewportInteraction(Viewport *viewport)
    : _viewport(viewport), _last_wheel_zoom_ms(0) {}

ViewportInteraction::~ViewportInteraction() {}

void ViewportInteraction::mousePressEvent(QMouseEvent *event) {
  assert(event);

  // 1.5.3-ZB -> 1.5.7: Shift + left-drag selects a persistent range on
  // one decoder-generated analog waveform (TDM Fast / PWM Fast).
  if (_viewport->action_type() == NO_ACTION &&
      event->button() == Qt::LeftButton &&
      (event->modifiers() & Qt::ShiftModifier) &&
      _viewport->type() == TIME_VIEW &&
      _viewport->view().is_logic_rendering_mode()) {
    std::vector<Trace *> traces;
    _viewport->view().get_traces(TIME_VIEW, traces);
    const int y = event->position().toPoint().y();
    const int vo = _viewport->view().get_vOffset();
    for (Trace *trace : traces) {
      DecodeTrace *dt = trace ? trace->as_decode() : nullptr;
      if (!dt || !dt->enabled()) continue;
      int ch = -1;
      std::shared_ptr<pv::data::DecoderAnalogData> data;
      if (!dt->hit_test_analog_channel(y, vo, ch, data) || !data ||
          !data->visible())
        continue;
      const uint64_t sample = _viewport->view().pixel2index(
          event->position().toPoint().x());
      _viewport->analog_measure_data() = data;
      _viewport->analog_measure_channel() = ch;
      _viewport->analog_measure_start() = sample;
      _viewport->analog_measure_end() = sample;
      _viewport->analog_measure_stats() = pv::data::DecoderAnalogStatistics{};
      _viewport->analog_measure_cycle() = pv::data::DecoderAnalogCycleMetrics{};
      _viewport->analog_measure_valid() = true;
      _viewport->set_action(ANALOG_RANGE_DRAG);
      _viewport->setCursor(Qt::CrossCursor);
      _viewport->update(UpdateEventType::UPDATE_EV_GENERIC);
      return;
    }
  }

  _viewport->mouse_down_point() = event->position().toPoint();
  _viewport->mouse_down_offset() = _viewport->view().offset();
  _viewport->drag_strength() = 0;
  _viewport->elapsed_time().restart();

  if (_viewport->type() == TIME_VIEW &&
      _viewport->view().is_logic_rendering_mode()) {
    std::vector<Trace *> traces;
    _viewport->view().get_traces(TIME_VIEW, traces);
    int mouseY = event->position().toPoint().y() +
                 _viewport->view().get_vOffset();
    const int HitBorderMargin = 5;

    std::vector<Trace *> enabled_traces;
    for (auto t : traces)
      if (t->enabled())
        enabled_traces.push_back(t);

    for (int i = 0; i < (int)enabled_traces.size() - 1; i++) {
      int traceBottom = enabled_traces[i]->get_v_offset() +
                        enabled_traces[i]->get_totalHeight() / 2 +
                        View::SignalMargin;

      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        _viewport->action_type() = RESIZE_SIGNAL;
        _viewport->resize_trace_upper() = enabled_traces[i];
        _viewport->resize_trace_lower() = enabled_traces[i + 1];
        _viewport->resize_mouse_down_y() = event->position().toPoint().y();
        _viewport->resize_upper_height() = enabled_traces[i]->get_totalHeight();
        _viewport->resize_lower_height() =
            enabled_traces[i + 1]->get_totalHeight();
        return;
      }
    }

    // Check bottom border of the last enabled trace
    if (!enabled_traces.empty()) {
      Trace *lastTrace = enabled_traces.back();
      int traceBottom = lastTrace->get_v_offset() +
                        lastTrace->get_totalHeight() / 2 +
                        View::SignalMargin;
      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        _viewport->action_type() = RESIZE_SIGNAL;
        _viewport->resize_trace_upper() = lastTrace;
        _viewport->resize_trace_lower() = nullptr;
        _viewport->resize_mouse_down_y() = event->position().toPoint().y();
        _viewport->resize_upper_height() = lastTrace->get_totalHeight();
        _viewport->resize_lower_height() = 0;
        return;
      }
    }
  }

  if (_viewport->action_type() == NO_ACTION &&
      event->button() == Qt::RightButton &&
      _viewport->view().session().is_stopped_status()) {
    if (_viewport->view().is_logic_rendering_mode()) {
      _viewport->set_action(LOGIC_ZOOM);
    } else if (_viewport->view().get_work_mode() == DSO) {
      if (_viewport->hover_hit()) {
        const int64_t index = _viewport->view().pixel2index(
            event->position().toPoint().x());
        _viewport->view().add_cursor(index);
        _viewport->view().show_cursors(true);
      }
    }
  }

  if (_viewport->action_type() == NO_ACTION &&
      event->button() == Qt::LeftButton &&
      _viewport->view().get_work_mode() == DSO) {

    // Use the raw event position (without vOffset) for the trigger rect
    // hit test. _mouse_point includes vOffset, but get_trig_rect() returns
    // a rect in viewport-widget coordinates (no vOffset). Without this fix,
    // any vertical scroll makes the T cursor impossible to grab.
    const QPoint clickPos = event->position().toPoint();

    for (auto &s : _viewport->view().get_own_signals()) {
      if (s->signal_type() == SR_CHANNEL_DSO && s->enabled()) {
        DsoSignal *dsoSig = (DsoSignal *)s.get();
        QRectF trigRect = dsoSig->get_trig_rect(0, _viewport->view().get_view_width());
        if (trigRect.contains(clickPos)) {
          _viewport->drag_sig() = s.get();
          _viewport->set_action(DSO_TRIG_MOVE);
          dsoSig->select(true);
          break;
        }
      }
    }
  }

  if (_viewport->action_type() == NO_ACTION &&
      event->button() == Qt::LeftButton) {
    if (_viewport->action_type() == NO_ACTION &&
        _viewport->view().search_cursor_shown()) {
      const int64_t searchX = _viewport->view().index2pixel(
          _viewport->view().get_search_cursor()->index());

      if (_viewport->view().get_search_cursor()->grabbed()) {
        _viewport->view().get_ruler()->rel_grabbed_cursor();
      } else if (qAbs(searchX - event->position().toPoint().x()) <=
                 Viewport::HitCursorMargin) {
        _viewport->view().get_ruler()->set_grabbed_cursor(
            _viewport->view().get_search_cursor());
        _viewport->set_action(CURS_MOVE);
      }
    }

    if (_viewport->action_type() == NO_ACTION &&
        _viewport->view().cursors_shown()) {
      auto &cursor_list = _viewport->view().get_cursorList();
      auto i = cursor_list.begin();

      while (i != cursor_list.end()) {
        const int64_t cursorX = _viewport->view().index2pixel((*i)->index());
        if ((*i)->grabbed()) {
          _viewport->view().get_ruler()->rel_grabbed_cursor();
        } else if (qAbs(cursorX - event->position().toPoint().x()) <=
                   Viewport::HitCursorMargin) {
          _viewport->view().get_ruler()->set_grabbed_cursor(i->get());
          _viewport->set_action(CURS_MOVE);
          break;
        }
        i++;
      }
    }

    if (_viewport->action_type() == NO_ACTION &&
        _viewport->view().xcursors_shown()) {
      auto &xcursor_list = _viewport->view().get_xcursorList();
      auto i = xcursor_list.begin();
      const QRect xrect = _viewport->view().get_view_rect();

      while (i != xcursor_list.end()) {
        const double cursorX =
            xrect.left() + (*i)->value(XCursor::XCur_Y) * xrect.width();
        const double cursorY0 =
            xrect.top() + (*i)->value(XCursor::XCur_X0) * xrect.height();
        const double cursorY1 =
            xrect.top() + (*i)->value(XCursor::XCur_X1) * xrect.height();

        if ((*i)->get_close_rect(xrect).contains(
                _viewport->view().hover_point())) {
          _viewport->view().del_xcursor(i->get());
          if (xcursor_list.empty())
            _viewport->view().show_xcursors(false);
          break;
        } else if ((*i)->get_map_rect(xrect).contains(
                       _viewport->view().hover_point())) {
          auto &sigs = _viewport->view().get_own_signals();
          auto s = sigs.begin();
          bool sig_looped = ((*i)->channel() == nullptr);
          bool no_dsoSig = true;

          while (true) {
            if ((*s)->signal_type() == SR_CHANNEL_DSO && (*s)->enabled()) {
              view::DsoSignal *dsoSig = (view::DsoSignal *)s->get();
              no_dsoSig = false;
              if (sig_looped) {
                (*i)->set_channel(dsoSig);
                break;
              } else if (dsoSig == (*i)->channel()) {
                sig_looped = true;
              }
            }

            s++;
            if (s == sigs.end()) {
              if (no_dsoSig) {
                (*i)->set_channel(nullptr);
                break;
              }
              sig_looped = true;
              s = sigs.begin();
            }
          }
          break;
        } else if ((*i)->grabbed() != XCursor::XCur_None) {
          (*i)->set_grabbed((*i)->grabbed(), false);
        } else if (qAbs(cursorX - _viewport->view().hover_point().x()) <=
                       Viewport::HitCursorMargin &&
                   _viewport->view().hover_point().y() >
                       min(cursorY0, cursorY1) &&
                   _viewport->view().hover_point().y() <
                       max(cursorY0, cursorY1)) {
          (*i)->set_grabbed(XCursor::XCur_Y, true);
          _viewport->set_action(CURS_MOVE);
          break;
        } else if (qAbs(cursorY0 - _viewport->view().hover_point().y()) <=
                   Viewport::HitCursorMargin) {
          (*i)->set_grabbed(XCursor::XCur_X0, true);
          _viewport->set_action(CURS_MOVE);
          break;
        } else if (qAbs(cursorY1 - _viewport->view().hover_point().y()) <=
                   Viewport::HitCursorMargin) {
          (*i)->set_grabbed(XCursor::XCur_X1, true);
          _viewport->set_action(CURS_MOVE);
          break;
        }
        i++;
      }
    }
  }
}

void ViewportInteraction::mouseMoveEvent(QMouseEvent *event) {
  assert(event);
  _viewport->hover_hit() = false;

  if (_viewport->action_type() == ANALOG_RANGE_DRAG) {
    _viewport->analog_measure_end() = _viewport->view().pixel2index(
        _viewport->mouse_point().x());
    _viewport->update(UpdateEventType::UPDATE_EV_GENERIC);
    return;
  }

  if (_viewport->action_type() == NO_ACTION && _viewport->type() == TIME_VIEW &&
      _viewport->view().is_logic_rendering_mode()) {
    std::vector<Trace *> traces;
    _viewport->view().get_traces(TIME_VIEW, traces);
    int mouseY =
        event->position().toPoint().y() + _viewport->view().get_vOffset();
    const int HitBorderMargin = 5;
    bool onBorder = false;

    std::vector<Trace *> enabled_traces;
    for (auto t : traces)
      if (t->enabled())
        enabled_traces.push_back(t);

    for (int i = 0; i < (int)enabled_traces.size() - 1; i++) {
      int traceBottom = enabled_traces[i]->get_v_offset() +
                        enabled_traces[i]->get_totalHeight() / 2 +
                        View::SignalMargin;

      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        onBorder = true;
        break;
      }
    }

    // Check bottom border of the last enabled trace
    if (!onBorder && !enabled_traces.empty()) {
      Trace *lastTrace = enabled_traces.back();
      int traceBottom = lastTrace->get_v_offset() +
                        lastTrace->get_totalHeight() / 2 +
                        View::SignalMargin;
      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        onBorder = true;
      }
    }

    _viewport->setCursor(onBorder ? Qt::SplitVCursor : Qt::ArrowCursor);
  }

  bool is_drag_action =
      (_viewport->action_type() == RESIZE_SIGNAL ||
       _viewport->action_type() == DSO_TRIG_MOVE ||
       _viewport->action_type() == CURS_MOVE) ||
      ((event->buttons() & Qt::LeftButton) &&
       (_viewport->type() == TIME_VIEW || _viewport->type() == FFT_VIEW));

  if (is_drag_action) {
    _viewport->drag_last_pos() = event->position().toPoint();
    _viewport->drag_buttons() = event->buttons();
    if (!_viewport->drag_frame_pending()) {
      _viewport->drag_frame_pending() = true;
      _viewport->applyDragFrame();
      _viewport->drag_frame_timer().start(Viewport::DragFrameInterval);
    }
    _viewport->mouse_point() =
        event->position().toPoint() + QPoint(0, _viewport->view().get_vOffset());
    return;
  }

  if (!(event->buttons() | Qt::NoButton)) {
    if (_viewport->action_type() == DSO_XM_STEP1 ||
        _viewport->action_type() == DSO_XM_STEP2) {
      for (auto &s : _viewport->view().get_own_signals()) {
        if (!s->get_view_rect().contains(event->position().toPoint())) {
          _viewport->clear_dso_xm();
        }
        break;
      }
    }

    if (_viewport->action_type() == DSO_YM)
      _viewport->dso_ym_end() = event->position().toPoint().y();
  }

  _viewport->mouse_point() =
      event->position().toPoint() + QPoint(0, _viewport->view().get_vOffset());

  _viewport->measure();

  update_edge_nav_buttons();

  _viewport->update(UpdateEventType::UPDATE_EV_MS_MOVE);
}

void ViewportInteraction::onLogicMouseRelease(QMouseEvent *event) {
  bool quickScroll = AppConfig::Instance().appOptions.quickScroll;
  QWidget *topWin = _viewport->view().window();
  bool isMaxWindow = topWin ? topWin->isMaximized() : false;

  switch (_viewport->action_type()) {
  case NO_ACTION: {
    if (event->button() == Qt::LeftButton &&
        _viewport->view().session().is_stopped_status()) {
      // priority 1
      // try to quick scroll view...
      int curX = event->position().toPoint().x();
      int clickX = _viewport->mouse_down_point().x();
      int moveLong = ABS_VAL(curX - clickX);
      int maxWidth = _viewport->geometry().width();
      float mvk = (float)moveLong / (float)maxWidth;

      if (quickScroll) {
        quickScroll = false;
        if (isMaxWindow && mvk > 0.4f) {
          quickScroll = true;
        } else if (!isMaxWindow && mvk > 0.25f) {
          quickScroll = true;
        }
      }

      if (_viewport->action_type() == NO_ACTION && quickScroll) {
        const double strength = _viewport->drag_strength() *
                                Viewport::DragTimerInterval * 1.0 /
                                _viewport->elapsed_time().elapsed();
        if (_viewport->elapsed_time().elapsed() < 200 &&
            abs(_viewport->drag_strength()) < Viewport::MinorDragOffsetUp &&
            abs(strength) > Viewport::MinorDragRateUp) {
          _viewport->drag_timer().start(Viewport::DragTimerInterval);
          _viewport->set_action(LOGIC_MOVE);
        } else if (_viewport->elapsed_time().elapsed() < 200 &&
                   abs(strength) > Viewport::DragTimerInterval) {
          _viewport->drag_strength() = strength * 5;
          _viewport->drag_timer().start(Viewport::DragTimerInterval);
          _viewport->set_action(LOGIC_MOVE);
        }
      }

      // priority 2
      if (_viewport->action_type() == NO_ACTION) {
        if (_viewport->mouse_down_point().x() ==
            event->position().toPoint().x()) {
          const auto &sigs = _viewport->view().get_own_signals();

          for (auto &s : sigs) {
            if (s->signal_type() == SR_CHANNEL_LOGIC) {
              view::LogicSignal *logicSig = (view::LogicSignal *)s.get();
              if (logicSig->is_by_edge(event->position().toPoint(),
                                       _viewport->edge_start(), 10)) {
                _viewport->set_action(LOGIC_JUMP);
                _viewport->cur_preX() =
                    _viewport->view().index2pixel(_viewport->edge_start());
                _viewport->cur_preY() = logicSig->get_y();
                _viewport->cur_preY_top() =
                    logicSig->get_y() -
                    qRound(logicSig->get_totalHeight() * 0.5) - 12;
                _viewport->cur_preY_bottom() =
                    logicSig->get_y() + logicSig->get_totalHeight() / 2 + 2;
                _viewport->cur_aftX() = _viewport->cur_preX();
                _viewport->cur_aftY() = _viewport->cur_preY();

                break;
              }
            }
          }
        }
      }

      // priority 3
      if (_viewport->action_type() == NO_ACTION) {
        if (_viewport->mouse_down_point().x() ==
            event->position().toPoint().x()) {
          const auto &sigs = _viewport->view().get_own_signals();

          for (auto &s : sigs) {
            if (abs(event->position().toPoint().y() - s->get_y()) <
                _viewport->view().get_signalHeight()) {
              _viewport->set_action(LOGIC_EDGE);
              _viewport->edge_start() = _viewport->view().pixel2index(
                  event->position().toPoint().x());
              break;
            }
          }
        }
      }
    }
    break;
  }
  case LOGIC_EDGE: {
    _viewport->set_action(NO_ACTION);
    _viewport->edge_rising() = 0;
    _viewport->edge_falling() = 0;
    break;
  }
  case LOGIC_JUMP: {
    _viewport->set_action(NO_ACTION);
    _viewport->edge_rising() = 0;
    _viewport->edge_falling() = 0;
    _viewport->edge_hit() = false;
    break;
  }
  case LOGIC_MOVE: {
    if (_viewport->mouse_down_point() == event->position().toPoint()) {
      _viewport->drag_strength() = 0;
      _viewport->drag_timer().stop();
      _viewport->set_action(NO_ACTION);
    } else {
      const double strength = _viewport->drag_strength() *
                              Viewport::DragTimerInterval * 1.0 /
                              _viewport->elapsed_time().elapsed();
      if (_viewport->elapsed_time().elapsed() < 200 &&
          abs(_viewport->drag_strength()) < Viewport::MinorDragOffsetUp &&
          abs(strength) > Viewport::MinorDragRateUp) {
        _viewport->drag_timer().start(Viewport::DragTimerInterval);
      } else if (_viewport->elapsed_time().elapsed() < 200 &&
                 abs(strength) > Viewport::DragTimerInterval) {
        _viewport->drag_strength() = strength * 5;
        _viewport->drag_timer().start(Viewport::DragTimerInterval);
      } else {
        _viewport->drag_strength() = 0;
        _viewport->drag_timer().stop();
        _viewport->set_action(NO_ACTION);
      }
    }
    break;
  }
  case LOGIC_ZOOM: {
    const QPoint releasePos = event->position().toPoint();
    const QPoint downPos = _viewport->mouse_down_point();
    const int dx = abs(releasePos.x() - downPos.x());
    const int dy = abs(releasePos.y() - downPos.y());
    const int dragThreshold = 5;

    if (dx < dragThreshold && dy < dragThreshold) {
      // Right-click without drag: show context menu
      _viewport->set_action(NO_ACTION);
      _viewport->show_logic_contextmenu(releasePos);
    } else if (releasePos.x() != downPos.x()) {
      int64_t newOffset =
          _viewport->view().offset() +
          (min(releasePos.x(), downPos.x()));
      const double newScale = max(
          min(_viewport->view().scale() *
                  abs(releasePos.x() - downPos.x()) /
                  _viewport->view().get_view_width(),
              _viewport->view().get_maxscale()),
          _viewport->view().get_minscale());
      newOffset = floor(newOffset * (_viewport->view().scale() / newScale));
      if (newScale != _viewport->view().scale())
        _viewport->view().set_scale_offset(newScale, newOffset);
      _viewport->set_action(NO_ACTION);
    } else {
      _viewport->set_action(NO_ACTION);
    }
    break;
  }
  default:
    break;
  }
}

void ViewportInteraction::onDsoMouseRelease(QMouseEvent *event) {
  switch (_viewport->action_type()) {
  case NO_ACTION: {
    if (event->button() == Qt::LeftButton && _viewport->hover_hit()) {
      _viewport->set_action(DSO_YM);
      _viewport->dso_ym_valid() = true;
      _viewport->dso_ym_sig_index() = _viewport->hover_sig_index();
      _viewport->dso_ym_sig_value() = _viewport->hover_sig_value();
      _viewport->dso_ym_index() = _viewport->hover_index();
      _viewport->dso_ym_start() = event->position().toPoint().y();
    }
    break;
  }
  case DSO_YM: {
    if (event->button() == Qt::LeftButton) {
      _viewport->dso_ym_end() = event->position().toPoint().y();
      _viewport->set_action(NO_ACTION);
    } else if (event->button() == Qt::RightButton) {
      _viewport->set_action(NO_ACTION);
      _viewport->dso_ym_valid() = false;
    }
    break;
  }
  case DSO_TRIG_MOVE: {
    if (_viewport->dso_trig_moved() && event->button() == Qt::LeftButton) {
      _viewport->drag_sig() = nullptr;
      _viewport->set_action(NO_ACTION);
      _viewport->dso_trig_moved() = false;

      std::vector<Trace *> traces;
      _viewport->view().get_traces(ALL_VIEW, traces);

      for (auto t : traces) {
        t->select(false);
      }
    }
    break;
  }
  case DSO_XM_STEP0: {
    if (event->button() == Qt::LeftButton) {
      _viewport->set_action(DSO_XM_STEP1);
      _viewport->dso_xm_valid() = true;
    }
    break;
  }
  case DSO_XM_STEP1: {
    if (event->button() == Qt::LeftButton) {
      _viewport->dso_xm_index()[1] =
          _viewport->view().pixel2index(event->position().toPoint().x());
      const uint64_t max_index =
          max(_viewport->dso_xm_index()[0], _viewport->dso_xm_index()[1]);
      _viewport->dso_xm_index()[0] =
          min(_viewport->dso_xm_index()[0], _viewport->dso_xm_index()[1]);
      _viewport->dso_xm_index()[1] = max_index;

      _viewport->set_action(DSO_XM_STEP2);
    } else if (event->button() == Qt::RightButton) {
      _viewport->clear_dso_xm();
      _viewport->measure_updated();
    }
    break;
  }
  case DSO_XM_STEP2: {
    if (event->button() == Qt::LeftButton) {
      _viewport->dso_xm_index()[2] =
          _viewport->view().pixel2index(event->position().toPoint().x());
      uint64_t max_index =
          max(_viewport->dso_xm_index()[1], _viewport->dso_xm_index()[2]);
      _viewport->dso_xm_index()[1] =
          min(_viewport->dso_xm_index()[1], _viewport->dso_xm_index()[2]);
      _viewport->dso_xm_index()[2] = max_index;

      max_index =
          max(_viewport->dso_xm_index()[0], _viewport->dso_xm_index()[1]);
      _viewport->dso_xm_index()[0] =
          min(_viewport->dso_xm_index()[0], _viewport->dso_xm_index()[1]);
      _viewport->dso_xm_index()[1] = max_index;

      _viewport->set_action(NO_ACTION);
    } else if (event->button() == Qt::RightButton) {
      _viewport->clear_dso_xm();
      _viewport->measure_updated();
    }
    break;
  }
  default:
    break;
  }
}

void ViewportInteraction::onAnalogMouseRelease(QMouseEvent *event) {
  (void)event;
}

void ViewportInteraction::mouseReleaseEvent(QMouseEvent *event) {
  assert(event);

  if (_viewport->action_type() == ANALOG_RANGE_DRAG) {
    _viewport->analog_measure_end() = _viewport->view().pixel2index(
        event->position().toPoint().x());
    _viewport->analog_measure_stats() = pv::data::DecoderAnalogStatistics{};
    _viewport->analog_measure_cycle() = pv::data::DecoderAnalogCycleMetrics{};
    _viewport->analog_measure_valid() =
        _viewport->analog_measure_data() &&
        _viewport->analog_measure_data()->get_statistics(
            _viewport->analog_measure_start(),
            _viewport->analog_measure_end(),
            _viewport->analog_measure_stats());
    if (_viewport->analog_measure_valid()) {
      _viewport->analog_measure_data()->get_range_cycle_metrics(
          _viewport->analog_measure_start(),
          _viewport->analog_measure_end(),
          _viewport->analog_measure_cycle());
    }
    _viewport->set_action(NO_ACTION);
    _viewport->setCursor(Qt::ArrowCursor);
    _viewport->update(UpdateEventType::UPDATE_EV_GENERIC);
    return;
  }

  _viewport->drag_frame_timer().stop();
  if (_viewport->drag_frame_pending()) {
    _viewport->drag_frame_pending() = false;
    _viewport->drag_last_pos() = event->position().toPoint();
    _viewport->drag_buttons() = event->buttons();
    _viewport->applyDragFrame();
  }

  if (_viewport->type() != TIME_VIEW) {
    _viewport->update(UpdateEventType::UPDATE_EV_MS_UP);
    return;
  }

  int mode = _viewport->view().get_work_mode();

  if (_viewport->action_type() == RESIZE_SIGNAL) {
    _viewport->resize_trace_upper() = nullptr;
    _viewport->resize_trace_lower() = nullptr;
    _viewport->set_action(NO_ACTION);
    return;
  }

  if (_viewport->view().is_logic_rendering_mode()) {
    onLogicMouseRelease(event);
  } else if (mode == DSO) {
    onDsoMouseRelease(event);
  } else if (mode == ANALOG) {
    onAnalogMouseRelease(event);
  }

  if (_viewport->action_type() == CURS_MOVE) {
    if (_viewport->curs_moved() && event->button() == Qt::LeftButton) {
      _viewport->set_action(NO_ACTION);
      _viewport->view().get_ruler()->rel_grabbed_cursor();
      _viewport->view().cursor_moved();
      _viewport->curs_moved() = false;
    }
    if (_viewport->xcurs_moved() && event->button() == Qt::LeftButton) {
      _viewport->set_action(NO_ACTION);
      auto &xcursor_list = _viewport->view().get_xcursorList();
      auto i = xcursor_list.begin();

      while (i != xcursor_list.end()) {
        (*i)->rel_grabbed();
        i++;
      }

      _viewport->xcurs_moved() = false;
    }
  }

  /*
  // This code block prevents the cursor from moving.
  if (_viewport->view().is_logic_rendering_mode() && event->button() == Qt::LeftButton){
      int clickX = _mouse_down_point.x();
      const int64_t index = _view.pixel2index(clickX);
      const int64_t total = _view.session().get_ring_sample_count();
      if (index > total){
          _measure_type = MeasureType::NO_MEASURE;
          set_action(NO_ACTION);
      }
  }
  */

  _viewport->update(UpdateEventType::UPDATE_EV_MS_UP);
}

void ViewportInteraction::mouseDoubleClickEvent(QMouseEvent *event) {
  assert(event);

  if (!_viewport->view().get_view_rect().contains(event->position().toPoint()))
    return;

  if (_viewport->type() == TIME_VIEW &&
      _viewport->view().is_logic_rendering_mode()) {
    std::vector<Trace *> traces;
    _viewport->view().get_traces(TIME_VIEW, traces);
    int mouseY =
        event->position().toPoint().y() + _viewport->view().get_vOffset();
    const int HitBorderMargin = 5;

    std::vector<Trace *> enabled_traces;
    for (auto t : traces)
      if (t->enabled())
        enabled_traces.push_back(t);

    for (int i = 0; i < (int)enabled_traces.size() - 1; i++) {
      int traceBottom = enabled_traces[i]->get_v_offset() +
                        enabled_traces[i]->get_totalHeight() / 2 +
                        View::SignalMargin;

      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        enabled_traces[i]->set_own_height(-1);
        enabled_traces[i + 1]->set_own_height(-1);
        _viewport->view().signals_changed(nullptr);
        return;
      }
    }

    // Check bottom border of the last enabled trace
    if (!enabled_traces.empty()) {
      Trace *lastTrace = enabled_traces.back();
      int traceBottom = lastTrace->get_v_offset() +
                        lastTrace->get_totalHeight() / 2 +
                        View::SignalMargin;
      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        lastTrace->set_own_height(-1);
        _viewport->view().signals_changed(nullptr);
        return;
      }
    }
  }

  if (_viewport->view().is_logic_rendering_mode()) {
    if (event->button() == Qt::RightButton) {
      if (_viewport->view().scale() == _viewport->view().get_maxscale())
        _viewport->view().set_preScale_preOffset();
      else
        _viewport->view().set_scale_offset(
            _viewport->view().get_maxscale(), _viewport->view().get_min_offset());
    } else if (event->button() == Qt::LeftButton) {
      bool logic = false;
      uint64_t index;
      uint64_t index0 = 0, index1 = 0, index2 = 0;

      if (_viewport->view().is_logic_rendering_mode()) {
        for (auto &s : _viewport->view().get_own_signals()) {
          if (s->signal_type() == SR_CHANNEL_LOGIC) {
            view::LogicSignal *logicSig = (view::LogicSignal *)s.get();
            if (logicSig->measure(event->position().toPoint(), index0, index1,
                                  index2)) {
              logic = true;
              break;
            }
          }
        }
      }
      const double curX = event->position().toPoint().x();
      const double curP = _viewport->view().index2pixel(index0);
      const double curN = _viewport->view().index2pixel(index1);

      if (logic &&
          (curX - curP < Viewport::SnapMinSpace ||
           curN - curX < Viewport::SnapMinSpace)) {
        if (curX - curP < curN - curX)
          index = index0;
        else
          index = index1;
      } else {
        index = _viewport->view().pixel2index(curX);
      }

      _viewport->view().add_cursor(index);
      _viewport->view().show_cursors(true);
    }

    _viewport->update(UpdateEventType::UPDATE_EV_MS_CLICK);
  } else if (_viewport->view().get_work_mode() == DSO &&
             _viewport->view().session().is_init_status() == false &&
             event->button() == Qt::LeftButton) {
    if (_viewport->dso_xm_valid()) {
      _viewport->clear_dso_xm();
      _viewport->measure_updated();
    } else if (_viewport->action_type() == NO_ACTION) {
      for (auto &s : _viewport->view().get_own_signals()) {
        if (s->get_view_rect().contains(event->position().toPoint())) {
          _viewport->dso_xm_index()[0] = _viewport->view().pixel2index(
              event->position().toPoint().x());
          _viewport->dso_xm_y() = event->position().toPoint().y();
          _viewport->set_action(DSO_XM_STEP0);
        }
        break;
      }
    }
  } else if (_viewport->view().get_work_mode() == ANALOG) {
    if (event->button() == Qt::LeftButton) {
      uint64_t index;
      const double curX = event->position().toPoint().x();
      index = _viewport->view().pixel2index(curX);
      _viewport->view().add_cursor(index);
      _viewport->view().show_cursors(true);
    }
  }
}

void ViewportInteraction::wheelEvent(QWheelEvent *event) {
  assert(event);

  if (_viewport->view().header_is_draging()) {
    return;
  }

  int x = event->position().toPoint().x();
  // Windows 把 Shift+滚轮 转成水平滚动事件 (angleDelta().y()==0,
  // angleDelta().x()!=0)，所以这里取绝对值较大的那个方向作为 delta，
  // 否则 Shift/Alt+滚轮 的平移会因 delta=0 而无效。
  const QPoint ad = event->angleDelta();
  const int anglex = ad.x();
  const int angley = ad.y();
  int delta;
  if (anglex == 0 || ABS_VAL(angley) >= ABS_VAL(anglex)) {
    delta = angley;
  } else {
    delta = anglex;
  }
  bool isVertical = (angley != 0);

  double zoom_scale = delta / 80.0;

  if (ABS_VAL(delta) <= 80) {
    zoom_scale = delta > 0 ? 1.5 : -1.5;
  }

  if (_viewport->type() == FFT_VIEW) {
    for (auto &t : _viewport->view().get_own_spectrum_traces()) {
      if (t->enabled()) {
        t->zoom(zoom_scale, x);
        break;
      }
    }
  } else if (_viewport->type() == TIME_VIEW) {
    static bool bLstTime = false;

    if (event->modifiers() & Qt::ControlModifier) {
      double vsteps = delta / 80;
      if (ABS_VAL(delta) <= 80)
        vsteps = delta > 0 ? 1.5 : -1.5;
      _viewport->view().zoom_vertical(vsteps);
      return;
    }

    if (event->modifiers() & Qt::ShiftModifier) {
      int vOffset = _viewport->view().get_vOffset();
      vOffset -= delta;
      vOffset = max(0, vOffset);
      _viewport->view().verticalScrollBar()->setSliderPosition(vOffset);
      return;
    }

    if (event->modifiers() & Qt::AltModifier) {
      _viewport->view().set_scale_offset(_viewport->view().scale(),
                                        _viewport->view().offset() - delta);
      return;
    }

    if (isVertical) {
      // Vertical scrolling is interpreted as zooming in/out
#ifdef Q_OS_DARWIN
      static int64_t last_time;

      if (event->source() == Qt::MouseEventSynthesizedBySystem) {
        if (!bLstTime) {
          last_time = QDateTime::currentMSecsSinceEpoch();
          bLstTime = true;
        } else {
          int64_t cur_time = QDateTime::currentMSecsSinceEpoch();
          if (cur_time - last_time > 50) {
            double scale = delta > 1.5 ? 1 : (delta < -1.5 ? -1 : 0);
            _viewport->view().zoom(scale, x);
            last_time = QDateTime::currentMSecsSinceEpoch();
          }
        }
      } else {
        _viewport->view().zoom(-zoom_scale, x);
      }
#else
      // 性能修复: Windows 路径加 16ms 节流 (60fps 上限)，合并高精度滚轮/触控板
      // 的密集 tick。旧代码每个 wheel event 同步触发 zoom→viewport_update→重绘链路，
      // 配合模拟通道逐样本绘制导致严重卡顿 (macOS 路径已有节流，Windows 被遗漏)。
      // 16ms: 配合 envelope 优化后单帧已 <5ms, 60fps 既跟手又避免事件堆积。
      // (旧值 50ms=20fps, 每 tick 1.5x 跳跃过大, 用户感觉 "卡在分界线")。
      const int64_t cur_ms = QDateTime::currentMSecsSinceEpoch();
      if (cur_ms - _last_wheel_zoom_ms > 16) {
        _viewport->view().zoom(zoom_scale, x);
        _last_wheel_zoom_ms = cur_ms;
      }
#endif
    } else {
      bLstTime = false;
      (void)bLstTime;

      // Horizontal scrolling is interpreted as moving left/right
      if (!(event->modifiers() & Qt::ShiftModifier))
        _viewport->view().set_scale_offset(_viewport->view().scale(),
                                          _viewport->view().offset() - delta);
    }
  }

  const auto &sigs = _viewport->view().get_own_signals();
  for (auto &s : sigs) {
    if (s->signal_type() == SR_CHANNEL_DSO) {
      view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
      dsoSig->auto_end();
    }
  }

  _viewport->measure();
}

bool ViewportInteraction::gestureEvent(QNativeGestureEvent *event) {
  static double total_scale = 0;
  switch (event->gestureType()) {
  case Qt::BeginNativeGesture:
    break;
  case Qt::EndNativeGesture:
    total_scale = 0;
    break;
  case Qt::ZoomNativeGesture: {
    total_scale += event->value() * 2;
    if (_viewport->view().zoom(total_scale, _viewport->view().hover_point().x()))
      total_scale = 0;
  } break;
  case Qt::SmartZoomNativeGesture:
    _viewport->view().zoom(-1, _viewport->view().hover_point().x());
    break;
  default:
    return _viewport->forward_event(event);
  }

  _viewport->measure();
  return true;
}

void ViewportInteraction::leaveEvent(QEvent *) {
  _viewport->mouse_point() = QPoint(-1, -1);

  if (_viewport->action_type() == LOGIC_EDGE) {
    _viewport->edge_rising() = 0;
    _viewport->edge_falling() = 0;
    _viewport->set_action(NO_ACTION);
  } else if (_viewport->action_type() == LOGIC_JUMP) {
    _viewport->edge_rising() = 0;
    _viewport->edge_falling() = 0;
    _viewport->set_action(NO_ACTION);
  } else if (_viewport->action_type() == LOGIC_MOVE) {
    _viewport->drag_strength() = 0;
    _viewport->drag_timer().stop();
    _viewport->set_action(NO_ACTION);
  } else if (_viewport->action_type() == DSO_XM_STEP1 ||
             _viewport->action_type() == DSO_XM_STEP2) {
    _viewport->clear_dso_xm();
  } else if (_viewport->action_type() == DSO_YM) {
    _viewport->dso_ym_valid() = false;
    _viewport->set_action(NO_ACTION);
  }

  _viewport->clear_measure();

  // Hide edge navigation buttons when mouse leaves
  _viewport->hover_logic_signal() = nullptr;
  _viewport->prev_edge_btn()->hide();
  _viewport->next_edge_btn()->hide();
}

void ViewportInteraction::keyPressEvent(QKeyEvent *event) {
// Escape clears analog range measurement
if (event->key() == Qt::Key_Escape && _viewport->analog_measure_valid()) {
_viewport->clear_analog_measurement();
return;
}

// Alt+Left / Alt+Right for edge navigation
  if (event->modifiers() & Qt::AltModifier) {
    if (_viewport->hover_logic_signal() &&
        _viewport->view().is_logic_rendering_mode() &&
        _viewport->view().session().is_stopped_status()) {
      if (event->key() == Qt::Key_Left) {
        navigate_to_edge(EdgeNavButton::Previous);
        return;
      } else if (event->key() == Qt::Key_Right) {
        navigate_to_edge(EdgeNavButton::Next);
        return;
      }
    }
  }
  _viewport->forward_keyPressEvent(event);
}

LogicSignal *ViewportInteraction::get_hovered_logic_signal(const QPoint &pos) {
  if (_viewport->type() != TIME_VIEW)
    return nullptr;
  if (!_viewport->view().is_logic_rendering_mode())
    return nullptr;
  if (!_viewport->view().session().is_stopped_status())
    return nullptr;

  int mouseY = pos.y() + _viewport->view().get_vOffset();
  for (auto &s : _viewport->view().get_own_signals()) {
    if (s->signal_type() == SR_CHANNEL_LOGIC && s->enabled()) {
      int sigY = s->get_v_offset();
      int halfH = s->get_totalHeight() / 2 + View::SignalMargin;
      if (abs(mouseY - sigY) < halfH) {
        return (LogicSignal *)s.get();
      }
    }
  }
  return nullptr;
}

void ViewportInteraction::update_edge_nav_buttons() {
  if (_viewport->type() != TIME_VIEW ||
      !_viewport->view().is_logic_rendering_mode() ||
      !_viewport->view().session().is_stopped_status()) {
    _viewport->prev_edge_btn()->hide();
    _viewport->next_edge_btn()->hide();
    _viewport->hover_logic_signal() = nullptr;
    return;
  }

  QPoint screenPos =
      _viewport->mouse_point() - QPoint(0, _viewport->view().get_vOffset());
  LogicSignal *sig = get_hovered_logic_signal(screenPos);
  if (!sig || !sig->data() || sig->data()->empty()) {
    pxv_warn("ViewportInteraction::update_edge_nav_buttons: no signal or empty data, hiding buttons");
    _viewport->prev_edge_btn()->hide();
    _viewport->next_edge_btn()->hide();
    _viewport->hover_logic_signal() = nullptr;
    return;
  }

  _viewport->hover_logic_signal() = sig;

  // Position buttons vertically centered on the signal row
  int sigY = sig->get_v_offset() - _viewport->view().get_vOffset();
  int halfH = sig->get_totalHeight() / 2;
  int btnY = sigY - halfH +
             (sig->get_totalHeight() - _viewport->prev_edge_btn()->height()) / 2;
  const int hOffset = 5;

  _viewport->prev_edge_btn()->move(hOffset, btnY);
  _viewport->next_edge_btn()->move(
      _viewport->width() - _viewport->next_edge_btn()->width() - hOffset, btnY);

  // Check if previous/next edges exist outside the viewport
  auto *snapshot = sig->data();
  int sig_index = sig->get_index();
  uint64_t ring_count = snapshot->get_ring_sample_count();
  if (ring_count == 0) {
    pxv_warn("ViewportInteraction: ring_sample_count==0, skipping edge nav");
    return;
  }
  uint64_t end = ring_count - 1;

  // Get current viewport boundaries in sample indices
  uint64_t leftIndex = _viewport->view().pixel2index(0);
  uint64_t rightIndex = _viewport->view().pixel2index(_viewport->view().get_view_width());

  // Check previous edge: search backward from the left edge of viewport
  bool hasPrev = false;
  if (leftIndex > 0) {
    uint64_t searchIdx = leftIndex;
    bool sample = snapshot->get_sample(searchIdx, sig_index);
    hasPrev = snapshot->get_pre_edge(searchIdx, sample, 1, sig_index);
  }

  // Check next edge: search forward from the right edge of viewport
  bool hasNext = false;
  if (rightIndex < end) {
    uint64_t searchIdx = rightIndex;
    bool sample = snapshot->get_sample(searchIdx, sig_index);
    hasNext = snapshot->get_nxt_edge(searchIdx, sample, end, 1, sig_index);
  }

  _viewport->prev_edge_btn()->setEnabled(hasPrev);
  _viewport->next_edge_btn()->setEnabled(hasNext);
  _viewport->prev_edge_btn()->setVisible(true);
  _viewport->next_edge_btn()->setVisible(true);
}

void ViewportInteraction::navigate_to_edge(EdgeNavButton::Direction dir) {
  if (!_viewport->hover_logic_signal())
    return;

  auto *snapshot = _viewport->hover_logic_signal()->data();
  if (!snapshot || snapshot->empty()) {
    pxv_warn("ViewportInteraction::navigate_to_edge: no snapshot or empty data, skipping");
    return;
  }

  int sig_index = _viewport->hover_logic_signal()->get_index();
  uint64_t ring_count = snapshot->get_ring_sample_count();
  if (ring_count == 0) {
    pxv_warn("ViewportInteraction: ring_sample_count==0, skipping edge nav");
    return;
  }
  uint64_t end = ring_count - 1;

  // Start searching from the viewport edge (consistent with Logic 2:
  // next edge searches from right edge, previous edge searches from left edge)
  uint64_t searchIdx;
  if (dir == EdgeNavButton::Next) {
    searchIdx = _viewport->view().pixel2index(_viewport->view().get_view_width());
  } else {
    searchIdx = _viewport->view().pixel2index(0);
  }

  if (searchIdx > end)
    return;

  bool sample = snapshot->get_sample(searchIdx, sig_index);
  bool found = false;

  if (dir == EdgeNavButton::Next) {
    found = snapshot->get_nxt_edge(searchIdx, sample, end, 1, sig_index);
  } else {
    found = snapshot->get_pre_edge(searchIdx, sample, 1, sig_index);
  }

  if (!found)
    return;

  // Move search cursor to the found edge
  _viewport->view().show_search_cursor(true);
  _viewport->view().get_search_cursor()->set_index(searchIdx);

  // Calculate offset to place the edge at 25% position
  const double time =
      searchIdx * 1.0 / _viewport->view().session().cur_snap_samplerate();
  int viewWidth = _viewport->view().get_view_width();
  double scale = _viewport->view().scale();

  int64_t newOffset;
  if (dir == EdgeNavButton::Next) {
    // Place edge at left 25%
    newOffset = (int64_t)(time / scale - viewWidth * 0.25);
  } else {
    // Place edge at right 25%
    newOffset = (int64_t)(time / scale - viewWidth * 0.75);
  }

  _viewport->view().set_scale_offset(scale, newOffset);
}

} // namespace view
} // namespace pv
