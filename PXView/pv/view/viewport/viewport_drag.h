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

#ifndef PXVIEW_PV_VIEW_VIEWPORT_DRAG_H
#define PXVIEW_PV_VIEW_VIEWPORT_DRAG_H

namespace pv {
namespace view {

class Viewport;

// Drag frame delegate extracted from Viewport (Phase F3).
// Holds a non-owning back-pointer to its Viewport and accesses the
// Viewport's private state through friend access. The drag-frame
// coalescing logic (applyDragFrame) and the inertial drag timer
// (on_drag_timer) live here; Viewport keeps only thin slot forwarders
// wired to its QTimer instances.
class ViewportDrag {
public:
  explicit ViewportDrag(Viewport *viewport);
  ~ViewportDrag();

  void applyDragFrame();
  void on_drag_timer();

private:
  Viewport *_viewport;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEWPORT_DRAG_H
