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

#include "pulse_analyzer.h"

#include <algorithm>

namespace pv {
namespace data {

std::vector<PulseAnalyzer::Pulse> PulseAnalyzer::find_pulses(
    LogicSnapshot *snap, int sig_index, uint64_t max_samples)
{
    std::vector<Pulse> pulses;

    // 显式空指针与边界检查(Release 下 assert 为空操作)
    if (snap == nullptr)
        return pulses;
    if (max_samples == 0)
        return pulses;
    if (!snap->has_data(sig_index))
        return pulses;

    const uint64_t total = snap->get_sample_count();
    if (total == 0)
        return pulses;

    // 限制扫描范围,防止超大采集卡顿
    const uint64_t scan_end = std::min(total, max_samples);
    if (scan_end == 0)
        return pulses;

    // 读取第 0 个采样作为初始基准电平
    // (get_sample / get_nxt_edge 内部均已处理 _loop_offset,这里使用用户坐标系)
    bool level = snap->get_sample(0, sig_index);
    uint64_t pulse_start = 0;
    uint64_t index = 0;
    uint64_t last_edge = 0;
    bool first_edge = true;

    // 边沿扫描循环:对齐 logicsignal.cpp:508-518 的迭代模式 ——
    // 每次找到边沿后翻转 level,从同一 index 继续搜索下一跳变
    while (true) {
        uint64_t search_idx = index;
        // end 取 scan_end-1(范围内最后一个有效采样),min_length=1.0
        const bool found = snap->get_nxt_edge(search_idx, level,
                                              scan_end - 1, 1.0, sig_index);
        if (!found) {
            // 区间内再无跳变,闭合末段脉冲到 scan_end
            if (pulse_start < scan_end)
                pulses.push_back({pulse_start, scan_end, level});
            break;
        }

        // 防御性前向推进检查:边沿未推进则中止,避免死循环
        if (!first_edge && search_idx <= last_edge) {
            if (pulse_start < scan_end)
                pulses.push_back({pulse_start, scan_end, level});
            break;
        }
        first_edge = false;
        last_edge = search_idx;

        // 闭合当前脉冲 [pulse_start, search_idx),其电平为 level
        pulses.push_back({pulse_start, search_idx, level});
        pulse_start = search_idx;
        level = !level;       // 翻转电平:下一脉冲为新电平
        index = search_idx;   // 从当前边沿继续搜索下一跳变
    }

    return pulses;
}

PulseAnalyzer::Histogram PulseAnalyzer::build_histogram(
    const std::vector<Pulse> &pulses, uint32_t max_width_cap)
{
    Histogram hist;
    // max_width 从实际数据中 ≤ cap 的脉冲计算(非 cap 本身)。
    // cap 用于过滤空闲状态产生的超长脉冲(几千~几万采样点),
    // 避免直方图被拉散、窄毛刺挤在最左边不可见。
    // 默认 cap=30(可由 popup 的上限输入框调整)。
    // 如果数据中最宽的短脉冲只有 18,则 max_width=18,柱子和滑块只到 18。
    hist.max_width = 0;

    for (const Pulse &p : pulses) {
        const uint32_t w = p.width();
        if (w == 0)
            continue;
        if (max_width_cap > 0 && w > max_width_cap)
            continue;  // 超过 cap 的长脉冲(空闲状态等)不计入
        hist.width_counts[w]++;
        if (w > hist.max_width)
            hist.max_width = w;  // 实际最大短脉冲宽度
    }

    return hist;
}

uint32_t PulseAnalyzer::recommend_threshold(const Histogram &hist)
{
    if (hist.width_counts.empty())
        return 3;

    // std::map 按 key 升序遍历,直接收集为有序数组
    std::vector<uint32_t> widths;
    widths.reserve(hist.width_counts.size());
    for (const auto &kv : hist.width_counts)
        widths.push_back(kv.first);

    if (widths.size() == 1)
        return widths[0] + 1;

    // 查找相邻宽度的最大间隙
    uint32_t max_gap = 0;
    uint32_t gap_after = widths[0];
    for (size_t i = 1; i < widths.size(); ++i) {
        // widths 严格升序,widths[i] > widths[i-1],无下溢
        const uint32_t gap = widths[i] - widths[i - 1];
        if (gap > max_gap) {
            max_gap = gap;
            gap_after = widths[i - 1];
        }
    }

    if (max_gap <= 1) {
        // 无明显间隙,取 30% 分位数 + 1
        size_t idx = widths.size() * 3 / 10;
        if (idx >= widths.size())
            idx = widths.size() - 1;
        return widths[idx] + 1;
    }

    return gap_after + 1;
}

std::vector<PulseAnalyzer::Pulse> PulseAnalyzer::preview_filter(
    const std::vector<Pulse> &pulses, uint32_t threshold,
    GlitchFilterMode mode)
{
    std::vector<Pulse> result;

    for (const Pulse &p : pulses) {
        // 仅宽度 <= threshold 的窄脉冲为毛刺候选
        if (p.width() > threshold)
            continue;

        bool match = false;
        switch (mode) {
        case GLITCH_FILTER_BOTH:
            // 不区分基准电平,所有窄脉冲均滤除
            match = true;
            break;
        case GLITCH_FILTER_HIGH:
            // 高基准上的低毛刺:脉冲电平为低(false)
            match = (p.level == false);
            break;
        case GLITCH_FILTER_LOW:
            // 低基准上的高毛刺:脉冲电平为高(true)
            match = (p.level == true);
            break;
        default:
            match = false;
            break;
        }

        if (match)
            result.push_back(p);
    }

    return result;
}

} // namespace data
} // namespace pv
