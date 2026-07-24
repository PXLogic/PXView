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

#ifndef PXVIEW_PV_VIEW_VIEW_CURSORS_H
#define PXVIEW_PV_VIEW_VIEW_CURSORS_H

#include <cstdint>
#include <list>

#include <QColor>
#include <QString>

#include "cursor.h"
#include "xcursor.h"

namespace pv {
namespace view {

class View;

// ViewCursors — delegate for View's cursor / xcursor responsibilities.
// Extracted from the View God-class during Phase E of the
// modernize-view-layer-v2 spec. All cursor state (_logic_cursors /
// _dso_cursors / _trig_cursor / _search_cursor / _show_cursors /
// _show_trig_cursor / _show_search_cursor / _search_pos / _search_hit /
// _xcursorList / _show_xcursors) still lives on View; this class only owns
// the *behaviour*. View declares `friend class ViewCursors;` so the delegate
// can read and mutate those private members directly.
class ViewCursors {
public:
  explicit ViewCursors(View *view) : _view(view) {}

  // -- visibility toggles ------------------------------------------------
  void show_cursors(bool show = true);
  void show_trig_cursor(bool show = true);
  void show_search_cursor(bool show = true);

  // -- trigger / search cursor position ---------------------------------
  void set_trig_cursor_posistion(uint64_t trig_pos);
  void set_search_pos(uint64_t search_pos, bool hit);

  // -- cursor list access ------------------------------------------------
  std::list<Cursor *> &get_cursorList();
  Cursor *get_cursor_by_index(int index);

  // -- cursor CRUD -------------------------------------------------------
  void make_cursors_order();
  void add_cursor(QColor color, uint64_t sampleIndex);
  void add_cursor(uint64_t sampleIndex);
  void del_cursor(Cursor *cursor);
  void clear_cursors();
  void set_cursor_middle(int index);

  // -- xcursor CRUD ------------------------------------------------------
  void add_xcursor(double value0, double value1);
  void del_xcursor(XCursor *xcursor);

  // -- cursor queries ----------------------------------------------------
  uint64_t get_cursor_samples(int index);
  QString get_cm_time(int index);
  QString get_cm_delta(int index1, int index2);
  int get_cursor_index_by_key(uint64_t key);

  // -- Core sync (Task C2.7) --------------------------------------------
  // Write the dragged cursor's new position back to the Core-layer
  // CursorRegistry via DataSource::set_cursor_position. Called by the
  // ruler / viewport drag handlers after TimeMarker::set_index. Accepts
  // TimeMarker* (base of Cursor) since the ruler's _grabbed_marker is
  // typed as TimeMarker*. No-op if the marker is not found in the
  // cursor list (e.g. it is the trig/search cursor which is not tracked
  // in the Core registry) or if the index is out of range on the Core
  // side (e.g. the cursor was created before Core sync was wired up —
  // the position simply does not persist, which is the historical
  // behaviour).
  void sync_cursor_position_to_core(TimeMarker *marker);

  // Reconcile the View's rendering cursor list with the Core-layer
  // CursorRegistry. Creates view::Cursor rendering objects for any Core
  // entries that do not yet have a matching View cursor. Called on
  // data-source binding so cursors added by MCP while headless appear
  // once the View is created (e.g. headless -> GUI transition).
  void sync_cursors_from_core();

private:
  View *_view;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_CURSORS_H
