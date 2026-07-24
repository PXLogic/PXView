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

#ifndef PULSEHISTOGRAMWIDGET_H
#define PULSEHISTOGRAMWIDGET_H

#include <QWidget>
#include <cstdint>
#include <map>

#include "../data/pulse_analyzer.h"

namespace pv {
namespace view {

/*
 * PulseHistogramWidget —— 脉冲宽度直方图控件(View 层,纯 QWidget)。
 *
 * 仅依赖 pv::data::PulseAnalyzer::Histogram 数据结构与 Qt Widgets,不依赖
 * 其他 View 层类型。绘制柱状图、推荐阈值线、当前阈值线与 x 轴刻度,
 * 用于毛刺滤波 UX 弹窗中可视化短脉冲宽度分布。
 */
class PulseHistogramWidget : public QWidget {
    Q_OBJECT

public:
    explicit PulseHistogramWidget(QWidget* parent = nullptr);

    void setData(const pv::data::PulseAnalyzer::Histogram& hist);
    void setNumBars(int n);  // 设置柱子数(= cap),与滑块上限同步
    void setThresholds(uint32_t recommended, uint32_t current);
    void setFilterThreshold(uint32_t threshold);
    void setRecommendedThreshold(uint32_t recommended);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    pv::data::PulseAnalyzer::Histogram _hist;
    uint32_t _recommended_threshold = 0;
    uint32_t _current_threshold = 0;
    uint32_t _filter_threshold = 0;
    bool _has_data = false;
    int _num_bars;  // 自适应柱子数,由 setData 根据 hist.max_width 设置
};

} // namespace view
} // namespace pv

#endif // PULSEHISTOGRAMWIDGET_H
