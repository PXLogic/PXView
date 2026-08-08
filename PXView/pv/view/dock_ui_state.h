/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_VIEW_DOCK_UI_STATE_H
#define PXVIEW_PV_VIEW_DOCK_UI_STATE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <map>
#include <cstdint>

namespace pv {
namespace view {

/**
 * Per-tab UI state cache for dock widgets and the sampling toolbar.
 *
 * Background: dock widgets (TriggerDock, DsoTriggerDock, MeasureDock,
 * SearchDock, ProtocolDock, DeviceOptionsDock) and
 * the SamplingBar are global singletons owned by MainWindow — they are NOT
 * per-tab. When the user switches tabs, each dock unbinds from the old
 * TabContext (saving its current UI state somewhere per-tab) and binds to
 * the new TabContext (restoring the new tab's UI state).
 *
 * Previously these states lived as 13 `_dock_*` public fields on
 * pv::data::SessionDocument (a Core-layer class), which leaked View-layer
 * UI semantics into Core. This struct moves them to the View layer, where
 * they belong. The state is hosted by view::View (which is per-tab and
 * survives tab switches without destruction), accessed by docks/toolbar via
 * `ctx->view()->dock_ui_state()`.
 *
 * NOTE: these fields are in-memory only — they are NOT serialized to the
 * .pxc session file. They exist solely to preserve dock UI state across
 * tab switches within a running session.
 */
struct DockUiState {
  // SamplingBar selections
  uint64_t dock_sample_rate;
  uint64_t dock_sample_limit;
  int dock_collect_mode;

  // SearchDock
  std::map<uint16_t, QString> dock_search_pattern;

  // MeasureDock
  bool dock_measure_fen_enabled;
  QJsonArray dock_measure_dist_rows;
  QJsonArray dock_measure_edge_rows;

  // TriggerDock / DsoTriggerDock / DeviceOptionsDock
  QJsonObject dock_trigger_session;
  QJsonObject dock_dso_trigger_session;
  QJsonObject dock_device_options_session;

  // ProtocolDock
  QString dock_protocol_search_text;
  QJsonArray dock_protocol_expanded_states;

  DockUiState() { reset(); }

  void reset() {
    dock_sample_rate = 0;
    dock_sample_limit = 0;
    dock_collect_mode = 0;
    dock_search_pattern.clear();
    dock_measure_fen_enabled = true;
    dock_measure_dist_rows = QJsonArray();
    dock_measure_edge_rows = QJsonArray();
    dock_trigger_session = QJsonObject();
    dock_dso_trigger_session = QJsonObject();
    dock_device_options_session = QJsonObject();
    dock_protocol_search_text.clear();
    dock_protocol_expanded_states = QJsonArray();
  }
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_DOCK_UI_STATE_H
