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

#ifndef PXVIEW_PV_VIEW_PAN_ANIMATION_H
#define PXVIEW_PV_VIEW_PAN_ANIMATION_H

#include <cmath>
#include <cstdint>

namespace pv {
namespace view {

/**
 * 水平平移动画状态机（纯逻辑，无 Qt 依赖 → 可直接单测）。
 *
 * ── 用途 ─────────────────────────────────────────────────────────────────
 * 给"程序化跳转"（如跳到上/下一边沿、跳到某个时间）提供一段可见的滑动过渡，
 * 而不是让 offset 瞬变。方向由 from/to 的相对大小自然决定，因此左右两个方向
 * 天然对称，不存在"永远朝同一侧"的问题。
 *
 * ── 过渡曲线 ─────────────────────────────────────────────────────────────
 *   · duration 固定为 DurationMs = 300ms：短到不拖慢连续点击的响应，长到足以
 *     看清"滑过去"的过程。
 *   · 缓动取 ease-out cubic：p = 1 - (1 - t)^3，t = clamp((now - start)/duration)。
 *     起步快、末段减速"滑入"目标，符合"跳到某处并稳稳停住"的观感；线性插值会
 *     显得生硬。
 *   · 进度用**绝对墙钟**而非逐帧累加：丢帧、或事件循环被挂起（模态框、窗口最小化）
 *     后恢复时会直接算出 t>=1，一帧补到终点，不会永久卡在中途。
 *
 * ── 与缩放动画的分工 ─────────────────────────────────────────────────────
 * 只改 offset，不动 scale（缩放动画见 zoom_animation.h）。两者共用同一套
 * "16ms 帧定时器 + 单调时钟"的驱动方式，但由各自的 active 状态独立启停。
 */
class PanAnimation {
public:
  /** 动画时长（毫秒）。定长，与平移距离远近无关。 */
  static constexpr int DurationMs = 300;

  /**
   * 启动或重定目标。动画进行中再次调用即为"重定目标"：起点取调用方传入的
   * 当前实际 offset、起始时刻归零，因此连续点击能平滑接管、不回跳。
   *
   * @param from_offset 动画起点 offset，必须是**当前实际值**（可能是上一次动画
   *                    插值出的中途值）。
   * @param to_offset   目标 offset。
   * @param now_ms      单调时钟读数（毫秒）。
   * @param duration_ms 时长；<=0 时按 1ms 处理（等价立即到达终点）。
   */
  void start(int64_t from_offset, int64_t to_offset, int64_t now_ms,
             int duration_ms = DurationMs) {
    _from = from_offset;
    _to = to_offset;
    _start_ms = now_ms;
    _duration_ms = (duration_ms > 0) ? duration_ms : 1;
    _active = true;
  }

  /**
   * 逐帧求值。返回 false 表示本帧已到达终点（动画结束）。
   *
   * @param offset_out 输出当前帧的 offset。
   */
  bool sample(int64_t now_ms, int64_t &offset_out) {
    if (!_active) {
      offset_out = _to;
      return false;
    }

    double t = static_cast<double>(now_ms - _start_ms) /
               static_cast<double>(_duration_ms);
    if (t >= 1.0) {  // 收尾帧硬写终值，避免插值残差
      offset_out = _to;
      _active = false;
      return false;
    }
    if (t < 0.0)  // 时钟回退保护（QElapsedTimer 理论上是单调的，防御性兜底）
      t = 0.0;

    const double eased = ease_out_cubic(t);
    offset_out = _from + static_cast<int64_t>(
                             std::llround(static_cast<double>(_to - _from) * eased));
    return true;
  }

  bool active() const { return _active; }
  void cancel() { _active = false; }

  int64_t from_offset() const { return _from; }
  int64_t target_offset() const { return _to; }

  /// 缓动后的进度 p ∈ [0,1]。与 sample() 同源同公式；动画结束后仍可调用
  /// （返回 1.0）。用于诊断/单测。
  double progress(int64_t now_ms) const {
    double t = static_cast<double>(now_ms - _start_ms) /
               static_cast<double>(_duration_ms);
    if (t < 0.0)
      t = 0.0;
    if (t > 1.0)
      t = 1.0;
    return ease_out_cubic(t);
  }

  /// ease-out cubic：起步快、末段减速滑入。
  static double ease_out_cubic(double t) {
    const double inv = 1.0 - t;
    return 1.0 - (inv * inv * inv);
  }

private:
  bool _active = false;
  int64_t _from = 0;
  int64_t _to = 0;
  int64_t _start_ms = 0;
  int _duration_ms = DurationMs;
};

}  // namespace view
}  // namespace pv

#endif  // PXVIEW_PV_VIEW_PAN_ANIMATION_H
