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

#include "pv/mainwindow/signal_connector.h"
#include "pv/mainwindow/mainwindow.h"

#include <QObject>

#include "pv/mainwindow/dock_manager.h"
#include "pv/mainwindow/tab_manager.h"
#include "pv/mainwindow/status_bar.h"
#include "pv/mainwindow/theme_manager.h"
#include "pv/mainwindow/shortcut_manager.h"

#include "pv/dock/searchdock.h"
#include "pv/dock/dsotriggerdock.h"
#include "pv/dock/protocoldock.h"

#include "pv/toolbars/filebar.h"
#include "pv/toolbars/logobar.h"
#include "pv/toolbars/samplingbar.h"
#include "pv/toolbars/trigbar.h"

#include "pv/view/view.h"

namespace pv {

void MainWindowSignalConnector::setup_connections() {
  // Note: EventObject dead connections removed — all session events are now
  // dispatched via EventBus (see SessionEventDispatcher). The remaining
  // connections below are UI-component signals (View, ToolBar, Dock) that are
  // still actively emitted and NOT part of the EventBus notification system.

  // view
  pv::view::View *initial_view = _wnd->tab_manager()->current_view();
  QObject::connect(initial_view, &view::View::prgRate, _wnd, &MainWindow::prgRate);
  QObject::connect(initial_view, &view::View::auto_trig,
          _wnd->dock_manager()->dso_trigger_widget(),
          &dock::DsoTriggerDock::auto_trig);

  // trig_bar
  QObject::connect(_wnd->trig_bar(), &toolbars::TrigBar::sig_setTheme, _wnd,
          &MainWindow::switchTheme);
  QObject::connect(_wnd->trig_bar(), &toolbars::TrigBar::sig_show_lissajous, initial_view,
          &view::View::show_lissajous);

  // file toolbar
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_load_file, _wnd,
          &MainWindow::on_load_file);
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_save, _wnd,
          &MainWindow::on_save);
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_export, _wnd,
          &MainWindow::on_export);
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_import_file, _wnd,
          &MainWindow::on_import_file);
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_screenShot, _wnd,
          &MainWindow::on_screenShot, Qt::QueuedConnection);
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_load_session, _wnd,
          &MainWindow::on_load_session);
  QObject::connect(_wnd->file_bar(), &toolbars::FileBar::sig_store_session, _wnd,
          &MainWindow::on_store_session);

  // logobar
  QObject::connect(_wnd->logo_bar(), &toolbars::LogoBar::sig_open_doc, _wnd,
          &MainWindow::on_open_doc);

  QObject::connect(_wnd->dock_manager()->protocol_widget(),
          &dock::ProtocolDock::protocol_updated, _wnd,
          &MainWindow::on_signals_changed);

  // SamplingBar
  QObject::connect(_wnd->sampling_bar(), &toolbars::SamplingBar::sig_store_session_data,
          _wnd, &MainWindow::on_save);

  //
  QObject::connect(_wnd->dock_manager()->dso_trigger_widget(),
          &dock::DsoTriggerDock::set_trig_pos, initial_view,
          &view::View::set_trig_pos);

  _wnd->delay_prop_msg_timer().SetCallback(
      std::bind(&MainWindow::on_delay_prop_msg, _wnd));

  _wnd->logo_bar()->set_mainform_callback(_wnd);

  // Bind initial context to docks
  pv::TabContext *initial_ctx = _wnd->tab_manager()->current_context();
  _wnd->sampling_bar()->bind_context(initial_ctx);
  _wnd->dock_manager()->bind_context(initial_ctx);

  _wnd->tab_manager()->setup_connections();

  // FPS / disk cache timers
  QObject::connect(&_wnd->fps_timer(), &QTimer::timeout, _wnd,
          [this]() { _wnd->status_bar()->update_fps(); });
  _wnd->fps_timer().start(1000);

  QObject::connect(&_wnd->disk_cache_status_timer(), &QTimer::timeout, _wnd,
          [this]() { _wnd->status_bar()->update_disk_cache_status(); });
  _wnd->disk_cache_status_timer().start(500);
}

} // namespace pv
