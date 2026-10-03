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

/**
 * One tab's cross-session work state.
 *
 * This is the "tab session layer" carrier. See `AGENT_CONTRACTS.md`
 * ("Workspace (tab session) persistence"): `.pxc` carries the *device profile*
 * (natural key = `(driver, workMode)`) and cannot carry per-tab state — doing so
 * made N tabs of the same device/mode silently overwrite each other.
 *
 * Deliberately free of any View/widget dependency so it can be unit-tested
 * on its own; the per-tab view density helpers live in `tab_manager.cpp`.
 */
struct WorkspaceTab {
  QString title;        // tab title (user-renamable)
  QString filePath;     // source file of a file-device tab (.pxl, ...); else empty
  // Which loader `filePath` needs on restore. false = native .pxl session file
  // (SigSession::set_file); true = input-module import such as VCD/CSV/binary
  // (SigSession::import_file). The two are not interchangeable — using
  // set_file() on a VCD file fails, which is why the tab appeared never to
  // reload. `importFormat` remembers an explicitly chosen module id (empty =
  // auto-detect) for formats whose extension cannot decide (e.g. ".bin").
  bool isImportedFile = false;
  QString importFormat;
  // Device identity. `ds_device_handle` is process-local and never persisted;
  // `(driver, connid)` is the stable cross-session key (see
  // SigSession::resolve_device_handle_by_identity).
  QString driver;
  QString connid;
  int workMode = 0;     // work mode (LOGIC / DSO / ANALOG / MSO)
  // File-device tabs are NOT replayed at startup (reading a .pxl triggers the
  // whole capture/replay pipeline); they are reopened lazily on first switch.
  bool isFileDevice = false;
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
 * Test hook: override the workspace path (empty string restores the default
 * GetProfileDir() location). Without this a unit test would write into — and
 * read back from — the real user profile directory.
 */
void set_workspace_path_override(const QString &path);

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

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_WORKSPACE_IO_H
