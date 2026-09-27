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

#include "pv/view/viewport/viewport_drag.h"
#include "pv/view/viewport/viewport.h"
#include "pv/view/component/ruler.h"

#include "pv/session/sigsession.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/signal.h"
#include "pv/view/trace/spectrumtrace.h"
#include "pv/view/cursor/timemarker.h"
#include "pv/view/cursor/xcursor.h"

#include <QPoint>
#include <algorithm>
#include <cmath>

using std::max;
using std::min;

// 惯性"轻重"旋钮：甩出距离 = v0 * τ（τ = PanDecay::TimeConstantMs）。
// 该系数作用于释放初速 v0（默认减半）：拖拽跟踪速度通常高于手感上"应该滑出去"
// 的距离，直接用会滑过头。
//   0.5 = 减半（默认）；
//   1.0 = 不减半 = 滑行距离翻倍；
//   0.6~0.8 = 介于两者之间。
constexpr double kPanDecayVelocityScale = 0.5;

namespace pv {
namespace view {

ViewportDrag::ViewportDrag(Viewport *viewport) : _viewport(viewport) {}

ViewportDrag::~ViewportDrag() {}

// ActionType / MeasureType enumerators are at pv::view namespace scope (see
// viewport.h). Bring them here so the legacy code ported from viewport.cpp
// can keep using bare names like Viewport::DSO_TRIG_MOVE.
// 注意:通道高度拉伸(RESIZE_SIGNAL)已收归 Header 独占,Viewport 不再参与。

void ViewportDrag::applyDragFrame() {
  _viewport->drag_frame_pending() = false;

  int mode = _viewport->view().get_work_mode();

  if (_viewport->type() == TIME_VIEW) {
    if (_viewport->drag_buttons() & Qt::LeftButton) {
      if (_viewport->action_type() == NO_ACTION) {
        int64_t x = _viewport->mouse_down_offset() +
                    (_viewport->mouse_down_point() - _viewport->drag_last_pos())
                        .x();
        _viewport->view().set_scale_offset(_viewport->view().scale(), x);
      }
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
        const double pos_delta = pos - static_cast<uint64_t>(pos);
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
          grabbed_marker->set_index(static_cast<uint64_t>(floor(pos)));
        } else {
          grabbed_marker->set_index(static_cast<uint64_t>(ceil(pos)));
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

void ViewportDrag::reset_drag_samples() {
  _samples.clear();
  _pan.cancel();
  _pan_remainder = 0.0;
}

void ViewportDrag::record_drag_sample() {
  DragSample s;
  s.t = _viewport->elapsed_time().elapsed();
  s.x = _viewport->drag_last_pos().x();
  _samples.push_back(s);
  // 只保留最近 MaxSamples 帧；鼠标静止时重复帧也保留 —— 它们让窗口内的
  // 平均速度自然趋向 0，"按住不动再松手"就不会产生惯性。
  if (_samples.size() > static_cast<size_t>(MaxSamples))
    _samples.erase(_samples.begin());
}

double ViewportDrag::recent_velocity() const {
  if (_samples.size() < 2)
    return 0.0;
  const DragSample &newest = _samples.back();
  // 只看最近 VelocityWindowMs 窗口：拖拽早期的慢速移动不应稀释释放速度。
  const int64_t t_min = newest.t - VelocityWindowMs;
  size_t first = 0;
  while (first + 1 < _samples.size() && _samples[first].t < t_min)
    first++;
  const DragSample &oldest = _samples[first];
  const double dt = static_cast<double>(newest.t - oldest.t);
  if (dt < 1.0)
    return 0.0;
  return static_cast<double>(newest.x - oldest.x) / dt;
}

bool ViewportDrag::start_pan_decay() {
  const double v = recent_velocity();
  // 给初速乘缩放系数控制"滑多远"（见 kPanDecayVelocityScale）。
  // 注意符号：拖拽时 offset = mouse_down_offset + (mouse_down_point.x -
  // drag_last_pos.x)，即 offset 随鼠标右移而减小。释放速度 v 是"鼠标空间"
  // 速度（右移为正），惯性必须沿同方向继续，换算到"offset 空间"要取反：
  // offset 速度 = -v。漏掉这个负号会让视图朝反方向弹回（回弹）。
  const double v0 = -v * kPanDecayVelocityScale;
  if (std::fabs(v0) <= PanDecay::StartMinVelocityPxPerMs)
    return false;
  _pan.start(v0, _viewport->elapsed_time().elapsed());
  // 启动衰减步进定时器：on_drag_timer 每帧推进后自行续期（singleShot）。
  // 注意：_drag_timer 仅由自身续期启动，若这里不首次 start，整段惯性滚动
  // 永远不会被驱动——这正是"小甩看不出来滑动"的根因（定时器从未启动）。
  _viewport->drag_timer().start(Viewport::PanFrameMs);
  return true;
}

void ViewportDrag::cancel_pan_decay() {
  _pan.cancel();
  _viewport->drag_timer().stop();
}

void ViewportDrag::on_drag_timer() {
  if (!_pan.active()) {
    _viewport->drag_timer().stop();
    _viewport->set_action(NO_ACTION);
    return;
  }

  // 采集进行中禁止惯性滚动（数据在持续增长，滚动基准每帧都在变）。
  if (!_viewport->view().session().is_stopped_status()) {
    _pan.cancel();
    _viewport->drag_timer().stop();
    _viewport->set_action(NO_ACTION);
    return;
  }

  const int64_t now = _viewport->elapsed_time().elapsed();
  double dx = 0.0;
  const bool more = _pan.sample(now, dx);

  // 亚像素累加：把本帧精确位移并入余数，只提交整数部分，避免尾部位移被
  // llround 丢弃，也避免"每帧移动 <1px"被下面的贴边判定误判为已停。
  _pan_remainder += dx;
  const int64_t step = static_cast<int64_t>(llround(_pan_remainder));
  _pan_remainder -= static_cast<double>(step);

  // 位移落地；set_scale_offset 内部会把 offset 夹到有效范围（顶到数据边界时
  // new_offset 与 offset 相等，此时 step 恒为 0、余数不再增长 → 真正停住）。
  const int64_t offset = _viewport->view().offset();
  const int64_t new_offset = offset + step;
  _viewport->view().set_scale_offset(_viewport->view().scale(), new_offset);

  // 速度衰减到阈值以下（PanDecay 已结算剩余位移）或顶到数据边界 → 结束。
  if (!more) {
    _pan.cancel();
    _viewport->drag_timer().stop();
    _viewport->set_action(NO_ACTION);
    return;
  }

  // singleShot 自续：节奏贴合实际事件循环/帧产出率，而不是固定 10Hz。
  _viewport->drag_timer().start(Viewport::PanFrameMs);
}

} // namespace view
} // namespace pv
