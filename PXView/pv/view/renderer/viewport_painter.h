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

#ifndef PXVIEW_PV_VIEW_VIEWPORT_PAINTER_H
#define PXVIEW_PV_VIEW_VIEWPORT_PAINTER_H

#include <chrono>
#include <QColor>
#include <QRect>
#include "pv/view/iview_delegates.h"

class QPainter;
class QPaintEvent;

// Frame timing: thread-local DSO paint sub-timing, written by
// DsoSignal::paint_mid and read by ViewportPainter::doPaint summary.
// (Moved here from viewport.h — widget-free, Task 3.1.)
struct DsoPaintTiming {
    bool active = false;
    qint64 get_samples_ms = 0;
    qint64 paint_draw_ms = 0;
    qint64 hw_offset_ms = 0;
    int64_t sample_count = 0;
    double samples_per_pixel = 0;
};
extern thread_local DsoPaintTiming s_dso_timing;

namespace pv {
namespace view {

// Paint delegate extracted from Viewport (Phase F1).
// Holds a non-owning back-pointer to its Viewport and drives the RenderPass
// pipeline. Task 3.1 (QML migration): the stored pointer is the widget-free
// IRenderViewport interface (Viewport implements it), so this paint code no
// longer includes viewport.h / view.h and compiles into pxview-render.
class ViewportPainter {
public:
  explicit ViewportPainter(IRenderViewport *viewport);
  ~ViewportPainter();

  void paintEvent(QPaintEvent *event);
  void doPaint(const QRect &dirtyRect = QRect());
  void paintCursors(QPainter &p);
  void paintSignals(QPainter &p, QColor fore, QColor back);
  void paintProgress(QPainter &p, QColor fore, QColor back);

private:
  IRenderViewport *_viewport;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEWPORT_PAINTER_H
