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

#ifndef PXVIEW_PV_VIEW_ZOOM_ANIMATION_H
#define PXVIEW_PV_VIEW_ZOOM_ANIMATION_H

#include <cmath>
#include <cstdint>

namespace pv {
namespace view {

/**
 * 滚轮缩放动画状态机（纯逻辑，无 Qt 依赖 → 可直接单测）。
 *
 * ── 为什么需要它 ─────────────────────────────────────────────────────────
 * 物理滚轮给的是**离散 tick**。若每个 tick 都瞬时跳到目标 scale，快速滚动会呈现
 * "跳档"而不是"滑动"。这里的做法是：滚轮只负责**提交目标态**，由渲染侧在
 * 固定时长内逐帧逼近目标 —— 顺滑感来自"目标态 + 定长插值"，不是缓动曲线。
 *
 * ── 落地的具体语义 ───────────────────────────────────────────────────────
 *   · duration 固定为 DurationMs = 100ms，与缩放跨度、滚动速度都无关：
 *     短到不影响连续滚动的响应，长到足以看到"滑动"过程。
 *   · 逐帧线性插值 + clamp：scale = initial + (target - initial) * t，
 *     t = clamp((now - start) / duration, 0, 1)；全程只有一次乘加，
 *     没有 exp/pow/sin/cos —— 不是指数或贝塞尔缓动。
 *   · 动画中再次输入 = 重定目标：initial ← 当前插值值，起始时刻归零重启，
 *     因此连续滚动平滑接管、不回跳（实现见 retarget_compound()）。
 *
 * ── 单位与坐标约定 ───────────────────────────────────────────────────────
 * scale 的单位是"秒/样本"（越大越缩小），与 ViewLayout::_scale 同单位：
 *     samples_per_pixel = samplerate * scale            (rasterize.cpp:93)
 *     sample_at_pixel   = (pixel + offset) * samples_per_pixel
 *                                              (logicsnapshot_edge_scan.cpp:203)
 * 因此"锚点不动"等价于 (offset + fixpoint) * scale 在整段动画中保持恒定。
 */
class ZoomAnimation {
public:
  /** 动画时长（毫秒）。定长，与缩放跨度和滚动速度无关。 */
  static constexpr int DurationMs = 100;

  /**
   * 启动或重定目标。动画进行中再次调用即为"重定目标"：起点取调用方传入的
   * 当前实际 scale、起始时刻归零，因此不会回跳。
   *
   * @param initial_scale 动画起点 scale。必须是**当前实际值**（可能是上一次动画
   *                      插值出的中途值），这样连续滚动才能平滑接管而不回跳。
   * @param target_scale  目标 scale。
   * @param fixpoint_px   锚点像素（鼠标 x）；整段动画中该像素处的时间不变。
   * @param anchor_offset 动画起点的 offset，与 initial_scale 配套。
   * @param now_ms        单调时钟读数（毫秒）。
   * @param duration_ms   时长；<=0 时按 1ms 处理（等价立即到达终点）。
   */
  void retarget(double initial_scale, double target_scale, double fixpoint_px,
                int64_t anchor_offset, int64_t now_ms, int duration_ms) {
    _initial_scale = initial_scale;
    _target_scale = target_scale;
    _fixpoint_px = fixpoint_px;
    _anchor_offset = anchor_offset;
    _start_ms = now_ms;
    _duration_ms = (duration_ms > 0) ? duration_ms : 1;
    _active = true;
  }

  /**
   * 逐帧求值。返回 false 表示本帧已到达终点（动画结束）。
   *
   * 进度用**绝对墙钟**而非逐帧累加：丢帧、或事件循环被挂起（模态框、窗口最小化）
   * 后恢复时会直接算出 t>=1，一帧补到终点，不会永久卡在中途。
   *
   * @param scale_out 输出当前帧的 scale。
   */
  bool sample(int64_t now_ms, double &scale_out) {
    if (!_active)
      return false;

    double t = static_cast<double>(now_ms - _start_ms) /
               static_cast<double>(_duration_ms);
    if (t >= 1.0) {  // 收尾帧硬写终值，避免插值残差
      scale_out = _target_scale;
      _active = false;
      return false;
    }
    if (t < 0.0)  // 时钟回退保护（QElapsedTimer 理论上是单调的，防御性兜底）
      t = 0.0;

    scale_out = _initial_scale + (_target_scale - _initial_scale) * t;
    return true;
  }

  /**
   * 复合重定目标 —— 连续滚轮的正确语义：在动画未走完时再次滚动，新目标以
   * **上一次的目标**为基准继续乘 zoom_factor，而不是以当前显示值为基准。
   *
   * 与 retarget() 的唯一区别是**目标的计算基准**：
   *   retarget()          ：新目标由调用方给出（通常是"当前显示值 × 因子"）
   *   retarget_compound() ：新目标 = **上一次的目标** × zoom_factor
   *
   * 为什么必须复合到旧目标：
   *   若每次 retarget 都以"当前显示值"为基准，就会丢弃上一格**尚未走完的那段
   *   行程**。实测（29 个手势 / 104 格滚轮）在快速连滚时缩放量只交付 26%：
   *   第 1 格目标 354ms，第 2 格到来时显示值才走到 475ms，新目标被算成
   *   475/1.5=316ms —— 第 1 格剩下的 475→354 被整段扔掉。
   *   复合到旧目标则累加量与滚动速度无关：N 格恒为 zoom_factor^N。
   *
   * initial 仍取**当前显示值**（可能是上一次插值的中途值），保证运动平滑不回跳。
   *
   * @param initial_scale 当前实际 scale（动画起点）。
   * @param zoom_factor   本次滚轮对应的缩放因子（<1 = 放大，与 target 同量纲）。
   * @param min_scale/max_scale 目标的有效范围（ViewLayout 的 _minscale/_maxscale）。
   */
  void retarget_compound(double initial_scale, double fixpoint_px,
                         int64_t anchor_offset, double zoom_factor,
                         double min_scale, double max_scale, int64_t now_ms,
                         int duration_ms) {
    // 动画在跑 → 复合到未到达的旧目标；未跑（本手势第一格）→ 从当前值起算。
    const double base = _active ? _target_scale : initial_scale;
    double target = base * zoom_factor;
    if (target < min_scale) target = min_scale;
    if (target > max_scale) target = max_scale;
    retarget(initial_scale, target, fixpoint_px, anchor_offset, now_ms,
             duration_ms);
  }

  bool active() const { return _active; }
  void cancel() { _active = false; }

  /// 线性进度 t = clamp((now - start)/duration, 0, 1)。用于诊断日志，
  /// 与 sample() 同源同公式；sample() 结束后仍可调用（返回 1.0）。
  double progress(int64_t now_ms) const {
    double t = static_cast<double>(now_ms - _start_ms) /
               static_cast<double>(_duration_ms);
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  }

  double initial_scale() const { return _initial_scale; }
  double target_scale() const { return _target_scale; }
  double fixpoint_px() const { return _fixpoint_px; }
  int64_t anchor_offset() const { return _anchor_offset; }

  /**
   * 给定当前 scale，算出保持锚点不动的 offset。
   *
   *   offset = floor((anchor_offset + fixpoint) * (initial_scale / scale) - fixpoint)
   *
   * 与 ViewLayout::zoom() 的公式同构（view_layout.cpp 的 zoom() 收尾），只是把
   * `_preScale/_preOffset` 换成**手势参照系**（retarget 时的 initial_scale /
   * anchor_offset）。
   *
   * ⚠ 必须始终以这个**固定参照系**计算，不能逐帧拿上一帧的结果递推：`floor`
   * 每帧都会向下取整，误差同向累积（4 帧即可漂移超过 1 像素，表现为缩放时
   * 鼠标下的波形跟着滑动）。以固定参照系计算时总漂移恒 < 1 像素。
   *
   * 推导：令 I = (offset + fixpoint) * scale（锚点处的时间）。代入上式得
   *   I = (anchor_offset + fixpoint) * initial_scale - frac * scale,  frac∈[0,1)
   * 即 I 与 scale 无关（误差仅一个 floor 步长 < 1 像素）。
   */
  int64_t offset_for(double scale) const {
    if (scale <= 0.0)
      return _anchor_offset;
    const double v =
        (_anchor_offset + _fixpoint_px) * (_initial_scale / scale) - _fixpoint_px;
    // floor 而非截断：负数区间（offset 可为负）也必须向下取整，保持同向误差。
    return static_cast<int64_t>(std::floor(v));
  }

  /**
   * 触边再锚定。当 offset_for() 的结果被 ViewLayout 的有效范围夹住时（已经顶到
   * 数据的最大/最小 offset），把参照系重设到夹取后的实际值 —— 否则后续帧仍按
   * 未夹取的旧参照系计算，会与夹取结果"较劲"产生回弹。
   */
  void reanchor(double scale, int64_t offset) {
    _initial_scale = scale;
    _anchor_offset = offset;
  }

private:
  bool _active = false;
  double _initial_scale = 1.0;
  double _target_scale = 1.0;
  double _fixpoint_px = 0.0;
  int64_t _anchor_offset = 0;
  int64_t _start_ms = 0;
  int _duration_ms = DurationMs;
};

}  // namespace view
}  // namespace pv

#endif  // PXVIEW_PV_VIEW_ZOOM_ANIMATION_H
