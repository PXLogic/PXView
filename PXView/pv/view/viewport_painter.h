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

#include <QColor>
#include <QRect>

class QPainter;
class QPaintEvent;

namespace pv {
namespace view {

class Viewport;

// Paint delegate extracted from Viewport (Phase F1).
// Holds a non-owning back-pointer to its Viewport and accesses the
// Viewport's private state through friend access. All paint-related
// rendering lives here; Viewport keeps only thin Qt override forwarders.
class ViewportPainter {
public:
  explicit ViewportPainter(Viewport *viewport);
  ~ViewportPainter();

  void paintEvent(QPaintEvent *event);
  void doPaint(const QRect &dirtyRect = QRect());
  void paintCursors(QPainter &p);
  void paintSignals(QPainter &p, QColor fore, QColor back);
  void paintProgress(QPainter &p, QColor fore, QColor back);

private:
  Viewport *_viewport;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEWPORT_PAINTER_H
