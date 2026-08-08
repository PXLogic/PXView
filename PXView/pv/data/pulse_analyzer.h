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

#ifndef PXVIEW_PV_DATA_PULSE_ANALYZER_H
#define PXVIEW_PV_DATA_PULSE_ANALYZER_H

#include <cstdint>
#include <map>
#include <vector>

#include "pv/data/snapshot/logicsnapshot.h"

namespace pv {
namespace data {

/*
 * PulseAnalyzer —— Core 层脉冲分析工具类(全静态方法,无状态)。
 *
 * 为毛刺滤波 UX 提供只读的脉冲统计与阈值推荐能力:
 *   1. find_pulses         扫描 LogicSnapshot 重建脉冲序列
 *   2. build_histogram     统计短脉冲宽度分布
 *   3. recommend_threshold 用最大间隙启发式推荐毛刺宽度阈值
 *   4. preview_filter      预览将被毛刺滤波器滤除的脉冲子集
 *
 * 仅依赖 Qt Core / libsigrok 数据层,不依赖 QWidget,可工作于 headless 模式。
 */
class PulseAnalyzer
{
public:
    /*
     * 一段电平稳定的脉冲区间 [start, end),level 为该区间内持续的逻辑电平。
     * width() 返回区间长度(采样点数)。
     */
    struct Pulse {
        uint64_t start;
        uint64_t end;
        bool     level;
        uint32_t width() const { return static_cast<uint32_t>(end - start); }
    };

    /*
     * 短脉冲宽度直方图:
     *   width_counts  宽度 -> 出现次数(仅含 width>0 的脉冲)
    *   max_width     数据中实际最大脉冲宽度(自适应,非固定值)
     */
    struct Histogram {
        std::map<uint32_t, int> width_counts;
        uint32_t                max_width;
    };

    /*
     * 扫描 LogicSnapshot 的前 max_samples 个采样,重建脉冲序列。
     * 复用 LogicSnapshot::get_nxt_edge 进行边沿扫描;扫描范围被显式限制,
     * 防止超大采集卡顿。snap 为空、sig_index 无效或 max_samples==0 时返回空向量。
     */
    static std::vector<Pulse> find_pulses(LogicSnapshot *snap, int sig_index,
                                          uint64_t max_samples = 100000);

    /*
     * 构建宽度直方图:仅统计 width>0 且 width<=max_width 的短脉冲
     * (长脉冲非毛刺候选,予以忽略)。
     */
    static Histogram build_histogram(const std::vector<Pulse> &pulses,
                                     uint32_t max_width_cap = 30);

    /*
     * 最大间隙启发式推荐毛刺宽度阈值:
     *   - 直方图为空             -> 3
     *   - 仅一个宽度             -> widths[0] + 1
     *   - 相邻宽度最大间隙 <= 1  -> 排序后 30% 分位数 + 1
     *   - 否则                   -> 最大间隙左侧宽度 + 1
     */
    static uint32_t recommend_threshold(const Histogram &hist);

    /*
     * 预览滤波:返回在给定 threshold 与 mode 下会被毛刺滤波器滤除的脉冲子集。
     * mode 语义对齐 LogicSnapshot::apply_glitch_filter:
     *   GLITCH_FILTER_BOTH  所有 width<=threshold 的脉冲
     *   GLITCH_FILTER_HIGH  仅 level==false 的低脉冲(高基准上的低毛刺)
     *   GLITCH_FILTER_LOW   仅 level==true  的高脉冲(低基准上的高毛刺)
     * 注意:此处为无状态简化版,不维护 apply_glitch_filter 内部的
     * accepted_level 状态机,直接以 pulse.level 判定。
     */
    static std::vector<Pulse> preview_filter(const std::vector<Pulse> &pulses,
                                             uint32_t threshold,
                                             GlitchFilterMode mode);

private:
    PulseAnalyzer() = delete;  // 全静态方法,禁止实例化
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_PULSE_ANALYZER_H
