/*
 * ruler_paint.h — widget-free ruler tick-mark painting (QML migration
 * Phase 3, Task 3.3).
 *
 * Bodies extracted VERBATIM from Ruler::draw_logic_tick_mark /
 * Ruler::draw_osc_tick_mark (ruler.cpp) with only dependency injection:
 *   * View&  -> IRenderView& (Task 3.1 interface; View implements it)
 *   * QWidget height()/rect() -> explicit rect parameter
 *   * _foreColor (QWidget palette fallback) -> fully resolved fore parameter
 *   * Ruler members _min_period/_cur_prefix -> out-parameters
 * The cursor-label tails (Cursor::paint_label / paint_fix_label) are kept
 * verbatim — they run through IRenderView + the widget-free Cursor class
 * (cursor.cpp, also pxview-render since Task 3.3).
 *
 * Ruler::draw_logic_tick_mark / Ruler::draw_osc_tick_mark delegate here
 * (zero logic change), and the QML shell's RulerItem calls the same
 * functions with its own geometry — one implementation, many shells.
 */

#ifndef PXVIEW_VIEW_RULER_PAINT_H
#define PXVIEW_VIEW_RULER_PAINT_H

#include <QColor>
#include <QRect>
#include <cstdint>

class QPainter;

namespace pv {
namespace view {

class IRenderView;

// Logic-mode time tick marks (Ruler::draw_logic_tick_mark body).
// Updates min_period / prefix exactly like the Ruler members did
// (Ruler::get_min_period / Ruler::format_time(t) keep working).
void paint_logic_tick_mark(QPainter &p, IRenderView &view, const QRect &rect,
    const QColor &fore, double &min_period, unsigned int &prefix);

// DSO-mode time tick marks (Ruler::draw_osc_tick_mark body).
void paint_osc_tick_mark(QPainter &p, IRenderView &view, const QRect &rect,
    const QColor &fore, double &min_period, unsigned int &prefix);

} // namespace view
} // namespace pv

#endif // PXVIEW_VIEW_RULER_PAINT_H
