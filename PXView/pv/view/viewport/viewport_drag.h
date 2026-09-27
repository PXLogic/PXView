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

#ifndef PXVIEW_PV_VIEW_VIEWPORT_DRAG_H
#define PXVIEW_PV_VIEW_VIEWPORT_DRAG_H

#include <cstdint>
#include <vector>

#include "pv/view/pan_decay.h"

namespace pv {
namespace view {

class Viewport;

// Drag frame delegate extracted from Viewport (Phase F3).
// Holds a non-owning back-pointer to its Viewport and accesses the
// Viewport's private state through friend access. The drag-frame
// coalescing logic (applyDragFrame) and the inertial drag timer
// (on_drag_timer) live here; Viewport keeps only thin slot forwarders
// wired to its QTimer instances.
class ViewportDrag {
public:
  explicit ViewportDrag(Viewport *viewport);
  ~ViewportDrag();

  void applyDragFrame();
  void on_drag_timer();

  // ---- 惯性滚动（拖拽释放后） ----
  // 拖拽中每帧调用 record_drag_sample() 记录 (时间, x)；释放时由交互层调
  // start_pan_decay()：取最近 VelocityWindowMs 窗口的平均速度作为释放速度，
  // 减半后交给 PanDecay 指数衰减，由 on_drag_timer 以 16ms 帧步进推进。
  // 任何新的输入（按下、滚轮）都应先 cancel_pan_decay() 抢占。
  void reset_drag_samples();
  bool start_pan_decay();
  void cancel_pan_decay();
  bool pan_decaying() const { return _pan.active(); }
  // 每次鼠标移动事件记一个速度样本（高频、按原始事件时刻，而非按渲染帧合并）。
  // 释放时交互层再补记一次最终点。
  void record_drag_sample();

private:
  // 最近 VelocityWindowMs 窗口内的平均水平速度（像素/毫秒，带方向）。
  double recent_velocity() const;

  Viewport *_viewport;

  // ---- 速度采样（拖拽中） ----
  struct DragSample {
    int64_t t;  // 相对 Viewport::elapsed_time() 的毫秒
    int x;      // 视口内像素 x
  };
  std::vector<DragSample> _samples;

  // ---- 惯性衰减状态 ----
  PanDecay _pan;
  // 亚像素位移累加器：每帧积分出的 dx 往往是 <1px 的小数，直接 llround 会丢
  // 掉尾部位移并可能让"贴边停止"误判提前触发。把小数累到这里，每帧只提交
  // 整数部分，余量留到下一帧，保证总位移精确、且不误停。
  double _pan_remainder = 0.0;

  // 释放速度采样：取最近 VelocityWindowMs 毫秒、最多 MaxSamples 个原始事件。
  // 窗口 = min(VelocityWindowMs, 样本跨度)。一般鼠标(≥125Hz)下样本跨度远短于窗口
  // 封顶，测到的是接近松手瞬间的瞬时速度（少被松手前减速稀释）→ 甩得远；若改成
  // 更长的固定窗口，会把减速一起平均进去，使释放初速偏低、手感偏重。
  static constexpr int VelocityWindowMs = 100;   // 采样窗口封顶（毫秒）
  static constexpr int MaxSamples = 10;          // 保留的事件样本数上限
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEWPORT_DRAG_H
