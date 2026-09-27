/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef PXVIEW_PV_MAINWINDOW_WORKSPACE_IO_H
#define PXVIEW_PV_MAINWINDOW_WORKSPACE_IO_H

#include <QJsonObject>
#include <QString>
#include <vector>

namespace pv {

namespace view {
class View;
}

/**
 * One tab's cross-session work state.
 *
 * This is the "tab session layer" carrier. See `AGENT_CONTRACTS.md`
 * ("Workspace (tab session) persistence"): `.pxc` carries the *device profile*
 * (natural key = `(driver, workMode)`) and cannot carry per-tab state — doing so
 * made N tabs of the same device/mode silently overwrite each other.
 */
struct WorkspaceTab {
  QString title;        // tab title (user-renamable)
  QString filePath;     // source file of a file-device tab (.pxl, ...); else empty
  QString driver;       // device driver name; a matching hint / log aid only
  int workMode = 0;     // work mode (LOGIC / DSO / ANALOG / MSO)
  // Exactly SessionDocument::signal_config_to_json(), plus a "uiLayout"
  // sub-object holding the per-tab view density.
  QJsonObject session;
};

struct Workspace {
  static constexpr int kVersion = 1;
  int version = kVersion;
  int activeTab = 0;    // index into `tabs`
  std::vector<WorkspaceTab> tabs;
};

/** Absolute path of workspace.json (under GetProfileDir()). Empty if unavailable. */
QString workspace_file_path();

/**
 * Atomically write the workspace (QSaveFile: temp file + rename).
 * Returns false on any failure; callers keep running (degradation is never fatal).
 */
bool write_workspace_file(const Workspace &ws);

/**
 * Read the workspace. Returns false when the file is missing, unparsable, or the
 * `Version` is unsupported — the caller then keeps today's single-tab behaviour.
 */
bool read_workspace_file(Workspace &out);

/**
 * Per-tab view density (signalHeightScale) lives INSIDE the tab session, not in
 * `.pxc` — capturing / applying it here keeps the JSON layout knowledge in one
 * place so TabManager never assembles raw JSON.
 */
void capture_session_ui_layout(pv::view::View *view, QJsonObject &session);
void apply_session_ui_layout(pv::view::View *view, const QJsonObject &session);

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_WORKSPACE_IO_H
