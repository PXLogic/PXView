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

#ifndef PXVIEW_PV_VIEW_VIEW_LAYOUT_H
#define PXVIEW_PV_VIEW_VIEW_LAYOUT_H

#include <cstdint>

#include "pv/view/iview_delegates.h"

namespace pv {
namespace view {

class View;

// ViewLayout — delegate for View's scale / offset / scroll / margin
// responsibilities. Extracted from the View God-class during Phase E of the
// modernize-view-layer-v2 spec. Since Phase 1 state migration, scale/offset
// state (_scale / _offset / _minscale / _maxscale / _preScale / _preOffset /
// _updating_scroll / _vOffset / _dso_zoom_factor) lives here. Other delegates
// (ViewCursors, ViewDataSync, ViewSignalSync) access this state via View's
// public API (scale(), offset(), set_scale_offset(), etc.) which forward to
// this delegate.
//
// Phase 8: implements IViewLayout so that future refactoring can have
// delegates depend on the abstract interface instead of the concrete
// class (enabling mock-based unit testing).
class ViewLayout : public IViewLayout {
public:
  explicit ViewLayout(View *view) : _view(view) {}

  // ---- Public state accessors (for other delegates) ----
  inline double scale() const override { return _scale; }
  inline int64_t offset() const override { return _offset; }
  inline double maxscale() const override { return _maxscale; }
  inline double minscale() const override { return _minscale; }
  inline double dso_zoom_factor() const override { return _dso_zoom_factor; }
  inline void set_dso_zoom_factor(double f) { _dso_zoom_factor = f; }

  // -- pre-scale / pre-offset (for View::set_preScale_preOffset) ----------
  inline double preScale() const { return _preScale; }
  inline int64_t preOffset() const { return _preOffset; }

  // -- signal height / vertical layout state (migrated from View) --------
  inline int spanY() const override { return _spanY; }
  inline int signalHeight() const override { return _signalHeight; }
  inline int signalHeightScale() const override { return _signalHeightScale; }
  inline int vOffset() const { return _vOffset; }
  inline void set_vOffset(int offset) { _vOffset = offset; }
  inline void set_signalHeight(int h) { _signalHeight = h; }
  inline void set_signalHeightScale(int h) { _signalHeightScale = h; }
  inline void set_spanY(int s) { _spanY = s; }

  // -- scale / offset mutators -------------------------------------------
  void set_scale_offset(double scale, int64_t offset) override;
  void limit_scale_offset();
  void update_scale_offset();
  void set_scale(double scale);

  // -- zoom --------------------------------------------------------------
  // zoom(steps) zooms around the viewport centre.
  void zoom(double steps);
  // zoom(steps, offset) zooms around the given pixel anchor; returns false
  // if the DSO horizontal resolution could not be changed.
  bool zoom(double steps, int offset);

  // -- scroll ------------------------------------------------------------
  void h_scroll_value_changed(int value);
  void update_scroll();
  void get_scroll_layout(int64_t &length, int64_t &offset) override;
  void update_margins();

  // -- offset bounds -----------------------------------------------------
  int64_t get_max_offset() override;
  int64_t get_min_offset() override;

private:
  View *_view;

  // ---- Scale / offset state (migrated from View) ----
  double _scale = 10;
  double _preScale = 1e-6;
  double _maxscale = 1e9;
  double _minscale = 1e-15;
  double _dso_zoom_factor = 1.0;
  int64_t _offset = 0;
  int64_t _preOffset = 0;
  int _vOffset = 0;
  int _lastWidth = -1;
  bool _updating_scroll = false;

  // ---- Signal height / vertical layout state (migrated from View) ----
  int _spanY = 0;
  int _signalHeight = 0;
  int _signalHeightScale = 24;  // default = View::MaxHeightUnit

public:
  // ---- Additional setters for delegate classes (Phase 2: friend elimination) ----
  inline void set_maxscale(double s) { _maxscale = s; }
  inline void set_minscale(double s) { _minscale = s; }
  inline void set_offset(int64_t o) { _offset = o; }
  inline int lastWidth() const { return _lastWidth; }
  inline void set_lastWidth(int w) { _lastWidth = w; }
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_LAYOUT_H
