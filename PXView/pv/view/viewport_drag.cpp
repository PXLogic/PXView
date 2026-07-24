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
  _viewport->_drag_frame_pending = false;

  if (_viewport->_action_type == RESIZE_SIGNAL) {
    int deltaY =
        _viewport->_drag_last_pos.y() - _viewport->_resize_mouse_down_y;
    int newUpperHeight = _viewport->_resize_upper_height + deltaY;
    if (newUpperHeight >= View::MinSignalHeight &&
        _viewport->_view.is_logic_rendering_mode()) {
      _viewport->_resize_trace_upper->set_own_height(newUpperHeight);
      _viewport->_view.signals_changed(NULL);
    }
    return;
  }

  int mode = _viewport->_view.get_work_mode();

  if (_viewport->_type == TIME_VIEW) {
    if (_viewport->_drag_buttons & Qt::LeftButton) {
      if (_viewport->_action_type == NO_ACTION) {
        int64_t x = _viewport->_mouse_down_offset +
                    (_viewport->_mouse_down_point - _viewport->_drag_last_pos)
                        .x();
        _viewport->_view.set_scale_offset(_viewport->_view.scale(), x);
      }
      _viewport->_drag_strength =
          (_viewport->_mouse_down_point - _viewport->_drag_last_pos).x();
    }
  } else if (_viewport->_type == FFT_VIEW) {
    if (_viewport->_drag_buttons & Qt::LeftButton) {
      for (auto t : _viewport->_view.get_own_spectrum_traces()) {
        if (t->enabled()) {
          double delta = (_viewport->_mouse_point - _viewport->_drag_last_pos).x();
          t->set_offset(delta);
          break;
        }
      }
    }
  }

  if (_viewport->_type == TIME_VIEW) {
    if (_viewport->_action_type == DSO_TRIG_MOVE) {
      if (_viewport->_drag_sig &&
          _viewport->_drag_sig->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)_viewport->_drag_sig;
        dsoSig->set_trig_vpos(_viewport->_drag_last_pos.y());
        _viewport->_dso_trig_moved = true;
      }
    }

    if (_viewport->_action_type == CURS_MOVE) {
      TimeMarker *grabbed_marker =
          _viewport->_view.get_ruler()->get_grabbed_cursor();
      if (grabbed_marker) {
        int curX = _viewport->_drag_last_pos.x();
        uint64_t index0 = 0, index1 = 0, index2 = 0;
        bool logic = false;

        for (auto s : _viewport->_view.get_own_signals()) {
          if (_viewport->_view.is_logic_rendering_mode() && s->signal_type() == SR_CHANNEL_LOGIC) {
            view::LogicSignal *logicSig = (view::LogicSignal *)s;
            if (logicSig->measure(_viewport->_drag_last_pos, index0, index1,
                                  index2)) {
              logic = true;
              break;
            }
          }
          if (mode == DSO && s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal *)s;
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
            view::AnalogSignal *analogSig = (view::AnalogSignal *)s;
            QRect vr = analogSig->get_view_rect();
            curX = min(vr.right(), curX);
            if (curX < vr.left()) {
              curX = vr.left();
            }
            break;
          }
        }

        const double pos = _viewport->_view.pixel2index(curX);
        const double pos_delta = pos - (uint64_t)pos;
        const double curP = _viewport->_view.index2pixel(index0);
        const double curN = _viewport->_view.index2pixel(index1);

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

        if (grabbed_marker == _viewport->_view.get_search_cursor()) {
          _viewport->_view.set_search_pos(grabbed_marker->index(), false);
        }

        _viewport->_view.cursor_moving();
        _viewport->_curs_moved = true;
      } else {
        if (_viewport->_view.xcursors_shown()) {
          auto &xcursor_list = _viewport->_view.get_xcursorList();
          const QRect xrect = _viewport->_view.get_view_rect();

          for (auto xc : xcursor_list) {
            if (xc->grabbed() != XCursor::XCur_None) {
              if (xc->grabbed() == XCursor::XCur_Y) {
                int hover_x = _viewport->_drag_last_pos.x();
                if (hover_x < xrect.left())
                  hover_x = xrect.left();
                if (hover_x > xrect.right())
                  hover_x = xrect.right();
                double rate =
                    (hover_x - xrect.left()) * 1.0 / xrect.width();
                xc->set_value(xc->grabbed(), min(rate, 1.0));
              } else {
                int msy = _viewport->_drag_last_pos.y();
                int body_y = _viewport->_view.get_body_height();
                if (msy > body_y)
                  msy = body_y;
                double rate = (msy - xrect.top()) * 1.0 / xrect.height();
                xc->set_value(xc->grabbed(), max(rate, 0.0));
              }
              _viewport->_xcurs_moved = true;
              break;
            }
          }
        }
      }
    }
  }

  _viewport->_mouse_point =
      _viewport->_drag_last_pos + QPoint(0, _viewport->_view.get_vOffset());
  _viewport->measure();
  _viewport->update(UpdateEventType::UPDATE_EV_MS_MOVE);
}

void ViewportDrag::on_drag_timer() {
  const int64_t offset = _viewport->_view.offset();
  const double scale = _viewport->_view.scale();

  if (_viewport->_view.session().is_stopped_status() &&
      _viewport->_drag_strength != 0 &&
      offset < _viewport->_view.get_max_offset() &&
      offset > _viewport->_view.get_min_offset()) {
    _viewport->_view.set_scale_offset(scale, offset + _viewport->_drag_strength);
    _viewport->_drag_strength /= Viewport::DragDamping;
    if (_viewport->_drag_strength != 0)
      _viewport->_drag_timer.start(Viewport::DragTimerInterval);
  } else if (offset == _viewport->_view.get_max_offset() ||
             offset == _viewport->_view.get_min_offset()) {
    _viewport->_drag_strength = 0;
    _viewport->_drag_timer.stop();
    _viewport->set_action(NO_ACTION);
  } else if (_viewport->_action_type == NO_ACTION) {
    _viewport->_drag_strength = 0;
    _viewport->_drag_timer.stop();
  }
}

} // namespace view
} // namespace pv
