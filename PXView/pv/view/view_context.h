/*
 * view_context.h — ViewContext 值对象，封装坐标映射所需的全部参数
 *
 * ViewContext 是一个轻量值对象（POD + 方法），包含：
 *   samplerate  — 采样率 (Hz)
 *   scale       — 时间/像素比例 (s/px)
 *   offset      — 偏移量 (samples)
 *   trig_hoff   — 触发偏移 (samples)
 *
 * 所有坐标映射公式以 ViewContext 方法形式提供：
 *   index2pixel(idx)         — 公式A: idx/spp - offset (+ trig_hoff/spp)
 *   pixel2index(px)         — 公式C: round((px+offset)*spp - trig_hoff)
 *   measure_pixel2index(px) — 1.5.8 floor: (uint64_t)(sr*scale*(offset+px))
 *   edge_pos(px)            — 浮点: sr*scale*(offset+px)
 *   waveform_col(idx)       — 公式B: floor((idx - offset*spp)/spp)
 *   divergence(idx)         — A vs B 偏差
 *
 * ViewDataSync::index2pixel / pixel2index 委托到 ViewContext。
 * LogicSignal::measure / is_by_edge / edge 使用 ViewContext 构造坐标。
 * Viewport::measure 使用 ViewContext::index2pixel 计算箭头端点。
 *
 * 单元测试直接链接 view_context.cpp，测试原装逻辑，无需 View 实例。
 */

#ifndef PXVIEW_PV_VIEW_VIEW_CONTEXT_H
#define PXVIEW_PV_VIEW_VIEW_CONTEXT_H

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace pv {
namespace view {

// Forward declaration for from_view factory
class View;

struct ViewContext
{
    double   samplerate = 0;
    double   scale      = 0;
    int64_t  offset     = 0;
    double   trig_hoff  = 0;

    // Default constructor — zero-initialized
    ViewContext() = default;

    // Parameterized constructor
    ViewContext(double sr, double sc, int64_t off, double hoff = 0.0)
        : samplerate(sr), scale(sc), offset(off), trig_hoff(hoff) {}

    // Factory: construct from a View instance (reads view->scale/offset/etc)
    // Defined in view_context.cpp (needs View's full definition)
    static ViewContext from_view(View *view, bool has_hoff = true);

    // ---- Coordinate mapping methods ----

    /// 公式A: index2pixel — ruler/cursor/arrow 使用
    /// pixel = idx / (samplerate * scale) - offset
    /// When has_hoff: pixel += trig_hoff / spp
    inline double index2pixel(uint64_t index, bool has_hoff = false) const
    {
        const double spp = samplerate * scale;
        double px = (double)index / spp - (double)offset;
        if (has_hoff)
            px += trig_hoff / spp;
        return px;
    }

    /// 公式C: pixel2index — 游标拖拽使用
    /// idx = round((pixel + offset) * spp - trig_hoff)
    inline uint64_t pixel2index(double pixel) const
    {
        const double spp = samplerate * scale;
        const double idx = (pixel + (double)offset) * spp - trig_hoff;
        if (idx < 0)
            return 0;
        return (uint64_t)std::round(idx);
    }

    /// 1.5.8 measure: 隐式 floor, 无 round
    /// idx = (uint64_t)(samplerate * scale * (offset + pixel))
    inline uint64_t measure_pixel2index(double pixel) const
    {
        const double pos = samplerate * scale * ((double)offset + pixel);
        return (uint64_t)pos;
    }

    /// 1.5.8 edge/is_by_edge: 浮点 pos (不截断)
    inline double edge_pos(double pixel) const
    {
        return samplerate * scale * ((double)offset + pixel);
    }

    /// 公式B: waveform column — RLE get_display_edges 使用
    /// col = floor((idx - offset * spp) / spp)
    inline double waveform_col(uint64_t index) const
    {
        const double spp = samplerate * scale;
        const double float_start = (double)offset * spp;
        return std::floor((double)((int64_t)index - float_start) / spp);
    }

    /// 跳变样本的精确像素位置(公式A 浮点,不截断)。
    /// pixel = idx / (samplerate * scale) - offset (+ trig_hoff/spp)
    /// 用于波形跳变竖线与测量箭头端点,保证两者像素一致。
    inline double edge_pixel(uint64_t index, bool has_hoff = false) const
    {
        return index2pixel(index, has_hoff);
    }

    /// 像素列 x 覆盖的采样范围 [s0, s1)。
    /// 供 get_display_edges 与 paint_mid_align 共用,确保列归属唯一。
    inline void sample_col_range(double x, uint64_t &s0, uint64_t &s1) const
    {
        const double spp = samplerate * scale;
        const double float_start = (double)offset * spp;
        s0 = (uint64_t)(float_start + x * spp);
        s1 = (uint64_t)(float_start + (x + 1) * spp);
        if (s1 <= s0)
            s1 = s0 + 1;
    }

    /// 计算公式A与公式B的偏差量
    inline double divergence(uint64_t index) const
    {
        return waveform_col(index) - index2pixel(index);
    }
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_CONTEXT_H
