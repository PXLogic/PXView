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

#include "pv/view/view_layout.h"

#include <cassert>
#include <cmath>
#include <algorithm>

#include <QDebug>
#include <QScrollBar>
#include "pv/base/log.h"
#include "pv/base/perflog.h"

#include "pv/view/view.h"
#include "pv/view/viewport/viewport.h"
#include "pv/view/component/devmode.h"
#include "pv/view/component/header.h"
#include "pv/view/component/ruler.h"
#include "pv/data/datasource.h"
#include "pv/session/sigsession.h"
#include "pv/toolbars/samplingbar.h"

using namespace std;

namespace pv {
namespace view {

void ViewLayout::set_scale_offset(double scale, int64_t offset) {
  // 即时操作抢占：滚动、Alt+滚轮、程序化跳转都必须立刻生效，不能被在跑的
  // 缩放动画继续覆盖。取消后本函数写下的值就是终值。
  _zoom_anim.cancel();

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
    auto *src = _view->document_snapshot_source(); // 可为 null（外来采集）
    double sampletime = src ? src->cur_sampletime() : 0.0;
    uint64_t samplerate = src ? src->cur_snap_samplerate() : 0;
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
    auto *src = _view->document_snapshot_source(); // 可为 null（外来采集）
    double sampletime = src ? src->cur_sampletime() : 0.0;
    uint64_t samplerate = src ? src->cur_snap_samplerate() : 0;
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
    // 程序化设 scale 同样是即时操作，抢占在跑的缩放动画。
    _zoom_anim.cancel();
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

  // 离散跳档也是一次即时操作：抢占在跑的缩放动画（例如 DSO 档位切换、
  // 采样条旋钮、SmartZoom）。动画路径本身走 zoom_animated()，不经过这里。
  _zoom_anim.cancel();

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

void ViewLayout::apply_scale_offset_epilogue() {
  // _preScale/_preOffset 由调用方在改 _scale/_offset 之前写入。只有真正变化
  // 时才重绘三个 widget；滚动条同步与可见范围通知则总要执行。
  if (_scale != _preScale || _offset != _preOffset) {
    _view->header_widget()->update();
    _view->get_ruler()->update();
    _view->viewport_update();
    update_scroll();
  }
  _view->schedule_visible_range_notify();
}

bool ViewLayout::zoom_animated(double steps, int anchor_px, int64_t now_ms) {
  const int width = _view->get_view_width();
  if (width == 0)
    return false;

  // DSO 的水平分辨率是离散时基档位（hori_knob），线性插值会经过并不存在的
  // 中间档位，因此这里禁用缩放动画：直接走一次离散跳档。
  if (_view->get_work_mode() == DSO) {
    zoom(steps, anchor_px);
    return false;
  }

  // 复合目标（实现见 ZoomAnimation::retarget_compound）。
  // 关键：基准是**尚未到达的旧目标**，不是当前显示值 —— 否则每次滚轮都会丢弃
  // 上一格没走完的行程（实测交付率仅 26%）。
  //
  // 每格比例取 √2：物理滚轮一格是 120 单位的 angleDelta，取
  // zoom_factor = √2^steps，于是视图可见跨度按 2^(k/2) 成阶梯变化
  // （10ms → 14 → 20 → 28 → 40 → 56 → 79 → 112ms …），每格视觉增量一致。
  constexpr double kWheelZoomPerNotch = 1.4142135623730951;  // sqrt(2)

  if (!_zoom_anim.active())
    pv::base::perf::zoom_trace_begin(now_ms, visible_time_ms());

  _zoom_anim.retarget_compound(
      _scale, static_cast<double>(anchor_px), _offset,
      std::pow(kWheelZoomPerNotch, -steps), _minscale, _maxscale, now_ms,
      ZoomAnimation::DurationMs);
  return true;
}

double ViewLayout::visible_time_ms() {
  // _scale 的单位是"秒/像素"（见 get_scroll_layout: length = sampletime/_scale
  // 得到的是像素数），故可见时间跨度 = _scale * 视口宽度，换算成毫秒再 ×1000。
  return _scale * static_cast<double>(_view->get_view_width()) * 1000.0;
}

void ViewLayout::cancel_zoom_animation() {
  if (!_zoom_anim.active())
    return;
  _zoom_anim.cancel();
  pv::base::perf::zoom_trace_end("cancel", visible_time_ms());
}

bool ViewLayout::tick_zoom_animation(int64_t now_ms) {
  double new_scale = _scale;
  const bool more = _zoom_anim.sample(now_ms, new_scale);

  const double pre_scale = _scale;
  const int64_t pre_offset = _offset;
  _scale = new_scale;

  // offset 一律以**手势参照系**（retarget 时记下的 initial_scale/anchor_offset）
  // 直接算出，而不是逐帧从上一帧递推。逐帧递推每帧都要 floor 取整，误差同向
  // 累积 —— 实测 4 帧就能让锚点漂移超过 1 像素，表现为缩放时鼠标下的波形滑动。
  // 固定参照系的漂移恒 < 1 像素（推导见 ZoomAnimation::offset_for）。
  const int64_t raw = _zoom_anim.offset_for(_scale);
  const int64_t clamped = max(min(raw, get_max_offset()), get_min_offset());
  if (clamped != raw) {
    // 已顶到数据边界：重设参照系，避免后续帧与夹取结果"较劲"产生回弹。
    _zoom_anim.reanchor(_scale, clamped);
  }
  _offset = clamped;

  _preScale = pre_scale;
  _preOffset = pre_offset;
  apply_scale_offset_epilogue();

  // 诊断：必须在 _scale 落地之后再取 view_ms（本帧的可见跨度）。
  const double view_ms = visible_time_ms();
  pv::base::perf::zoom_trace_frame(now_ms, _zoom_anim.progress(now_ms), view_ms);
  if (!more)
    pv::base::perf::zoom_trace_end("done", view_ms);

  return more;
}

void ViewLayout::h_scroll_value_changed(int value) {
  // _updating_scroll 期间是动画/缩放的收尾在同步滚动条，不能当作用户拖动。
  if (_updating_scroll)
    return;

  // 用户拖动横向滚动条 = 即时操作，取消在跑的缩放动画。
  _zoom_anim.cancel();

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
  auto *src = _view->document_snapshot_source(); // 可为 null（外来采集）
  length = src ? ceil(src->cur_snap_sampletime() / _scale) : 0;
  offset = _offset;
}

void ViewLayout::update_scroll() {
  assert(_view->viewcenter_widget());

  // Defer scroll updates during decoder analog trigger display-hold to
  // avoid intermediate-frame geometry changes while the new frame is being
  // decoded and aligned.
  if (_view->is_decoder_analog_trigger_hold())
    return;

  int width = _view->get_view_width();
  if (width == 0) {
    return;
  }

  const QSize areaSize = QSize(width, _view->get_view_height());

  // Set the horizontal scroll bar
  int64_t length = 0;
  int64_t offset = 0;
  get_scroll_layout(length, offset);
  // Overflow-free equivalent of max(length - areaSize.width(), 0):
  // length may be INT64_MIN, and INT64_MIN - width overflows (UBSan).
  length = std::max(length, static_cast<int64_t>(areaSize.width())) - areaSize.width();

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
  auto *src = _view->document_snapshot_source(); // 可为 null（外来采集）
  if (!src)
    return 0;

  return ceil((src->cur_snap_sampletime() /
               _scale) -
              (width * View::MaxViewRate));
}

} // namespace view
} // namespace pv
