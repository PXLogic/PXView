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

#include <QtGlobal>  // quint64

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
// during Phase J of the modernize-view-layer-v3 spec. All data-sync state
// (_data_source / _document / _own_signals / _own_lissajous_trace /
// _time_viewport / _fft_viewport / _device_agent / _viewbottom /
// _data_updated_timer / _search_hit / _search_pos) still lives on View;
// this class only owns the *behaviour*. View declares
// `friend class ViewDataSync;` so the delegate can read and mutate those
// private members directly.
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

private:
  View *_view;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_DATA_SYNC_H
