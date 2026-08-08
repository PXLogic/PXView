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

#include "pv/view/component/pulsehistogramwidget.h"

#include "pv/config/appconfig.h"
#include "pv/ui/langresource.h"
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QRectF>
#include <algorithm>
#include <cstdint>

namespace pv {
namespace view {

// 自适应柱子数:根据数据 max_width 决定,而不是固定 30。
// clamp 到 [10, 100] 防止极端值(数据太少时至少 10 根,太多时上限 100)。
namespace {
const double kBarGap = 2.0;
const int kPadTop = 20;      // 顶部留给"推荐/当前"标签
const int kPadBottom = 18;   // 底部留给 X 轴标签
const int kPadX = 8;

// 强调色保留固定值(跨主题一致,便于辨识)
const QColor kFilterColor("#ff5252");
const QColor kRecommendLine("#ff9800");
const QColor kCurrentLine("#42a5f5");

// 基础色从主题 token 取(跟随主题切换),缺失时回退深色
QColor tokenColor(const char* name, const char* fallback) {
    QString v = AppConfig::Instance().GetThemeTokenValue(name);
    QColor c(v.isEmpty() ? QString::fromLatin1(fallback) : v);
    return c.isValid() ? c : QColor(QString::fromLatin1(fallback));
}
} // namespace

PulseHistogramWidget::PulseHistogramWidget(QWidget* parent)
    : QWidget(parent), _num_bars(30)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void PulseHistogramWidget::setData(const pv::data::PulseAnalyzer::Histogram& hist)
{
    _hist = hist;
    _has_data = true;
    // _num_bars 由 setNumBars 单独控制(初始 30,调整上限后跟随 cap),
    // setData 不重置,避免覆盖 on_max_changed 设置的新值。
    update();
}

void PulseHistogramWidget::setNumBars(int n)
{
    // 柱子数 = cap(spinbox 值),与滑块上限同步。
    // 初始 30,用户调整上限后跟随。
    _num_bars = std::max(1, n);
    update();
}

void PulseHistogramWidget::setThresholds(uint32_t recommended, uint32_t current)
{
    _recommended_threshold = recommended;
    _current_threshold = current;
    update();
}

void PulseHistogramWidget::setFilterThreshold(uint32_t threshold)
{
    _filter_threshold = threshold;
    update();
}

void PulseHistogramWidget::setRecommendedThreshold(uint32_t recommended)
{
    _recommended_threshold = recommended;
    update();
}

QSize PulseHistogramWidget::sizeHint() const
{
    return QSize(388, 160);
}

QSize PulseHistogramWidget::minimumSizeHint() const
{
    return QSize(200, 120);
}

void PulseHistogramWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 基础色从主题 token 取(每次 paint 时刷新,跟随主题热切换)
    const QColor bgColor = tokenColor("@bg-base", "#1a1d24");
    const QColor borderColor = tokenColor("@border-strong", "#2a2f38");
    const QColor barColor = tokenColor("@toolbtn-hover", "#4a5060");
    const QColor tickColor = tokenColor("@fg-muted", "#9e9e9e");

    const QRect r = rect();
    const QRect barsArea(kPadX, kPadTop,
                         r.width() - 2 * kPadX,
                         r.height() - kPadTop - kPadBottom);

    // 1) 背景 + 边框 (token: @bg-base / @border-strong),圆角 4px
    p.setPen(QPen(borderColor, 1));
    p.setBrush(bgColor);
    p.drawRoundedRect(r.adjusted(0, 0, -1, -1), 4, 4);

    if (!barsArea.isValid() || barsArea.width() <= 0 || barsArea.height() <= 0)
        return;

    const int nBars = _num_bars;

    // 动态 gap:柱子少时 2px 间距,多时缩减为 1px 或 0px,保证 barW >= 1px
    double gap = kBarGap;
    if (nBars > 80) gap = 0;
    else if (nBars > 40) gap = 1;

    // 2) 计算 barW (保证 >= 1px,否则减少 gap)
    double barW = (barsArea.width() - (nBars - 1) * gap) / nBars;
    if (barW < 1.0) {
        gap = 0;
        barW = barsArea.width() / (double)nBars;
        if (barW < 0.5) barW = 0.5;  // 极端情况:细线条
    }

    // 3) 取 maxCount (跨所有宽度 1..nBars)
    int maxCount = 1;
    if (_has_data) {
        for (const auto& kv : _hist.width_counts) {
            if (kv.first >= 1 && kv.first <= (uint32_t)nBars && kv.second > maxCount)
                maxCount = kv.second;
        }
    }

    // 4) 阈值线 X 坐标辅助 — 必须与柱子绘制公式对齐(第 t 个柱子的中心),
    // 否则 nBars 较大时等距插值 (t-1)/(nBars-1)*width 与实际柱位 (t-1)*(barW+gap)
    // 累积偏差会导致红线/标签超前于红柱。
    auto thresholdX = [&](int t) -> double {
        int tt = std::clamp(t, 1, nBars);
        return barsArea.left() + (tt - 1) * (barW + gap) + barW / 2.0;
    };

    // 5) 绘制柱子 (颜色:width <= currentThreshold → 红 #ff5252,否则 #4a5060)
    p.setPen(Qt::NoPen);
    for (int i = 0; i < nBars; ++i) {
        int w = i + 1;
        int count = 0;
        if (_has_data) {
            auto it = _hist.width_counts.find((uint32_t)w);
            if (it != _hist.width_counts.end())
                count = it->second;
        }
        // 对齐 HTML .histogram-bar { min-height: 2px }:
        // count=0 也画 2px 高的小矩形,让用户看到所有宽度位置都有"槽位"
        double h = (count > 0) ? (double)count / maxCount * barsArea.height() : 2.0;
        h = std::max(h, 2.0);  // min-height 2px

        double x = barsArea.left() + i * (barW + gap);
        double y = barsArea.bottom() - h;

        QColor c = (_filter_threshold > 0 && w <= (int)_filter_threshold)
                       ? kFilterColor
                       : barColor;
        p.setBrush(c);
        p.drawRoundedRect(QRectF(x, y, barW, h), 2, 2, Qt::AbsoluteSize);
    }

    // 6) 推荐阈值线 (橙 #ff9800, 2px, 带"推荐"标签)
    QFont lblFont("Segoe UI", 7);
    QFontMetrics fm(lblFont);

    bool has_rec = (_recommended_threshold >= 1 && _recommended_threshold <= (uint32_t)nBars);
    bool has_cur = (_current_threshold >= 1 && _current_threshold <= (uint32_t)nBars);
    double recX = has_rec ? thresholdX((int)_recommended_threshold) : 0;
    double curX = has_cur ? thresholdX((int)_current_threshold) : 0;
    QString recLabel = QString::fromUtf8(L_S(STR_PAGE_SIGNAL_PROC, "IDS_GLITCH_POPUP_RECOMMENDED", "Rec"));
    QString curLabel = QString::fromUtf8(L_S(STR_PAGE_SIGNAL_PROC, "IDS_GLITCH_POPUP_CURRENT", "Cur"));
    int recTextW = fm.horizontalAdvance(recLabel) + 8;
    int curTextW = fm.horizontalAdvance(curLabel) + 8;

    if (has_rec) {
        p.setPen(QPen(kRecommendLine, 2));
        p.drawLine(QPointF(recX, barsArea.top()), QPointF(recX, barsArea.bottom()));

        p.setPen(Qt::NoPen);
        p.setBrush(kRecommendLine);
        p.setFont(lblFont);
        QRectF recTag(recX - recTextW / 2.0, barsArea.top() - 16, recTextW, 12);
        p.drawRoundedRect(recTag, 2, 2);
        p.setPen(QColor("#000"));
        p.drawText(recTag, Qt::AlignCenter, recLabel);
    }

    // 7) 当前阈值线 (蓝 #42a5f5, 2px, 带"当前"标签)
    // 重叠时:当前标签画在与推荐标签同一位置,后画覆盖推荐(用户偏好"当前覆盖推荐")
    if (has_cur) {
        p.setPen(QPen(kCurrentLine, 2));
        p.drawLine(QPointF(curX, barsArea.top()), QPointF(curX, barsArea.bottom()));

        p.setPen(Qt::NoPen);
        p.setBrush(kCurrentLine);
        // 标签始终在顶部 (barsArea.top() - 16),重叠时后画覆盖推荐
        QRectF curTag(curX - curTextW / 2.0, barsArea.top() - 16, curTextW, 12);
        p.drawRoundedRect(curTag, 2, 2);
        p.setPen(QColor("#fff"));
        p.drawText(curTag, Qt::AlignCenter, curLabel);
    }

    // 8) X 轴标签:1 和每 5(或 nBars/4)一标
    p.setPen(tickColor);
    p.setFont(QFont("Segoe UI", 7));
    int step = std::max(1, nBars / 5);
    for (int w = 1; w <= nBars; ++w) {
        if (w == 1 || w % step == 0 || w == nBars) {
            double x = thresholdX(w);
            QRectF tag(x - 10, barsArea.bottom() + 3, 20, 12);
            p.drawText(tag, Qt::AlignCenter, QString::number(w));
        }
    }
}

} // namespace view
} // namespace pv
