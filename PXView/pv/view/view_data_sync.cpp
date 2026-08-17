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

// Phase J (modernize-view-layer-v3): data-source / data-document / capture
// lifecycle data-sync behaviour extracted from the View God-class.
// ViewDataSync owns its state (_data_source / _document / _back_ready /
// _data_updated_timer). It is declared a friend of View so it can access
// View's private widget members (_time_viewport, _fft_viewport, _viewcenter,
// _ruler, _device_agent, etc.) and call private helper methods
// (mark_derived_traces_dirty, rebuild_signals, document_snapshot_source,
// get_work_mode, limit_scale_offset, set_trig_cursor_posistion,
// set_search_pos, set_update, headerWidth, update_margins, update_scroll,
// update_scale_offset, viewport_update).

#include "pv/view/view_data_sync.h"
#include "pv/view/view_context.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QResizeEvent>
#include <QtGlobal>

#include "pv/view/view.h"

#include "pv/config/appconfig.h"
#include "pv/data/datasource.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/model/signalmodel.h"
#include "pv/base/pxvdef.h"
#include "pv/session/sigsession.h"
#include "pv/toolbars/samplingbar.h"

#include "pv/view/signal/analogsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/component/header.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/view/trace/mathtrace.h"
#include "pv/view/component/ruler.h"
#include "pv/view/signal/signal.h"
#include "pv/view/signal/signalfactory.h"
#include "pv/view/viewport/viewport.h"
#include "pv/view/component/viewstatus.h"

using namespace std;

namespace pv {
namespace view {

// =============================================================================
// DRY helpers: eliminate repeated switch-case boilerplate via Signal
// polymorphism (set_data_from_source / clear_data).
// =============================================================================

void ViewDataSync::apply_source_to_signals(pv::data::DataSource *source) {
  for (auto &sig : _view->get_own_signals())
    sig->set_data_from_source(source);
}

void ViewDataSync::clear_all_signal_data() {
  for (auto &sig : _view->get_own_signals())
    sig->clear_data();
}

void ViewDataSync::refresh_dso_signal_paint(pv::data::DataSource *source,
                                             bool set_scale) {
  if (!source)
    return;
  for (auto &sig : _view->get_own_signals()) {
    auto *s = sig->as_dso();
    if (!s)
      continue;
    if (set_scale) {
      if (_view->is_logic_rendering_mode()) {
        s->set_scale(s->get_totalHeight());
      } else {
        const int scale_height =
            s->get_view_rect().height() - View::DsoStatusHeight;
        s->set_scale(scale_height > 0 ? scale_height
                                        : s->get_view_rect().height());
      }
    }
    s->paint_prepare();
  }
}

void ViewDataSync::set_data_source(pv::data::DataSource *source) {
  _data_source = source;
  _view->mark_derived_traces_dirty();
  _view->rebuild_signals();

  if (_view->get_time_view()) {
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  if (_view->fft_viewport()) {
    _view->fft_viewport()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  _view->update();
}

void ViewDataSync::clear_signal_data() {
  clear_all_signal_data();

  if (_view->get_time_view()) {
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  if (_view->fft_viewport()) {
    _view->fft_viewport()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  _view->update();
}

void ViewDataSync::set_signal_data_from_source(
    pv::data::DataSource *source) {
  apply_source_to_signals(source);

  if (_view->get_time_view()) {
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  if (_view->fft_viewport()) {
    _view->fft_viewport()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  _view->update();
}

void ViewDataSync::set_data_document(pv::data::SessionDocument *doc) {
  // A2 fix: handle nullptr to detach the document pointer. Without this, the
  // early return left _document pointing at a soon-to-be-destroyed object,
  // causing use-after-free when the View received paint events before its
  // deleteLater() was processed.
  if (!doc) {
    _document = nullptr;
    // Clear signal data pointers so paint events don't dereference freed data.
    clear_all_signal_data();
    return;
  }

  _document = doc;
  _view->mark_derived_traces_dirty();

  if (!doc->has_data())
    return;

  if (_view->get_own_signals().empty()) {
    auto created_sigs =
        SignalFactory::create_signals(_data_source, _data_source);
    for (auto &sig : created_sigs) {
      _view->get_own_signals().push_back(std::move(sig));
    }
  }

  // SessionDocument::get_*_snapshot() delegates to get_active_*(),
  // so apply_source_to_signals(doc) correctly binds all signals.
  apply_source_to_signals(doc);

  // CRITICAL: Now that all signal raw pointers have been rebound to the new
  // snapshots, it is safe to release the document's deferred (old) shared_ptrs.
  // Until this call, the old snapshots are kept alive by _pending_* to prevent
  // use-after-free on raw pointers that were still pointing to them.
  doc->clear_pending_release();

  if (_view->get_time_view()) {
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  if (_view->fft_viewport()) {
    _view->fft_viewport()->update(UpdateEventType::UPDATE_EV_GENERIC);
  }
  _view->update();
}

void ViewDataSync::clone_signals_for_document(
    pv::data::SessionDocument *doc) {
  if (!doc)
    return;

  _view->get_own_signals().clear();

  auto created_sigs =
      SignalFactory::create_signals(_data_source, _data_source);
  for (auto &sig : created_sigs) {
    _view->get_own_signals().push_back(std::move(sig));
  }

  set_data_document(doc);
}

pv::data::DataSource *ViewDataSync::document_snapshot_source() {
  // During active capture in non-stream repeat mode, return the session
  // (SigSession) so that get_logic_snapshot() returns view_data's live
  // data. With single-buffer mode (capture_data == view_data), data goes
  // directly into view_data, and the session returns it for live display.
  //
  // Without this check, the document (holding the PREVIOUS capture's data
  // via shared_ptr) would be returned, causing the view to show frozen data
  // instead of the current capture's live data.
  //
  // This matches the DSView original design: non-stream repeat used single
  // buffer, and the view always read from the session's view_data.
  // NOTE: the guard must be as narrow as possible. `is_working()` is true
  // whenever device_status()==ST_RUNNING, and ST_RUNNING is still set while
  // RevEndPacket/CopyToDocDone/SessionStopped are queued on the async event
  // bus. Combined with `is_repeat_mode()` (which is a *persistent* collect-mode
  // setting that stays true even for an instant/single shot taken while the
  // mode selector sits on Repeat), the old condition also fired for single
  // capture: the view then bypassed _document and read view_data, which
  // capture_init() had already cleared to a fresh empty snapshot -> blank
  // screen. Use is_repeating() (excludes instant) so a single shot always
  // keeps reading the document.
  if (_data_source && _data_source->is_running_status() &&
      _data_source->is_repeating() &&
      !_data_source->is_realtime_refresh())
    return _data_source;

  // Active real-time refresh (loop mode, or stream+single/repeat while capturing):
  // the live data lives in the session's capture_data/view_data, NOT in the
  // document (which still holds the PREVIOUS capture's data via shared_ptr).
  // Returning the document here would freeze the view on stale data while the
  // loop capture keeps streaming underneath (PathDiag keeps advancing but no
  // RenderDiag appears). is_realtime_refresh() is only true while is_working();
  // a buffer-mode single shot keeps it false and falls through to the document,
  // preserving the blank-screen guard for single shots.
  if (_data_source && _data_source->is_working() &&
      _data_source->is_realtime_refresh())
    return _data_source;

  if (_document && _document->has_data()) {
    return _document;
  }
  return _data_source;
}

void ViewDataSync::frame_began() {
  // Reset search state via the public View API (forwards to ViewCursors)
  _view->set_search_pos(0, false);
}

void ViewDataSync::receive_end() {
  if (_view->is_logic_rendering_mode()) {
    bool rle = false;
    uint64_t actual_samples;
    bool ret;

    ret = _view->device_agent()->get_config_bool(SR_CONF_RLE, rle);

    if (ret && rle) {
      ret = _view->device_agent()->get_config_uint64(SR_CONF_ACTUAL_SAMPLES,
                                                     actual_samples);
      if (ret) {
        if (actual_samples !=
            _view->document_snapshot_source()->cur_samplelimits()) {
          _view->viewstatus_widget()->set_rle_depth(actual_samples);
        }
      }
    }
  }
  _view->get_time_view()->unshow_wait_trigger();

  _view->limit_scale_offset();
}

void ViewDataSync::receive_trigger(quint64 trig_pos1) {
  // CRITICAL FIX: 使用 feed_in_trigger() 传入的最新 trig_pos1,而不是从
  // document_snapshot_source()->get_trigger_pos() 读取。
  //
  // 旧实现 (void)trig_pos1; 然后读 document_snapshot_source()->get_trigger_pos(),
  // 但在第一次采集时 document 已经有上一次采集的数据(has_data() 返回 true),
  // document_snapshot_source() 返回 document,读取的是 document->_trigger_pos
  // (旧值!),导致光标显示在旧位置。直到 copy_data_to_document() 把新值复制到
  // document 后,下一次采集光标才显示正确。
  //
  // feed_in_trigger() 传入的 trig_pos1 是 capture_data()->_trig_pos,即驱动
  // 通过 SR_DF_TRIGGER 包返回的最新 trigger_pos,这是正确的值。
  _view->set_trig_cursor_posistion(trig_pos1);
}

void ViewDataSync::data_updated() {
  // Detect DSO continuous (running) mode for the fast-path below.
  const bool is_dso_running =
      (_view->get_work_mode() == DSO && _data_source &&
       _data_source->is_running_status());

  // --- DSO continuous fast path ---
  // In DSO continuous mode the demo/hardware driver sends ~40 packets/sec.
  // The original code did a full layout/margins/scroll rebuild every frame,
  // which is unnecessary because viewport geometry doesn't change between
  // frames. We throttle to ~30 FPS (33ms) and only update data pointers +
  // paint_prepare, skipping layout/scroll rebuilds.
  if (is_dso_running) {
    if (_data_updated_timer.isValid() &&
        _data_updated_timer.elapsed() < 33) {
      _view->set_update(_view->get_time_view(), true);
      return;
    }

    auto *source = _view->document_snapshot_source();
    if (source) {
    for (auto &sig : _view->get_own_signals())
      sig->set_data_from_source(source);
    refresh_dso_signal_paint(source, false);
      if (_view->get_own_lissajous_trace())
        _view->get_own_lissajous_trace()->set_data(source->get_dso_snapshot());
    }

    _view->set_update(_view->get_time_view(), true);
    _view->set_update(_view->fft_viewport(), true);
    _view->viewport_update();
    _data_updated_timer.start();
    return;
  }

  // --- General path (non-DSO or DSO stopped) ---
  // Deduplicate rapid calls: if called within 16ms of the last execution,
  // only mark viewports dirty without doing full update cycle.
  // CRITICAL: Still update signal data pointers in the dedup path!
  // Without this, when SR_DF_END arrives right after the last data packet
  // (< 16ms), data_updated() is deduplicated and signal _data pointers
  // remain null/stale. The subsequent viewport_update() (from signals_changed)
  // triggers paint, but paint_mid_align checks !_data → returns → blank screen.
  if (_data_updated_timer.isValid() &&
      _data_updated_timer.elapsed() < 16) {
    auto *source = _view->document_snapshot_source();
    if (source) {
      apply_source_to_signals(source);
      refresh_dso_signal_paint(source, false);
    }
    _view->set_update(_view->get_time_view(), true);
    _view->set_update(_view->fft_viewport(), true);
    return;
  }

  // Refresh data pointers in render objects (does NOT rebuild them).
  auto *source = _view->document_snapshot_source();
  if (source) {
    apply_source_to_signals(source);
    refresh_dso_signal_paint(source, true);

    if (_view->get_own_lissajous_trace()) {
      _view->get_own_lissajous_trace()->set_data(source->get_dso_snapshot());
    }
  }

  _view->set_viewport_margins(_view->headerWidth(), _view->rulerHeight(), 0, 0);
  _view->update_margins();
  _view->update_scale_offset();
  _view->update_scroll();

  _view->get_time_view()->unshow_wait_trigger();
  _view->set_update(_view->get_time_view(), true);
  _view->set_update(_view->fft_viewport(), true);
  _view->viewport_update();
  _view->get_ruler()->update();

  _data_updated_timer.start();
}

void ViewDataSync::set_receive_len(uint64_t len) {
  if (_view->get_time_view())
    _view->get_time_view()->set_receive_len(len);

  if (_view->fft_viewport() && _view->device_agent()->get_work_mode() == DSO)
    _view->fft_viewport()->set_receive_len(len);
}

// =============================================================================
// Phase J additional: coordinate conversion / capture / scale / geometry
// =============================================================================

double ViewDataSync::index2pixel(uint64_t index, bool has_hoff) {
  // Delegate to ViewContext (extracted for testability)
  const ViewContext ctx = ViewContext::from_view(_view, has_hoff);
  return ctx.index2pixel(index, has_hoff);
}

uint64_t ViewDataSync::pixel2index(double pixel) {
  // Delegate to ViewContext (extracted for testability)
  const ViewContext ctx = ViewContext::from_view(_view, true);
  const uint64_t sampleIndex = ctx.pixel2index(pixel);

  /* Clamp to [0, sample_limit-1] to prevent cursor indices from exceeding
   * the valid sample range. */
  const uint64_t sample_limit = _view->document_snapshot_source()->cur_samplelimits();
  if (sample_limit > 0 && sampleIndex >= sample_limit)
    return sample_limit - 1;

  return sampleIndex;
}

void ViewDataSync::capture_init() {
  int width = _view->get_view_width();
  if (width == 0) {
    return;
  }

  int mode = _view->get_work_mode();

  if (mode == DSO)
    _view->show_trig_cursor(true);
  else if (!_data_source->is_repeating())
    _view->show_trig_cursor(false);

  double sampletime = _view->document_snapshot_source()->cur_sampletime();
  if (sampletime > 0) {
    _view->layout_delegate()->set_maxscale(sampletime / (width * View::MaxViewRate));

    if (mode == ANALOG) {
      _view->set_scale_offset(_view->layout_delegate()->maxscale(), 0);
    }
  }

  _view->status_clear();

  _view->set_trig_hoff(0);
}

void ViewDataSync::show_region(uint64_t start, uint64_t end, bool keep) {
  if (start > end) {
    pxv_warn("ViewDataSync::show_region: start %llu > end %llu, swapping",
             (unsigned long long)start, (unsigned long long)end);
    std::swap(start, end);
  }

  int width = _view->get_view_width();
  if (width == 0) {
    return;
  }

  if (keep) {
    _view->set_all_update(true);
    _view->update();
  } else if (_data_source->get_map_zoom() == 0) {
    const double ideal_scale = (end - start) * 2.0 /
                               _view->document_snapshot_source()->cur_snap_samplerate() /
                               width;
    const double new_scale = max(min(ideal_scale, _view->layout_delegate()->maxscale()), _view->layout_delegate()->minscale());
    const double new_off =
        (start + end) * 0.5 /
            (_view->document_snapshot_source()->cur_snap_samplerate() * new_scale) -
        (width / 2.0);
    _view->set_scale_offset(new_scale, new_off);
  } else {
    const double new_scale = _view->scale();
    const double new_off =
        (start + end) * 0.5 /
            (_view->document_snapshot_source()->cur_snap_samplerate() * new_scale) -
        (width / 2.0);
    _view->set_scale_offset(new_scale, new_off);
  }
}

void ViewDataSync::timebase_changed() {
  int width = _view->get_view_width();
  if (width == 0) {
    return;
  }

  if (_view->get_work_mode() != DSO) {
    return;
  }

  double scale = _view->scale();
  double hori_res = _view->sampling_bar()->get_hori_res();

  if (hori_res > 0) {
    scale = _data_source->cur_view_time() / width;
  }

  _view->set_scale_offset(scale, _view->offset());
}

void ViewDataSync::mode_changed() {
  // Reset DSO user zoom factor on mode transition — entering DSO should
  // start at fit-frame (1.0), and leaving DSO shouldn't carry a stale
  // factor back in if the user later re-enters DSO.
  _view->layout_delegate()->set_dso_zoom_factor(1.0);
  if (_view->device_agent()->is_virtual()) {
    uint64_t samplerate = _view->document_snapshot_source()->cur_snap_samplerate();
    if (samplerate > 0)
      _view->set_scale_offset(View::WellSamplesPerPixel * 1.0 / samplerate, _view->layout_delegate()->offset());
  }
  _view->set_scale_offset(max(min(_view->layout_delegate()->scale(), _view->layout_delegate()->maxscale()), _view->layout_delegate()->minscale()), _view->layout_delegate()->offset());
}

void ViewDataSync::auto_set_max_scale() {
  const double limitTime = _view->document_snapshot_source()->cur_sampletime();
  const int width = _view->get_view_width();

  if (width > 0) {
    _view->layout_delegate()->set_maxscale(limitTime / (width * View::MaxViewRate));
    _view->set_scale(_view->layout_delegate()->maxscale());
  }
}

int ViewDataSync::get_view_width() {
  int view_width = 0;
  if (_view->get_work_mode() == DSO) {
    for (auto &s : _view->get_own_signals()) {
      view_width = max(view_width, s->get_view_rect().width());
    }
  } else {
    view_width = _view->viewcenter_widget()->width();
  }

  if (view_width == 0) {
    view_width = 1;
  }

  return view_width;
}

int ViewDataSync::get_view_height() {
  int view_height = 0;
  if (_view->get_work_mode() == DSO) {
    for (auto &s : _view->get_own_signals()) {
      view_height = max(view_height, s->get_view_rect().height());
    }
  } else {
    view_height = _view->get_time_view() ? _view->get_time_view()->height() : 0;
  }

  return view_height;
}

QRect ViewDataSync::get_view_rect() {
  if (_view->get_work_mode() == DSO) {
    const auto &sigs = _view->get_own_signals();
    if (sigs.size() > 0) {
      return sigs[0]->get_view_rect();
    }
  }

  return _view->viewcenter_widget()->rect();
}

int64_t ViewDataSync::get_logic_lst_data_offset() {
  int width = _view->get_view_width();
  assert(width > 0);

  return ceil((_data_source->get_logic_data_view_time() / _view->layout_delegate()->scale()) -
              (width * View::MaxViewRate));
}

void ViewDataSync::scroll_to_logic_last_data_time() {
  _view->set_scale_offset(_view->scale(), get_logic_lst_data_offset() + 10);
}

// DSO calibration dialog (show_calibration / on_calibration_closed /
// check_calibration) removed: Calibration class and SR_CONF_CALI fork key
// were deleted (DSO mode deprecated, DSCope hardware dropped).

void ViewDataSync::vDial_updated() {
  auto math_trace = _view->get_own_math_trace();
  if (math_trace && math_trace->enabled()) {
    math_trace->update_vDial();
  }
}

void ViewDataSync::dso_factor_updated() {
  auto math_trace = _view->get_own_math_trace();
  if (math_trace && math_trace->enabled()) {
    math_trace->update_vDial();
  }
}

QString ViewDataSync::get_index_delta(uint64_t start, uint64_t end) {
  if (start == end)
    return "0";

  uint64_t delta_sample = (start > end) ? start - end : end - start;
  return _view->get_ruler()->format_real_time(
      delta_sample, _view->document_snapshot_source()->cur_snap_samplerate());
}

// =============================================================================
// Phase J additional: Qt event handling bodies
// =============================================================================

bool ViewDataSync::eventFilter(QObject *object, QEvent *event) {
  if (_view->destroying())
    return false;

  const QEvent::Type type = event->type();
  if (type == QEvent::MouseMove) {
    const QMouseEvent *const mouse_event = static_cast<QMouseEvent *>(event);
    if (object == _view->get_ruler() || object == _view->get_time_view() ||
        object == _view->fft_viewport()) {
      double cur_periods = (mouse_event->position().toPoint().x() + _view->layout_delegate()->offset()) *
                           _view->layout_delegate()->scale() / _view->get_ruler()->get_min_period();
      int integer_x =
          round(cur_periods) * _view->get_ruler()->get_min_period() / _view->layout_delegate()->scale() - _view->layout_delegate()->offset();
      double cur_deviate_x =
          qAbs(mouse_event->position().toPoint().x() - integer_x);
      if (_view->is_logic_rendering_mode() && cur_deviate_x < 10)
        _view->hover_point() = QPoint(integer_x, mouse_event->position().toPoint().y());
      else
        _view->hover_point() = mouse_event->position().toPoint();
    } else if (object == _view->header_widget())
      _view->hover_point() = QPoint(0, (int)mouse_event->position().y());
    else
      _view->hover_point() = QPoint(-1, -1);

    _view->hover_point_changed();
  } else if (type == QEvent::Leave) {
    _view->hover_point() = QPoint(-1, -1);
    _view->hover_point_changed();
  }

  return false;
}

void ViewDataSync::resizeEvent(QResizeEvent *event) {
  (void)event;
  int width = _view->get_view_width();

  if (width == 0) {
    return;
  }

  bool widthChanged = (_view->layout_delegate()->lastWidth() != width);
  _view->layout_delegate()->set_lastWidth(width);

  if (!widthChanged && _view->get_work_mode() != DSO) {
    _view->set_viewport_margins(_view->headerWidth(), _view->rulerHeight(), 0, 0);
    _view->header_widget()->header_resize();
    _view->update_scroll();
    _view->viewport_update();
    return;
  }

  _view->reconstruct();
  _view->set_viewport_margins(_view->headerWidth(), _view->rulerHeight(), 0, 0);
  _view->update_margins();
  _view->update_scroll();
  _view->signals_changed(nullptr);

  if (_view->get_work_mode() == DSO) {
    _view->set_scale_offset(_data_source->cur_view_time() / width, _view->layout_delegate()->offset());
  }

  if (_view->get_work_mode() != DSO) {
    _view->layout_delegate()->set_maxscale(_view->document_snapshot_source()->cur_sampletime() / (width * View::MaxViewRate));
    if (_view->layout_delegate()->scale() > _view->layout_delegate()->maxscale()) {
      _view->set_scale_offset(_view->layout_delegate()->maxscale(), _view->layout_delegate()->offset());
    }
  } else {
    _view->layout_delegate()->set_maxscale(1e9);
  }

  _view->get_ruler()->update();
  _view->header_widget()->header_resize();
  _view->set_update(_view->get_time_view(), true);
  _view->set_update(_view->fft_viewport(), true);
  _view->resize();
  _view->schedule_visible_range_notify();
}

// =============================================================================
// ViewContext::from_view implementation
// Defined here (not in view_context.cpp) because it needs View's full
// definition, which would pull in View/Session/Snapshot dependencies that
// unit tests should not need to link.
// =============================================================================

ViewContext ViewContext::from_view(View *view, bool has_hoff)
{
    if (!view)
        return ViewContext{};

    const double sr = view->document_snapshot_source()
        ? (double)view->document_snapshot_source()->cur_snap_samplerate()
        : 0;
    return ViewContext(
        sr,
        view->scale(),
        view->offset(),
        has_hoff ? view->trig_hoff() : 0.0
    );
}

} // namespace view
} // namespace pv
