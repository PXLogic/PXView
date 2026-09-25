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

#ifndef PXVIEW_PV_VIEW_VIEWPORT_INTERACTION_H
#define PXVIEW_PV_VIEW_VIEWPORT_INTERACTION_H

#include <QNativeGestureEvent>
#include <QPoint>

#include "pv/view/component/edge_nav_button.h"

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace pv {
namespace view {

class LogicSignal;
class Viewport;

// Interaction delegate extracted from Viewport (Phase F2).
// Holds a non-owning back-pointer to its Viewport and accesses the
// Viewport's private state through friend access. All Qt mouse/wheel/
// key/gesture event handling and the three mode-specific release
// handlers live here; Viewport keeps only thin Qt override forwarders.
class ViewportInteraction {
public:
  explicit ViewportInteraction(Viewport *viewport);
  ~ViewportInteraction();

  void mousePressEvent(QMouseEvent *event);
  void mouseMoveEvent(QMouseEvent *event);
  void mouseReleaseEvent(QMouseEvent *event);
  void mouseDoubleClickEvent(QMouseEvent *event);
  void wheelEvent(QWheelEvent *event);
  void keyPressEvent(QKeyEvent *event);
  bool gestureEvent(QNativeGestureEvent *event);
  void leaveEvent(QEvent *event);

  void update_edge_nav_buttons();
  void navigate_to_edge(EdgeNavButton::Direction dir);

private:
  void onLogicMouseRelease(QMouseEvent *event);
  void onDsoMouseRelease(QMouseEvent *event);
  void onAnalogMouseRelease(QMouseEvent *event);
  LogicSignal *get_hovered_logic_signal(const QPoint &pos);

  Viewport *_viewport;
  // 滚轮缩放垂直同步合帧: 累积两次应用之间到达的全部 wheel delta,
  // 由 apply_pending_wheel_zoom() 在下一轮事件循环(即下一帧渲染前)
  // 一次性应用。旧实现按 16ms 墙钟丢弃 tick: 高分辨率滚轮/触控板的
  // delta 被丢掉 → 快滚终点缩放量不足 → 光标锚点错位; 且 16ms 与
  // 120/144Hz 屏的真实帧周期不匹配。累积方案不丢任何 delta, 节奏由
  // 事件循环+重绘自然钳制在每帧一次。仅 GUI 线程访问, 无需原子。
  double _pending_zoom_steps = 0.0;
  int _pending_zoom_x = 0;
  bool _pending_zoom_scheduled = false;

  // 应用累积的缩放, 并执行 wheel 尾部工作 (DSO auto_end + measure)。
  void apply_pending_wheel_zoom();
  // wheel 尾部工作, 由 FFT 即时路径与合帧路径共用。
  void post_wheel_update();
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEWPORT_INTERACTION_H
