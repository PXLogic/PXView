/*
 * ruler_paint.cpp — widget-free ruler tick-mark painting implementation
 * (QML migration Phase 3, Task 3.3).
 *
 * Bodies are verbatim copies of Ruler::draw_logic_tick_mark /
 * Ruler::draw_osc_tick_mark (ruler.cpp) with the dependency substitutions
 * listed in ruler_paint.h. Ruler-local constants consumed by the bodies
 * (MinPeriodScale, SIPrefixes count) are replicated as local constexpr
 * copies — same pattern as QmlRenderView's kMaxViewRate local copy (View
 * statics stay GUI-bound; values must match ruler.h).
 */
#include "pv/view/component/ruler_paint.h"

#include <limits.h>
#include <cmath>

#include <QPainter>

#include "pv/view/component/ruler_format.h"
#include "pv/view/cursor/cursor.h"
#include "pv/view/iview_delegates.h"
#include "pv/data/datasource.h"
#include "pv/session/deviceagent.h"

namespace pv {
namespace view {

namespace {

// Ruler::MinPeriodScale 本地常量副本（ruler.h 静态成员位于 GUI 归档；
// 值必须与 ruler.cpp 保持一致）。
constexpr int kMinPeriodScale = 10;
// Ruler::SIPrefixes 表长度本地副本（表本体在 ruler_format.cpp）。
constexpr int kSIPrefixCount = 9;

} // namespace

void paint_logic_tick_mark(QPainter &p, IRenderView &view, const QRect &rect,
    const QColor &fore, double &min_period, unsigned int &prefix)
{
    auto *ds = view.data_source();
    if (!ds || !ds->device() || ds->device()->have_instance() == false) {
        return;
    }

    double scale = view.scale();
    if (scale <= 0) {
        return;
    }

    data::DataSource *snapshot = view.document_snapshot_source();
    if (!snapshot) {
        return;
    }
    uint64_t samplerate = snapshot->cur_snap_samplerate();
    if (samplerate == 0) {
        return;
    }

    double view_width = view.get_view_width();
    if (view_width <= 0) {
        return;
    }
    double scale_width = scale * view_width;
    if (scale_width <= 0) {
        return;
    }

    const double SpacingIncrement = 32.0;
    const double MinValueSpacing = 16.0;
    const int ValueMargin = 5;
    const double abs_min_period = 10.0 / samplerate;

    double min_width = SpacingIncrement;
    double typical_width;
    double tick_period = 0;
    int64_t offset = view.offset();

    const uint64_t cur_period_scale = ceil((scale * min_width) / abs_min_period);

    min_period = cur_period_scale * abs_min_period;

    const int order = (int)floorf(log10f(scale_width));
    int prefix_val = (order - kFirstSIPrefixPower) / 3;
    if (prefix_val < 0) prefix_val = 0;
    if (prefix_val >= kSIPrefixCount) prefix_val = kSIPrefixCount - 1;
    prefix = prefix_val;
    typical_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
        Qt::AlignLeft | Qt::AlignTop, format_time(offset * scale,
        prefix)).width() + MinValueSpacing;

    int tick_period_loop_count = 0;
    do
    {
        tick_period += min_period;
        if (++tick_period_loop_count > 1000) {
            break;
        }
    } while(typical_width > tick_period / scale);

    if (tick_period <= 0) {
        return;
    }

    const int text_height = p.boundingRect(0, 0, INT_MAX, INT_MAX,
        Qt::AlignLeft | Qt::AlignTop, "8").height();

    QColor fore_color = fore;
    fore_color.setAlpha(IRenderView::ForeAlpha);
    p.setPen(fore_color);

    const double minor_tick_period = tick_period / kMinPeriodScale;
    const int minor_order = (int)floorf(log10f(minor_tick_period));
    int minor_prefix_val = (minor_order - kFirstSIPrefixPower) / 3;
    if (minor_prefix_val < 0) minor_prefix_val = 0;
    if (minor_prefix_val >= kSIPrefixCount) minor_prefix_val = kSIPrefixCount - 1;
    const unsigned int minor_prefix = minor_prefix_val;

    const double first_major_division =
        floor(offset * scale / tick_period);
    const double first_minor_division =
        floor(offset * scale / minor_tick_period + 1);
    const double t0 = first_major_division * tick_period;

    int division = (int)round(first_minor_division -
        first_major_division * kMinPeriodScale) - 1;

    const int major_tick_y1 = text_height + ValueMargin * 3;
    const int tick_y2 = rect.height();
    const int minor_tick_y1 = (major_tick_y1 + tick_y2) / 2;

    int x = rect.left() - 1;

    const double inc_text_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                                 Qt::AlignLeft | Qt::AlignTop,
                                                 format_time(minor_tick_period,
                                                             minor_prefix)).width() + MinValueSpacing;
    int loop_count = 0;
    while (true) {
        const double t = t0 + division * minor_tick_period;
        const double major_t = t0 + floor(division / kMinPeriodScale) * tick_period;

        double x_double = t / scale - offset;
        if (x_double > rect.right()) {
            break;
        }

        if (++loop_count > 2000) {
            break;
        }

        if (x_double < -1e6 || x_double > 1e6) {
            division++;
            continue;
        }

        x = (int)x_double;

        if (division % kMinPeriodScale == 0)
        {
            // Draw a major tick
            p.drawText(x, 2 * ValueMargin, 0, text_height,
                Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
                format_time(t, prefix));
            p.drawLine(QPoint(x, major_tick_y1),
                QPoint(x, tick_y2));
        }
        else
        {
            // Draw a minor tick
            if (minor_tick_period / scale > 2 * typical_width)
                p.drawText(x, 2 * ValueMargin, 0, text_height,
                    Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
                    format_time(t, prefix));
            //else if ((tick_period / scale > width() / 4) && (minor_tick_period / scale > inc_text_width))
            else if (minor_tick_period / scale > 1.1 * inc_text_width ||
                     tick_period / scale > view.get_view_width())
                p.drawText(x, 2 * ValueMargin, 0, minor_tick_y1 + ValueMargin,
                    Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
                    format_time(t - major_t, minor_prefix));
            p.drawLine(QPoint(x, minor_tick_y1),
                QPoint(x, tick_y2));
        }

        division++;
    }

    // Draw the cursors
    auto &cursor_list = view.get_cursorList();
    // 遗留A2：游标标签的"完整测量"判定加 per-tab 兜底（本 ctx 文档为
    // 显示来源时——其他 ctx 采集/静止——同样显示完整标签）。
    bool bWorkStoped = view.data_source()->is_stopped_status() ||
                       view.display_source_is_document();

    for (auto &cursor : cursor_list)
    {
        cursor->paint_label(p, rect, prefix, bWorkStoped);
    }

    if (cursor_list.size()) {
        auto i = cursor_list.begin();

        while (i != cursor_list.end()) {
            (*i)->paint_label(p, rect, prefix, bWorkStoped);
            i++;
        }
    }

    if (view.trig_cursor_shown()) {
        view.get_trig_cursor()->paint_fix_label(p, rect, prefix, 'T', view.get_trig_cursor()->get_color(), false);
    }
    if (view.search_cursor_shown()) {
        view.get_search_cursor()->paint_fix_label(p, rect, prefix, 'S', view.get_search_cursor()->get_color(), true);
    }
}

void paint_osc_tick_mark(QPainter &p, IRenderView &view, const QRect &rect,
    const QColor &fore, double &min_period, unsigned int &prefix)
{
    const double MinValueSpacing = 16.0;
    const int ValueMargin = 5;

    double typical_width;
    double tick_period = 0;
    double scale = view.scale();
    int64_t offset = 0;

    double view_width = view.get_view_width();
    if (view_width <= 0) {
        return;
    }
    double scale_width = scale * view_width;
    if (scale_width <= 0) {
        return;
    }

    // Find tick spacing, and number formatting that does not cause
    // value to collide.
    auto *ds = view.data_source();
    if (!ds || !ds->device()) {
        return;
    }
    min_period = ds->device()->get_time_base() * std::pow(10.0, -9.0);

    const int order = (int)floorf(log10f(scale_width));
    //const double order_decimal = pow(10, order);
    int prefix_val = (order - kFirstSIPrefixPower) / 3;
    if (prefix_val < 0) prefix_val = 0;
    if (prefix_val >= kSIPrefixCount) prefix_val = kSIPrefixCount - 1;
    prefix = prefix_val;
    typical_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
        Qt::AlignLeft | Qt::AlignTop, format_time(offset * scale,
        prefix)).width() + MinValueSpacing;

    int tick_period_loop_count = 0;
    do
    {
        tick_period += min_period;
        if (++tick_period_loop_count > 1000) {
            break;
        }
    } while(typical_width > tick_period / scale);

    const int text_height = p.boundingRect(0, 0, INT_MAX, INT_MAX,
        Qt::AlignLeft | Qt::AlignTop, "8").height();

    // Draw the tick marks
    QColor fore_color = fore;
    fore_color.setAlpha(IRenderView::ForeAlpha);
    p.setPen(fore_color);

    const double minor_tick_period = tick_period / kMinPeriodScale;
    const int minor_order = (int)floorf(log10f(minor_tick_period));
    //const double minor_order_decimal = pow(10, minor_order);
    int minor_prefix_val = (minor_order - kFirstSIPrefixPower) / 3;
    if (minor_prefix_val < 0) minor_prefix_val = 0;
    if (minor_prefix_val >= kSIPrefixCount) minor_prefix_val = kSIPrefixCount - 1;
    const unsigned int minor_prefix = minor_prefix_val;

    const double first_major_division =
        floor(offset * scale / tick_period);
    const double first_minor_division =
        floor(offset * scale / minor_tick_period + 1);
    const double t0 = first_major_division * tick_period;

    int division = (int)round(first_minor_division -
        first_major_division * kMinPeriodScale) - 1;

    const int major_tick_y1 = text_height + ValueMargin * 3;
    const int tick_y2 = rect.height();
    const int minor_tick_y1 = (major_tick_y1 + tick_y2) / 2;

    int x = rect.left() - 1;

    const double inc_text_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                                 Qt::AlignLeft | Qt::AlignTop,
                                                 format_time(minor_tick_period,
                                                             minor_prefix)).width() + MinValueSpacing;
    int loop_count = 0;
    while (true) {
        const double t = t0 + division * minor_tick_period;
        const double major_t = t0 + floor(division / kMinPeriodScale) * tick_period;

        double x_double = t / scale - offset;
        if (x_double > rect.right()) {
            break;
        }

        if (++loop_count > 2000) {
            break;
        }

        if (x_double < -1e6 || x_double > 1e6) {
            division++;
            continue;
        }

        x = (int)x_double;

        if (division % kMinPeriodScale == 0)
        {
            // Draw a major tick
            p.drawText(x, 2 * ValueMargin, 0, text_height,
                Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
                format_time(t, prefix));
            p.drawLine(QPoint(x, major_tick_y1), QPoint(x, tick_y2));
        }
        else
        {
            // Draw a minor tick
            if (minor_tick_period / scale > 2 * typical_width)
                p.drawText(x, 2 * ValueMargin, 0, text_height,
                    Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
                    format_time(t, prefix));
            //else if ((tick_period / scale > width() / 4) && (minor_tick_period / scale > inc_text_width))
            else if (minor_tick_period / scale > 1.1 * inc_text_width ||
                     tick_period / scale > view.get_view_width())
                p.drawText(x, 2 * ValueMargin, 0, minor_tick_y1 + ValueMargin,
                    Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
                    format_time(t - major_t, minor_prefix));
            p.drawLine(QPoint(x, minor_tick_y1), QPoint(x, tick_y2));
        }

        division++;
    }

    // Draw the cursors
    auto &cursor_list = view.get_cursorList();

    if (!cursor_list.empty()) {
        bool bWorkStoped = view.data_source()->is_stopped_status();

        for (auto &cursor : cursor_list) {
            cursor->paint_label(p, rect, prefix, bWorkStoped);
        }
    }

    if (view.trig_cursor_shown()) {
        view.get_trig_cursor()->paint_fix_label(p, rect, prefix, 'T', view.get_trig_cursor()->get_color(), false);
    }
    if (view.search_cursor_shown()) {
        view.get_search_cursor()->paint_fix_label(p, rect, prefix, 'S', view.get_search_cursor()->get_color(), true);
    }
}

} // namespace view
} // namespace pv
