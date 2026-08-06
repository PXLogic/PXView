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

#include "viewport_drag.h"
#include "viewport.h"
#include "ruler.h"

#include "../sigsession.h"
#include "analogsignal.h"
#include "dsosignal.h"
#include "logicsignal.h"
#include "signal.h"
#include "spectrumtrace.h"
#include "timemarker.h"
#include "xcursor.h"

#include <QPoint>
#include <algorithm>
#include <cmath>

using std::max;
using std::min;

namespace pv {
namespace view {

ViewportDrag::ViewportDrag(Viewport *viewport) : _viewport(viewport) {}

ViewportDrag::~ViewportDrag() {}

// ActionType / MeasureType enumerators are at pv::view namespace scope (see
// viewport.h). Bring them here so the legacy code ported from viewport.cpp
// can keep using bare names like Viewport::RESIZE_SIGNAL.

void ViewportDrag::applyDragFrame() {
  _viewport->drag_frame_pending() = false;

  if (_viewport->action_type() == RESIZE_SIGNAL) {
    int deltaY =
        _viewport->drag_last_pos().y() - _viewport->resize_mouse_down_y();
    int newUpperHeight = _viewport->resize_upper_height() + deltaY;
    if (newUpperHeight >= View::MinSignalHeight &&
        _viewport->view().is_logic_rendering_mode()) {
      _viewport->resize_trace_upper()->set_own_height(newUpperHeight);
      _viewport->view().signals_changed(nullptr);
    }
    return;
  }

  int mode = _viewport->view().get_work_mode();

  if (_viewport->type() == TIME_VIEW) {
    if (_viewport->drag_buttons() & Qt::LeftButton) {
      if (_viewport->action_type() == NO_ACTION) {
        int64_t x = _viewport->mouse_down_offset() +
                    (_viewport->mouse_down_point() - _viewport->drag_last_pos())
                        .x();
        _viewport->view().set_scale_offset(_viewport->view().scale(), x);
      }
      _viewport->drag_strength() =
          (_viewport->mouse_down_point() - _viewport->drag_last_pos()).x();
    }
  } else if (_viewport->type() == FFT_VIEW) {
    if (_viewport->drag_buttons() & Qt::LeftButton) {
      for (auto &t : _viewport->view().get_own_spectrum_traces()) {
        if (t->enabled()) {
          double delta = (_viewport->mouse_point() - _viewport->drag_last_pos()).x();
          t->set_offset(delta);
          break;
        }
      }
    }
  }

  if (_viewport->type() == TIME_VIEW) {
    if (_viewport->action_type() == DSO_TRIG_MOVE) {
      if (_viewport->drag_sig() &&
          _viewport->drag_sig()->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)_viewport->drag_sig();
        dsoSig->set_trig_vpos(_viewport->drag_last_pos().y());
        _viewport->dso_trig_moved() = true;
      }
    }

    if (_viewport->action_type() == CURS_MOVE) {
      TimeMarker *grabbed_marker =
          _viewport->view().get_ruler()->get_grabbed_cursor();
      if (grabbed_marker) {
        int curX = _viewport->drag_last_pos().x();
        uint64_t index0 = 0, index1 = 0, index2 = 0;
        bool logic = false;

        for (auto &s : _viewport->view().get_own_signals()) {
          if (_viewport->view().is_logic_rendering_mode() && s->signal_type() == SR_CHANNEL_LOGIC) {
            view::LogicSignal *logicSig = (view::LogicSignal *)s.get();
            if (logicSig->measure(_viewport->drag_last_pos(), index0, index1,
                                  index2)) {
              logic = true;
              break;
            }
          }
          if (mode == DSO && s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
            curX = min(dsoSig->get_view_rect().right(), curX);
            if (curX < dsoSig->get_view_rect().left()) {
              curX = dsoSig->get_view_rect().left();
            }
            break;
          }
          /* ANALOG mode: clamp curX to the viewport bounds so cursors
           * can't be dragged outside the signal area. The original
           * DSView had no branch for ANALOG here, leaving curX
           * unclamped — a known bug. */
          if (mode == ANALOG && s->signal_type() == SR_CHANNEL_ANALOG) {
            view::AnalogSignal *analogSig = (view::AnalogSignal *)s.get();
            QRect vr = analogSig->get_view_rect();
            curX = min(vr.right(), curX);
            if (curX < vr.left()) {
              curX = vr.left();
            }
            break;
          }
        }

        const double pos = _viewport->view().pixel2index(curX);
        const double pos_delta = pos - (uint64_t)pos;
        const double curP = _viewport->view().index2pixel(index0);
        const double curN = _viewport->view().index2pixel(index1);

        if (logic &&
            (curX - curP < Viewport::SnapMinSpace ||
             curN - curX < Viewport::SnapMinSpace)) {
          if (curX - curP < curN - curX)
            grabbed_marker->set_index(index0);
          else
            grabbed_marker->set_index(index1);
        } else if (pos_delta < 0.5) {
          grabbed_marker->set_index((uint64_t)floor(pos));
        } else {
          grabbed_marker->set_index((uint64_t)ceil(pos));
        }

        if (grabbed_marker == _viewport->view().get_search_cursor()) {
          _viewport->view().set_search_pos(grabbed_marker->index(), false);
        }

        _viewport->view().cursor_moving();
        _viewport->curs_moved() = true;
      } else {
        if (_viewport->view().xcursors_shown()) {
          auto &xcursor_list = _viewport->view().get_xcursorList();
          const QRect xrect = _viewport->view().get_view_rect();

          for (auto &xc : xcursor_list) {
            if (xc->grabbed() != XCursor::XCur_None) {
              if (xc->grabbed() == XCursor::XCur_Y) {
                int hover_x = _viewport->drag_last_pos().x();
                if (hover_x < xrect.left())
                  hover_x = xrect.left();
                if (hover_x > xrect.right())
                  hover_x = xrect.right();
                double rate =
                    (hover_x - xrect.left()) * 1.0 / xrect.width();
                xc->set_value(xc->grabbed(), min(rate, 1.0));
              } else {
                int msy = _viewport->drag_last_pos().y();
                int body_y = _viewport->view().get_body_height();
                if (msy > body_y)
                  msy = body_y;
                double rate = (msy - xrect.top()) * 1.0 / xrect.height();
                xc->set_value(xc->grabbed(), max(rate, 0.0));
              }
              _viewport->xcurs_moved() = true;
              break;
            }
          }
        }
      }
    }
  }

  _viewport->mouse_point() =
      _viewport->drag_last_pos() + QPoint(0, _viewport->view().get_vOffset());
  _viewport->measure();
  _viewport->update(UpdateEventType::UPDATE_EV_MS_MOVE);
}

void ViewportDrag::on_drag_timer() {
  const int64_t offset = _viewport->view().offset();
  const double scale = _viewport->view().scale();

  if (_viewport->view().session().is_stopped_status() &&
      _viewport->drag_strength() != 0 &&
      offset < _viewport->view().get_max_offset() &&
      offset > _viewport->view().get_min_offset()) {
    _viewport->view().set_scale_offset(scale, offset + _viewport->drag_strength());
    _viewport->drag_strength() /= Viewport::DragDamping;
    if (_viewport->drag_strength() != 0)
      _viewport->drag_timer().start(Viewport::DragTimerInterval);
  } else if (offset == _viewport->view().get_max_offset() ||
             offset == _viewport->view().get_min_offset()) {
    _viewport->drag_strength() = 0;
    _viewport->drag_timer().stop();
    _viewport->set_action(NO_ACTION);
  } else if (_viewport->action_type() == NO_ACTION) {
    _viewport->drag_strength() = 0;
    _viewport->drag_timer().stop();
  }
}

} // namespace view
} // namespace pv
