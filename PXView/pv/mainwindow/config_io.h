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

#ifndef PXVIEW_PV_MAINWINDOW_CONFIG_IO_H
#define PXVIEW_PV_MAINWINDOW_CONFIG_IO_H

#include <QJsonObject>
#include <QJsonDocument>
#include <QString>

// Forward declarations
class DeviceAgent;

namespace pv {
class SigSession;
class MainWindow;

/**
 * MainWindowConfigIO — delegate for MainWindow's session config I/O.
 *
 * Extracted from the MainWindow God-class during Phase 2 of the view-layer
 * modernization. All JSON config save/load logic (~1000 lines) lives here.
 * MainWindow declares `friend class MainWindowConfigIO;` so this delegate
 * can access its private members (_device_agent, _session, _protocol_widget,
 * current_view(), etc.) directly, just as the code did when it was inline.
 *
 * The delegate is owned by MainWindow via unique_ptr and constructed in
 * MainWindow's constructor. All methods are called from MainWindow's own
 * methods (save_config, load_config_from_file, etc.) which now forward here.
 */
class MainWindowConfigIO {
public:
  explicit MainWindowConfigIO(MainWindow *wnd) : _wnd(wnd) {}

  // ---- Config file path ----
  QString gen_config_file_path(bool isNewFormat);

  // ---- Save (serialize to JSON) ----
  bool gen_config_json(QJsonObject &sessionVar);
  void save_config();
  bool save_config_to_file(QString file);
  bool genSessionData(std::string &str);

  // ---- Load (deserialize from JSON) ----
  bool load_config_from_file(QString file);
  bool load_config_from_json(QJsonDocument &doc, bool &haveDecoder);
  void load_device_config();
  void check_config_file_version();
  void load_demo_decoder_config(QString optname);

  // ---- Data file embedded config ----
  QJsonDocument get_config_json_from_data_file(QString file, bool &bSuccess);
  QJsonArray get_decoder_json_from_data_file(QString file, bool &bSuccess);

private:
  MainWindow *_wnd;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_CONFIG_IO_H
