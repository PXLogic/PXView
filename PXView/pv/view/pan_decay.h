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

#ifndef PXVIEW_PV_VIEW_PAN_DECAY_H
#define PXVIEW_PV_VIEW_PAN_DECAY_H

#include <cmath>
#include <cstdint>

namespace pv {
namespace view {

/**
 * 拖拽释放后的惯性滚动积分器（纯逻辑，无 Qt 依赖 → 可直接单测）。
 *
 * ── 物理模型 ─────────────────────────────────────────────────────────────
 * 释放时刻取拖拽末段的实时速度 v0（像素/毫秒，带方向），此后速度按指数衰减：
 *
 *     v(t) = v0 * e^(-t / τ)，τ = TimeConstantMs
 *
 * 位移是速度的精确积分（不是"每帧 v*dt 再乘系数"的近似）：
 *
 *     x(t) = v0 * τ * (1 - e^(-t / τ))
 *
 * 每帧由 sample() 给出 [上一帧, 本帧] 区间的精确位移，因此帧率变化（60/120/
 * 144Hz、掉帧）不影响轨迹形状，只影响采样密度。
 *
 * ── 参数取值依据 ─────────────────────────────────────────────────────────
 *   · τ = 100ms：滑动在约 0.3s（3τ）内自然停下，既保留"甩出去"的顺滑感，
 *     又不会滑出太远导致定位困难。
 *   · 停止/结算阈值 StopMinVelocityPxPerMs = 0.01（= 10 像素/秒）：本积分器内部
 *     用，低于该速度的剩余滑行在视觉上不可辨，到阈值即结算全部剩余位移并结束，
 *     避免定时器空转。
 *   · 启动阈值 StartMinVelocityPxPerMs = 0.00001（= 0.01 像素/秒）：仅用于决定
 *     是否启动惯性——过滤"基本静止松手"，不影响正常甩动手感。
 *   · 初速由调用方减半后传入（v0 = 释放速度 / 2）：拖拽中的平均速度总是高于
 *     手感上"应该滑出去"的距离，直接用会滑过头。
 */
class PanDecay {
public:
  /** 速度衰减时间常数（毫秒）。 */
  static constexpr int TimeConstantMs = 100;
  /** 停止/结算阈值（像素/毫秒）。低于它结算剩余位移并结束。 */
  static constexpr double StopMinVelocityPxPerMs = 0.01;
  /** 启动阈值（像素/毫秒）。仅用于决定是否启动惯性（过滤"基本静止松手"）。 */
  static constexpr double StartMinVelocityPxPerMs = 0.00001;

  /**
   * 启动一次惯性滚动。
   * @param v0_px_per_ms 初速（像素/毫秒，带方向；0 直接不启动）。
   * @param now_ms       单调时钟读数（毫秒）。
   */
  void start(double v0_px_per_ms, int64_t now_ms) {
    _v0 = v0_px_per_ms;
    _t0 = now_ms;
    _last_t = 0;
    _active = v0_px_per_ms != 0.0;
  }

  /**
   * 逐帧推进。返回 false 表示本帧已结束（含结算的剩余位移）。
   *
   * 进度用**绝对墙钟**而非逐帧累加：丢帧或事件循环被挂起后恢复，会一次性
   * 结算 [last, now] 的精确位移并按阈值收尾，不会跳变也不会永久滑行。
   *
   * @param dx_out 输出本帧位移（像素，带方向）。
   */
  bool sample(int64_t now_ms, double &dx_out) {
    if (!_active) {
      dx_out = 0.0;
      return false;
    }

    // 相对时间；时钟回退（理论上单调时钟不会发生）时钳到上一帧。
    double t1 = static_cast<double>(now_ms - _t0);
    if (t1 < _last_t)
      t1 = _last_t;

    // [last, t1] 区间的精确位移：v0*τ*(e^(-last/τ) - e^(-t1/τ))。
    const double tau = static_cast<double>(TimeConstantMs);
    const double dx =
        _v0 * tau * (std::exp(-_last_t / tau) - std::exp(-t1 / tau));
    _last_t = t1;

    const double v_now = _v0 * std::exp(-t1 / tau);
    if (std::fabs(v_now) <= StopMinVelocityPxPerMs) {
      // 到阈值：把 [t1, ∞) 的剩余位移一次结算，保证总位移精确等于 v0*τ。
      dx_out = dx + v_now * tau;
      _active = false;
      return false;
    }
    dx_out = dx;
    return true;
  }

  bool active() const { return _active; }
  void cancel() { _active = false; }
  double initial_velocity() const { return _v0; }

  /** 当前瞬时速度（像素/毫秒）。未启动时返回 0。诊断用。 */
  double velocity_at(int64_t now_ms) const {
    if (!_active)
      return 0.0;
    double t = static_cast<double>(now_ms - _t0);
    if (t < 0.0)
      t = 0.0;
    return _v0 * std::exp(-t / static_cast<double>(TimeConstantMs));
  }

private:
  bool _active = false;
  double _v0 = 0.0;      // 像素/毫秒，带方向
  int64_t _t0 = 0;       // 启动时刻（同一单调时钟）
  double _last_t = 0.0;  // 上次积分到的相对时间（毫秒）
};

}  // namespace view
}  // namespace pv

#endif  // PXVIEW_PV_VIEW_PAN_DECAY_H
