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

// Phase E (modernize-view-layer-v2): scale / offset / scroll / margin
// behaviour extracted from the View God-class. Since Phase 1 state
// migration, scale/offset state lives on ViewLayout directly. View's
// widgets (_header / _ruler / _viewcenter / _time_viewport) are still
// accessed via _view->… because they are View-owned QWidgets.

#include "view_layout.h"

#include <cassert>
#include <cmath>
#include <algorithm>

#include <QDebug>
#include <QScrollBar>
#include "../log.h"

#include "view.h"
#include "viewport.h"
#include "devmode.h"
#include "header.h"
#include "ruler.h"
#include "../data/datasource.h"
#include "../sigsession.h"
#include "../toolbars/samplingbar.h"

using namespace std;

namespace pv {
namespace view {

void ViewLayout::set_scale_offset(double scale, int64_t offset) {
  // Bidirectional clamping: both _scale and _offset are clamped to their
  // valid ranges. Without the upper-bound clamp on _offset, trigger cursor
  // positioning (set_trig_cursor_posistion) can scroll past the data end.
  _preScale = _scale;
  _preOffset = _offset;

  _scale = max(min(scale, _maxscale), _minscale);
  _offset = floor(max(min(offset, _view->get_max_offset()),
                              _view->get_min_offset()));

  if (_scale != _preScale || _offset != _preOffset) {
    update_scroll();
    _view->header_widget()->update();
    _view->get_ruler()->update();
    _view->viewport_update();
  }
  _view->schedule_visible_range_notify();
}

void ViewLayout::limit_scale_offset() {
  int width = _view->get_view_width();
  if (_view->get_work_mode() != DSO) {
    double sampletime = _view->document_snapshot_source()->cur_sampletime();
    uint64_t samplerate = _view->document_snapshot_source()->cur_snap_samplerate();
    if (sampletime > 0 && samplerate > 0 && width > 0) {
      _maxscale = sampletime / (width * View::MaxViewRate);
      _minscale = (1.0 / samplerate) / View::MaxPixelsPerSample;
    }
    _scale = max(min(_scale, _maxscale), _minscale);
  } else {
    // DSO mode: re-derive _scale from the (possibly changed) base_scale so
    // the user's _dso_zoom_factor is respected. limit_scale_offset() is
    // called from receive_end(), which fires after capture ends — without
    // updating scroll here the horizontal scrollbar range stays at its
    // pre-capture value (often 0), making the bar un-draggable even after
    // the user zoomed in.
    const double base_scale = _view->data_source()->cur_view_time() / width;
    if (base_scale > 0) {
      _maxscale = 1e9;
      _minscale = base_scale * 1e-6;
      _scale = base_scale * _dso_zoom_factor;
      _scale = max(min(_scale, _maxscale), _minscale);
      _dso_zoom_factor = _scale / base_scale;
    }
  }
  _offset =
      max(min(_offset, get_max_offset()), get_min_offset());
  update_scroll();
  _view->get_ruler()->update();
  _view->viewport_update();
  _view->schedule_visible_range_notify();
}

void ViewLayout::update_scale_offset() {
  int width = _view->get_view_width();
  if (width == 0) {
    return;
  }

  if (_view->get_work_mode() != DSO) {
    double sampletime = _view->document_snapshot_source()->cur_sampletime();
    uint64_t samplerate = _view->document_snapshot_source()->cur_snap_samplerate();
    if (sampletime > 0 && samplerate > 0) {
      _maxscale = sampletime / (width * View::MaxViewRate);
      _minscale = (1.0 / samplerate) / View::MaxPixelsPerSample;
    } else {
      _maxscale = 1e9;
      _minscale = 1e-15;
    }
    _scale = max(min(_scale, _maxscale), _minscale);
  } else {
    // DSO mode: base_scale = fit one frame to viewport width. User zoom
    // is preserved across data frames via _dso_zoom_factor (zoom() only
    // mutates the factor; this re-derives _scale every frame). This lets
    // the user zoom in and pan horizontally like LOGIC mode, instead of
    // the original DSView design that stepped discrete timebase values.
    const double base_scale = _view->data_source()->cur_view_time() / width;
    if (base_scale > 0) {
      // Keep original _maxscale=1e9 (no zoom-out beyond fit-frame is
      // naturally prevented because get_max_offset() would go negative,
      // clamping offset to 0). _minscale caps how far in you can zoom.
      _maxscale = 1e9;
      _minscale = base_scale * 1e-6;
      _scale = base_scale * _dso_zoom_factor;
      _scale = max(min(_scale, _maxscale), _minscale);
      _dso_zoom_factor = _scale / base_scale;
    } else {
      // cur_view_time() not yet available (e.g. device not opened). Keep
      // wide defaults so we don't collapse _scale to 0/NaN and blank the view.
      _maxscale = 1e9;
      _minscale = 1e-15;
      _scale = max(_scale, _minscale);
    }
  }

  // Restore upper bound clamp on _offset (Reference/DSView-master/DSView/
  // pv/view/view.cpp:660). Without `min(..., get_max_offset())` the offset
  // could remain past the data end after a scale change in DSO mode,
  // causing the same off-screen cursor / waveform symptom.
  _offset = max(min(_offset, get_max_offset()),
                        get_min_offset());

  _preScale = _scale;
  _preOffset = _offset;

  _view->get_ruler()->update();
  _view->viewport_update();
  _view->schedule_visible_range_notify();
}

void ViewLayout::set_scale(double scale) {
  if (scale < _minscale)
    scale = _minscale;
  if (scale > _maxscale)
    scale = _maxscale;

  if (_scale != scale) {
    _scale = scale;
    _view->header_widget()->update();
    _view->get_ruler()->update();
    _view->viewport_update();
    update_scroll();
  }
  _view->schedule_visible_range_notify();
}

void ViewLayout::zoom(double steps) {
  int width = _view->get_view_width();
  if (width > 0) {
    zoom(steps, width / 2);
  }
}

bool ViewLayout::zoom(double steps, int offset) {
  int width = _view->get_view_width();
  if (width == 0) {
    return false;
  }

  bool ret = true;
  _preScale = _scale;
  _preOffset = _offset;

  if (_view->get_work_mode() != DSO) {
    // LOGIC/ANALOG: direct continuous scale zoom
    _scale *= std::pow(3.0 / 2.0, -steps);
    _scale = max(min(_scale, _maxscale), _minscale);
  } else {
    // DSO mode: wheel steps through discrete timebase values via hori_knob(),
    // matching DSView's original design (Reference/DSView-master/DSView/pv/
    // view/view.cpp:301-313). This keeps the wheel and the timebase combobox
    // in sync — scrolling the wheel changes the selected timebase item, just
    // like clicking the combobox. The previous design (mutating _dso_zoom_factor
    // for continuous view zoom) decoupled the wheel from the combobox, so
    // selecting a timebase from the dropdown made the wheel appear to "do
    // nothing" (it changed the zoom factor, not the timebase).
    // The instant-mode running guard is retained: don't zoom while an
    // instant capture is in progress.
    if (_view->data_source()->is_running_status() &&
        _view->data_source()->is_instant()) {
      return ret;
    }
    double hori_res = -1;
    if (steps > 0.5)
      hori_res = _view->sampling_bar()->hori_knob(-1);
    else if (steps < -0.5)
      hori_res = _view->sampling_bar()->hori_knob(1);

    if (hori_res > 0) {
      const double scale = _view->data_source()->cur_view_time() / width;
      _scale = max(min(scale, _maxscale), _minscale);
    } else {
      ret = false;
    }
  }

  _offset =
      floor((_offset + offset) * (_preScale / _scale) - offset);
  _offset =
      max(min(_offset, get_max_offset()), get_min_offset());

  if (_scale != _preScale || _offset != _preOffset) {
    _view->header_widget()->update();
    _view->get_ruler()->update();
    _view->viewport_update();
    update_scroll();
  }
  _view->schedule_visible_range_notify();

  return ret;
}

void ViewLayout::h_scroll_value_changed(int value) {
  if (_updating_scroll)
    return;

  _preOffset = _offset;

  const int range = _view->horizontalScrollBar()->maximum();
  if (range < _view->maxScrollValue())
    _offset = value;
  else {
    int64_t length = 0;
    int64_t offset = 0;
    get_scroll_layout(length, offset);
    _offset = floor(value * 1.0 / _view->maxScrollValue() * length);
  }

  _offset =
      max(min(_offset, get_max_offset()), get_min_offset());

  if (_offset != _preOffset) {
    _view->get_ruler()->update();
    _view->viewport_update();
  }
  _view->schedule_visible_range_notify();
}

void ViewLayout::get_scroll_layout(int64_t &length, int64_t &offset) {
  length = ceil(_view->document_snapshot_source()->cur_snap_sampletime() /
                _scale);
  offset = _offset;
}

void ViewLayout::update_scroll() {
  assert(_view->viewcenter_widget());

  int width = _view->get_view_width();
  if (width == 0) {
    return;
  }

  const QSize areaSize = QSize(width, _view->get_view_height());

  // Set the horizontal scroll bar
  int64_t length = 0;
  int64_t offset = 0;
  get_scroll_layout(length, offset);
  length = max(length - areaSize.width(), (int64_t)0);

  _view->horizontalScrollBar()->setPageStep(areaSize.width());

  _updating_scroll = true;

  if (length < _view->maxScrollValue()) {
    _view->horizontalScrollBar()->setRange(0, length);
    _view->horizontalScrollBar()->setSliderPosition(offset);
  } else {
    _view->horizontalScrollBar()->setRange(0, _view->maxScrollValue());
    _view->horizontalScrollBar()->setSliderPosition(
        _offset * 1.0 / length * _view->maxScrollValue());
  }

  _updating_scroll = false;

  // Set the vertical scrollbar
  int totalContentHeight = 0;
  if (_view->get_time_view())
    totalContentHeight = _view->get_time_view()->get_total_height();
  int vRange = max(0, totalContentHeight - areaSize.height());
  if (vRange > 0)
    _view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  else
    _view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  _view->verticalScrollBar()->setPageStep(areaSize.height());
  _view->verticalScrollBar()->setRange(0, vRange);
  _view->verticalScrollBar()->setSliderPosition(_vOffset);
}

void ViewLayout::update_margins() {
  int width = _view->get_view_width();

  if (width > 0) {
    _view->get_ruler()->setGeometry(_view->viewcenter_widget()->x(), 0,
                               _view->width() - _view->viewcenter_widget()->x(),
                               _view->viewcenter_widget()->y());
    _view->header_widget()->setGeometry(0, _view->viewcenter_widget()->y(),
                                _view->viewcenter_widget()->x(),
                                _view->viewcenter_widget()->height());
    _view->devmode_widget()->setGeometry(0, 0, _view->viewcenter_widget()->x(),
                                 _view->viewcenter_widget()->y());
  }
}

int64_t ViewLayout::get_min_offset() {
  int width = _view->get_view_width();
  assert(width > 0);

  if (View::MaxViewRate > 1)
    return floor(width * (1 - View::MaxViewRate));
  else
    return 0;
}

int64_t ViewLayout::get_max_offset() {
  int width = _view->get_view_width();
  assert(width > 0);

  return ceil((_view->document_snapshot_source()->cur_snap_sampletime() /
               _scale) -
              (width * View::MaxViewRate));
}

} // namespace view
} // namespace pv
