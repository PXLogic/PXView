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

#ifndef PXVIEW_PV_VIEW_VIEW_DATA_SYNC_H
#define PXVIEW_PV_VIEW_VIEW_DATA_SYNC_H

#include <cstdint>

#include <QtGlobal>
#include <QElapsedTimer>  // quint64

class QObject;
class QEvent;
class QResizeEvent;
class QRect;
class QString;

namespace pv {

namespace data {
class DataSource;
class SessionDocument;
} // namespace data

namespace view {

class View;

// ViewDataSync — delegate for View's data-source / data-document / capture
// lifecycle data-sync responsibilities. Extracted from the View God-class
// during Phase J of the modernize-view-layer-v3 spec. Data-sync state
// (_data_source / _document / _back_ready / _data_updated_timer) lives in
// this class. View declares `friend class ViewDataSync;` so the delegate
// can access View's private widget members (_time_viewport, _fft_viewport,
// _viewcenter, _ruler, _device_agent, etc.) and call private helper methods.
class ViewDataSync {
public:
  explicit ViewDataSync(View *view) : _view(view) {}

  // -- data source / document binding -----------------------------------
  void set_data_source(pv::data::DataSource *source);
  void set_data_document(pv::data::SessionDocument *doc);
  void clone_signals_for_document(pv::data::SessionDocument *doc);
  pv::data::DataSource *document_snapshot_source();

  // -- per-signal snapshot pointer refresh ------------------------------
  void clear_signal_data();
  void set_signal_data_from_source(pv::data::DataSource *source);

  // -- capture lifecycle (frame_began / receive_end / receive_trigger) --
  void frame_began();
  void receive_end();
  void receive_trigger(quint64 trig_pos1);

  // -- data refresh / receive length ------------------------------------
  void data_updated();
  void set_receive_len(uint64_t len);

  // -- coordinate conversion (Phase J additional) -----------------------
  double index2pixel(uint64_t index, bool has_hoff);
  uint64_t pixel2index(double pixel);

  // -- capture init / scale / region (Phase J additional) ---------------
  void capture_init();
  void show_region(uint64_t start, uint64_t end, bool keep);
  void timebase_changed();
  void mode_changed();
  void auto_set_max_scale();

  // -- view geometry (Phase J additional) -------------------------------
  int get_view_width();
  int get_view_height();
  QRect get_view_rect();
  int64_t get_logic_lst_data_offset();
  void scroll_to_logic_last_data_time();

  // -- dso / measure (Phase J additional) -------------------------------
  void vDial_updated();
  void dso_factor_updated();
  QString get_index_delta(uint64_t start, uint64_t end);

  // -- Qt event handling bodies (Phase J additional) --------------------
  bool eventFilter(QObject *object, QEvent *event);
  void resizeEvent(QResizeEvent *event);

  // -- state accessors (for View) ---------------------------------------
  pv::data::DataSource *data_source_ptr() const { return _data_source; }
  void set_data_source_ptr(pv::data::DataSource *src) { _data_source = src; }
  pv::data::SessionDocument *document_ptr() const { return _document; }
  void set_document_ptr(pv::data::SessionDocument *doc) { _document = doc; }
  bool back_ready() const { return _back_ready; }
  void set_back_ready(bool v) { _back_ready = v; }
  QElapsedTimer &data_updated_timer() { return _data_updated_timer; }

  // [PX3-DEBUG] 诊断：统计当前 view 信号中持有非空 snapshot 的数量，
  // 用于定位"切回旧 tab 后波形数据被清空"具体由哪次操作引起。
  int count_signals_with_data() const;
  void log_data_state(const char *tag) const;

private:
  View *_view;

  pv::data::DataSource *_data_source = nullptr;
  pv::data::SessionDocument *_document = nullptr;
  bool _back_ready = false;
  QElapsedTimer _data_updated_timer;

  // --- DRY helpers (eliminate repeated switch-case boilerplate) ---
  // Uses Signal::set_data_from_source() polymorphism instead of per-type
  // static_cast + set_data.
  void apply_source_to_signals(pv::data::DataSource *source);
  void clear_all_signal_data();
  // DSO-specific post-data-refresh: paint_prepare + optional set_scale.
  void refresh_dso_signal_paint(pv::data::DataSource *source,
                                 bool set_scale);
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_DATA_SYNC_H
