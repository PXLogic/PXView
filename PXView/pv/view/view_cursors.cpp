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

// Phase E (modernize-view-layer-v2): cursor / xcursor behaviour extracted
// from the View God-class. Since Phase 1 state migration, cursor state
// lives on ViewCursors directly (not on View). Cross-method calls that
// remain on View (e.g. set_scale_offset, get_view_width,
// document_snapshot_source) go through _view->… so the public View API is
// unchanged.

#include "view_cursors.h"

#include <cassert>
#include <list>

#include <QPalette>
#include <QWidget>

#include "view.h"
#include "ruler.h"

#include "../config/appconfig.h"
#include "../data/datasource.h"
#include "../data/signalmodel.h"
#include "../deviceagent.h"
#include "../sigsession.h"

using namespace std;

namespace pv {
namespace view {

ViewCursors::ViewCursors(View *view) : _view(view) {}

ViewCursors::~ViewCursors() {
  // unique_ptr containers auto-delete all elements.
  // _trig_cursor and _search_cursor also auto-deleted by unique_ptr.
}

void ViewCursors::init_cursors(QColor foreColor) {
  _show_trig_cursor = false;
  _trig_cursor = std::make_unique<Cursor>(*_view, -1, 0);
  _trig_cursor->set_colour(View::LightRed);
  _show_search_cursor = false;
  _search_pos = 0;
  _search_cursor = std::make_unique<Cursor>(*_view, -1, _search_pos);
  _search_cursor->set_colour(foreColor);
}

void ViewCursors::show_cursors(bool show) {
  _show_cursors = show;
  _view->get_ruler()->update();
  _view->viewport_update();
}

void ViewCursors::show_trig_cursor(bool show) {
  _show_trig_cursor = show;
  _view->get_ruler()->update();
  _view->viewport_update();
}

void ViewCursors::show_search_cursor(bool show) {
  _show_search_cursor = show;
  _view->get_ruler()->update();
  _view->viewport_update();
}

void ViewCursors::set_trig_cursor_posistion(uint64_t trig_pos) {
  const double time =
      trig_pos * 1.0 / _view->document_snapshot_source()->cur_snap_samplerate();
  _trig_cursor->set_index(trig_pos);

  int width = _view->get_view_width();
  assert(width > 0);

  // B2 fix: query Core trigger state instead of ds_trigger_get_en().
  // Trigger is enabled if any logic channel has a non-NONTRIG trig_type
  // (Simple mode), or if trigger_config mode is Adv/Serial (always enabled).
  bool trigger_enabled = false;
  const auto &trig_cfg = _view->data_source()->trigger_config();
  if (trig_cfg.mode() != pv::data::TriggerConfig::Simple) {
    trigger_enabled = true;
  } else {
    for (const auto &m : _view->data_source()->get_signal_models()) {
      if (m && m->type() == SR_CHANNEL_LOGIC &&
          m->trig_type() != pv::data::SignalModel::NONTRIG) {
        trigger_enabled = true;
        break;
      }
    }
  }

  AppConfig &app = AppConfig::Instance();

  if (trigger_enabled || _view->device_agent()->is_virtual() ||
      _view->get_work_mode() == DSO) {
    _show_trig_cursor = true;

    // DSO 持续采集时每帧都会调用 set_trig_cursor_posistion() (经由
    // receive_trigger -> ViewDataSync::receive_trigger)。如果此处也调用
    // set_scale_offset() 强制居中到触发位置,会覆盖用户的水平滚动操作,
    // 表现为水平滑动条被强制移动到最右端 (DSO 触发位置靠近帧末尾时,
    // (time/scale - width/2) 会被 clamp 到 get_max_offset())。
    // 修复: DSO 模式下仅更新光标可视位置,不自动滚动。用户主动设置
    // 触发位置 (View::set_trig_pos, 仅在采集前调用) 仍会通过 LOGIC/ANALOG
    // 路径或首次进入 DSO 时由其他逻辑处理居中。
    if (app.appOptions.trigPosDisplayInMid &&
        _view->get_work_mode() != DSO) {
      _view->set_scale_offset(_view->scale(), (time / _view->scale()) - (width / 2));
    }
  }

  _view->get_ruler()->update();
  _view->viewport_update();
}

void ViewCursors::set_search_pos(uint64_t search_pos, bool hit) {
  QColor fore(_view->palette().color(_view->foregroundRole()));
  fore.setAlpha(View::BackAlpha);

  const double time =
      search_pos * 1.0 / _view->document_snapshot_source()->cur_snap_samplerate();
  _search_pos = search_pos;
  _search_hit = hit;
  _search_cursor->set_index(search_pos);
  _search_cursor->set_colour(hit ? View::Blue : fore);

  int width = _view->get_view_width();
  assert(width);

  if (hit) {
    _view->set_scale_offset(_view->scale(), (time / _view->scale()) - (width / 2));
    _view->get_ruler()->update();
    _view->viewport_update();
  }
}

std::list<std::unique_ptr<Cursor>> &ViewCursors::get_cursorList() {
  if (_view->is_logic_rendering_mode()) {
    return _logic_cursors;
  } else {
    return _dso_cursors;
  }
}

Cursor *ViewCursors::get_cursor_by_index(int index) {
  int dex = 0;
  auto &cursors = get_cursorList();

  for (auto &c : cursors) {
    if (dex == index) {
      return c.get();
    }
    dex++;
  }
  return nullptr;
}

void ViewCursors::make_cursors_order() {
  int dex = 1;

  for (auto &cursor : get_cursorList()) {
    cursor->set_order(dex++);
  }

  dex = 1;
  for (auto &cursor : _view->get_xcursorList()) {
    cursor->set_order(dex++);
  }
}

void ViewCursors::add_cursor(QColor color, uint64_t sampleIndex) {
  (void)color;
  auto newCursor = std::make_unique<Cursor>(*_view, -1, sampleIndex);
  get_cursorList().push_back(std::move(newCursor));
  make_cursors_order();
  _view->cursor_update();
}

void ViewCursors::add_cursor(uint64_t sampleIndex) {
  static int lastOrder = 1;
  auto newCursor = std::make_unique<Cursor>(*_view, lastOrder++, sampleIndex);
  get_cursorList().push_back(std::move(newCursor));
  make_cursors_order();
  _view->cursor_update();
}

void ViewCursors::del_cursor(Cursor *cursor) {
  assert(cursor);

  auto &lst = get_cursorList();
  for (auto it = lst.begin(); it != lst.end(); ++it) {
    if (it->get() == cursor) {
      lst.erase(it);
      break;
    }
  }
  make_cursors_order();

  _view->cursor_update();
}

void ViewCursors::clear_cursors() {
  auto &lst = get_cursorList();
  // unique_ptr elements are auto-deleted when the list is cleared.
  lst.clear();
}

void ViewCursors::set_cursor_middle(int index) {
  auto &lst = get_cursorList();
  int size = lst.size();
  if (index < 0 || index >= size) {
    pxv_warn("ViewCursors::set_cursor_middle: index %d out of range (size=%d)",
             index, size);
    return;
  }

  int width = _view->get_view_width();

  auto i = lst.begin();

  while (index-- != 0) {
    i++;
  }

  _view->set_scale_offset(
      _view->scale(),
      (*i)->index() /
          (_view->document_snapshot_source()->cur_snap_samplerate() *
           _view->scale()) -
          (width / 2));
}

void ViewCursors::add_xcursor(double value0, double value1) {
  static int lastXCursorOrder = 1;
  auto newXCursor = std::make_unique<XCursor>(*_view, lastXCursorOrder++, value0, value1);
  _xcursorList.push_back(std::move(newXCursor));
  make_cursors_order();
  _view->xcursor_update();
}

void ViewCursors::del_xcursor(XCursor *xcursor) {
  assert(xcursor);

  for (auto it = _xcursorList.begin(); it != _xcursorList.end(); ++it) {
    if (it->get() == xcursor) {
      _xcursorList.erase(it);
      break;
    }
  }
  make_cursors_order();
  _view->xcursor_update();
}

uint64_t ViewCursors::get_cursor_samples(int index) {
  auto &lst = get_cursorList();
  if (index < 0 || index >= (int)lst.size()) {
    pxv_warn("ViewCursors::get_cursor_samples: index %d out of range (size=%d)",
             index, (int)lst.size());
    return 0;
  }

  uint64_t ret = 0;
  int curIndex = 0;
  for (auto i = lst.begin(); i != lst.end(); i++) {
    if (index == curIndex) {
      ret = (*i)->index();
    }
    curIndex++;
  }
  return ret;
}

QString ViewCursors::get_cm_time(int index) {
  uint64_t sampleIndex = get_cursor_samples(index);
  uint64_t sampleRate = _view->document_snapshot_source()->cur_snap_samplerate();
  return _view->get_ruler()->format_real_time(sampleIndex, sampleRate);
}

QString ViewCursors::get_cm_delta(int index1, int index2) {
  if (index1 == index2)
    return "0";

  uint64_t samples1 = get_cursor_samples(index1);
  uint64_t samples2 = get_cursor_samples(index2);
  uint64_t delta_sample =
      (samples1 > samples2) ? samples1 - samples2 : samples2 - samples1;
  return _view->get_ruler()->format_real_time(
      delta_sample, _view->document_snapshot_source()->cur_snap_samplerate());
}

int ViewCursors::get_cursor_index_by_key(uint64_t key) {
  auto &lst = get_cursorList();

  int dex = 0;
  for (auto &c : lst) {
    if (c->get_key() == key) {
      return dex;
    }
    ++dex;
  }
  return -1;
}

// Task C2.7: write the dragged cursor's new position back to the Core-layer
// CursorRegistry. The positional index of the cursor in the View's rendering
// list is used as the Core registry index — these stay in sync as long as
// cursors are added/removed through the same path (ViewCursors::add_cursor /
// del_cursor, which the ruler invokes). If the index is out of range on the
// Core side (e.g. the cursor was added before Core sync was wired up),
// set_cursor_position returns false and we silently drop the write — this
// matches the historical behaviour where cursor positions were View-only.
void ViewCursors::sync_cursor_position_to_core(TimeMarker *marker) {
  if (!marker || !_view->data_source())
    return;

  // Find the positional index of the marker in the cursor list by pointer
  // identity. This covers Cursor markers dragged via the ruler; trig/search
  // cursors are not in the list and are silently skipped (they are not
  // tracked in the Core CursorRegistry).
  auto &lst = get_cursorList();
  int idx = 0;
  bool found = false;
  for (auto &c : lst) {
    if (c.get() == marker) { found = true; break; }
    ++idx;
  }
  if (!found)
    return;

  _view->data_source()->set_cursor_position(idx, marker->index());
}

// Task C2.7: reconcile the View's rendering cursor list with the Core-layer
// CursorRegistry. Creates view::Cursor rendering objects for any Core entries
// that do not yet have a matching View cursor (by positional index). Called
// on data-source binding so cursors added by MCP while headless appear once
// the View is created. Existing View cursors that already match a Core entry
// by index are left untouched (their position is not overwritten — the user
// may have dragged them since).
void ViewCursors::sync_cursors_from_core() {
  if (!_view->data_source())
    return;

  auto core_entries = _view->data_source()->get_cursors();
  auto &view_cursors = get_cursorList();

  int core_count = static_cast<int>(core_entries.size());
  int view_count = static_cast<int>(view_cursors.size());

  // If the View already has at least as many cursors as Core, assume they
  // are in sync (positional index correspondence). This is the common case
  // after the initial binding.
  if (view_count >= core_count)
    return;

  // Create rendering objects for the trailing Core entries that have no
  // matching View cursor. Use add_cursor(uint64_t) which assigns the next
  // _order and triggers cursor_update().
  for (int i = view_count; i < core_count; ++i) {
    add_cursor(core_entries[i].sample_position);
  }
}

} // namespace view
} // namespace pv
