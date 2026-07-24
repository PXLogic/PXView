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

#ifndef PXVIEW_PV_VIEW_VIEW_SIGNAL_SYNC_H
#define PXVIEW_PV_VIEW_VIEW_SIGNAL_SYNC_H

#include <cstdint>
#include <vector>

class QColor;
class QString;
class QRect;

namespace pv {

namespace data {
struct SignalConfig;
} // namespace data

namespace view {

class View;
class Trace;

// ViewSignalSync — delegate for View's signal-group / signal-rebuild /
// signals-changed layout responsibilities. Extracted from the View
// God-class during Phase J of the modernize-view-layer-v3 spec. All
// signal state (_own_signals / _signal_groups / _rebuild_in_progress)
// still lives on View; this class only owns the *behaviour*. View
// declares `friend class ViewSignalSync;` so the delegate can read and
// mutate those private members directly.
class ViewSignalSync {
public:
  explicit ViewSignalSync(View *view) : _view(view) {}

  // -- signal grouping ---------------------------------------------------
  void compute_signal_groups();

  // -- full layout refresh ----------------------------------------------
  void signals_changed(const Trace *eventTrace);

  // -- signal rebuild ----------------------------------------------------
  void rebuild_signals_from_config(const pv::data::SignalConfig &config);
  void rebuild_signals();

  // -- incremental signal-change events --------------------------------
  void on_signals_changed();
  void signals_added_layout();
  void signals_removed_layout();
  void signals_modified_refresh();

  // -- trace access (Phase J additional) --------------------------------
  void get_traces(int type, std::vector<Trace *> &traces);
  static bool compare_trace_v_offsets(const Trace *a, const Trace *b);
  static bool compare_trace_view_index(const Trace *a, const Trace *b);
  static bool compare_trace_y(const Trace *a, const Trace *b);

  // -- layout (Phase J additional) --------------------------------------
  void normalize_layout();
  void zoom_vertical(double steps);
  int headerWidth();

  // -- theme / colors (Phase J additional) ------------------------------
  void UpdateTheme();
  QColor get_group_card_color();
  QColor get_group_card_color(int group_index);
  QColor get_trace_card_color(Trace *trace);
  bool is_colored_card_mode();

private:
  View *_view;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_SIGNAL_SYNC_H
