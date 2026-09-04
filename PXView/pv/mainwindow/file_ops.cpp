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

#include "pv/mainwindow/file_ops.h"
#include "pv/mainwindow/mainwindow.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPixmap>

#include "pv/config/appconfig.h"
#include "pv/session/deviceagent.h"
#include "pv/base/log.h"
#include "pv/mainwindow/mainframe.h"

// The Windows SDK (pulled in transitively via mainframe.h -> wintaskbarprogress.h
// -> shobjidl.h) defines `interface` as a preprocessor macro. This corrupts
// `pv::interface::IEventListener` in sigsession.h, making SigSession appear as
// an incomplete type. Clear it so qualified names parse correctly.
#ifdef interface
#  undef interface
#endif

#include "pv/mainwindow/config_io.h"
#include "pv/mainwindow/dock_manager.h"
#include "pv/mainwindow/tab_manager.h"
#include "pv/base/pxvdef.h"
#include "pv/session/sessionmanager.h"
#include "pv/session/sigsession.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/utility/path.h"

#include "pv/core/documentregistry.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/dialogs/storeprogress.h"
#include "pv/view/view.h"

namespace pv {

void MainWindowFileOps::on_load_file(QString file_name) {
  pv::view::View *new_view = new pv::view::View(_wnd->session(), _wnd->sampling_bar(), _wnd);
  // phase 2: document owned by DocumentRegistry.
  size_t new_doc_idx = _wnd->session()->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_wnd->session()->device()));
  pv::data::SessionDocument *new_doc =
      _wnd->session()->document_registry()->get_document_by_index(new_doc_idx);
  pv::TabContext *ctx =
      SessionManager::instance()->create_context(new_view, _wnd->session(), new_doc,
                                                 new_doc_idx,
                                                 _wnd->session()->document_registry());

  QFileInfo fi(file_name);
  ctx->set_title(fi.baseName());
  ctx->set_file_path(file_name);

  _wnd->add_tab(ctx);

  try {
    if (_wnd->device_agent()->is_hardware()) {
      _wnd->save_config();
    }

    // 架构修复：检查 set_file 返回值，失败时不创建空白 tab
    if (!_wnd->session()->set_file(file_name)) {
      QString strMsg(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
      strMsg += file_name;
      MsgBox::Show(strMsg);
      // 回滚已创建的 tab
      int idx = _wnd->tab_manager()->contexts().indexOf(ctx);
      if (idx >= 0)
        _wnd->remove_tab(idx);
      _wnd->session()->set_default_device();
      return;
    }
    // The virtual device created by set_file() belongs to THIS tab. Record it
    // so activate() can restore it when the user tabs away and back — the
    // global DeviceAgent can only hold one active device.
    ctx->set_device_handle(_wnd->session()->get_device()->handle());
    // Mirror the handle onto the document so the document is self-describing:
    // closing the tab releases exactly its own device (see TabManager::remove_tab).
    new_doc->set_device_handle(ctx->device_handle());
    ctx->make_live();
    ctx->activate();
    _wnd->update_tab_style(_wnd->tab_manager()->contexts().indexOf(ctx));
  } catch (QString e) {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
    strMsg += file_name;
    MsgBox::Show(strMsg);
    _wnd->session()->set_default_device();
  }
}

void MainWindowFileOps::on_import_file(QString file_name) {
  pv::view::View *new_view = new pv::view::View(_wnd->session(), _wnd->sampling_bar(), _wnd);
  // phase 2: document owned by DocumentRegistry.
  size_t new_doc_idx = _wnd->session()->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_wnd->session()->device()));
  pv::data::SessionDocument *new_doc =
      _wnd->session()->document_registry()->get_document_by_index(new_doc_idx);
  pv::TabContext *ctx =
      SessionManager::instance()->create_context(new_view, _wnd->session(), new_doc,
                                                 new_doc_idx,
                                                 _wnd->session()->document_registry());

  QFileInfo fi(file_name);
  ctx->set_title(fi.baseName());
  ctx->set_file_path(file_name);

  _wnd->add_tab(ctx);

  try {
    // Import external data file using libsigrok input modules
    // (VCD, CSV, binary, Saleae, etc.) — aligned with PulseView.
    if (!_wnd->session()->import_file(file_name)) {
      QString strMsg(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
      strMsg += file_name;
      MsgBox::Show(strMsg);
      // 回滚已创建的 tab
      int idx = _wnd->tab_manager()->contexts().indexOf(ctx);
      if (idx >= 0)
        _wnd->remove_tab(idx);
      _wnd->session()->set_default_device();
      return;
    }
    // Same as on_load_file: the input-module device created by import_file()
    // belongs to THIS tab and must be restorable across tab switches.
    ctx->set_device_handle(_wnd->session()->get_device()->handle());
    // Mirror the handle onto the document so the document is self-describing:
    // closing the tab releases exactly its own device (see TabManager::remove_tab).
    new_doc->set_device_handle(ctx->device_handle());
    ctx->make_live();
    ctx->activate();
    _wnd->update_tab_style(_wnd->tab_manager()->contexts().indexOf(ctx));
  } catch (QString e) {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
    strMsg += file_name;
    MsgBox::Show(strMsg);
    _wnd->session()->set_default_device();
  }
}

void MainWindowFileOps::on_save() {
  using pv::dialogs::StoreProgress;

  if (_wnd->device_agent()->have_instance() == false) {
    pxv_info("Have no device, can't to save data.");
    return;
  }

  if (_wnd->session()->is_working()) {
    pxv_info("Save data: stop the current device.");
    _wnd->session()->stop_capture();
  }

  _wnd->session()->set_saving(true);

  StoreProgress *dlg = new StoreProgress(_wnd->session(), _wnd);
  dlg->SetView(_wnd->current_view());
  dlg->save_run(_wnd);
}

void MainWindowFileOps::on_export() {
  using pv::dialogs::StoreProgress;

  if (_wnd->session()->is_working()) {
    pxv_info("Export data: stop the current device.");
    _wnd->session()->stop_capture();
  }

  StoreProgress *dlg = new StoreProgress(_wnd->session(), _wnd);
  dlg->SetView(_wnd->current_view());
  dlg->export_run();
}

void MainWindowFileOps::on_screenShot() {
  AppConfig &app = AppConfig::Instance();
  QString default_name =
      app.userHistory.screenShotPath + "/" + APP_NAME +
      QDateTime::currentDateTime().toString("-yyMMdd-hhmmss");

  int x = _wnd->parentWidget()->pos().x();
  int y = _wnd->parentWidget()->pos().y();
  int w = _wnd->parentWidget()->frameGeometry().width();
  int h = _wnd->parentWidget()->frameGeometry().height();

  (void)h;
  (void)w;
  (void)x;
  (void)y;

#ifdef _WIN32
  QPixmap pixmap = _wnd->parentWidget()->grab();
#elif __APPLE__
  x += MainFrame::Margin;
  y += MainFrame::Margin;
  w -= MainFrame::Margin * 2;
  h -= MainFrame::Margin * 2;

  QPixmap pixmap =
      QGuiApplication::primaryScreen()->grabWindow(_wnd->winId(), x, y, w, h);
#else
  QPixmap pixmap = _wnd->parentWidget()->grab();
#endif

  QString format = "png";
  QString fileName = QFileDialog::getSaveFileName(
      _wnd, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAVE_AS), "Save As"), default_name,
      "png file(*.png);;jpeg file(*.jpeg)", &format);

  if (!fileName.isEmpty()) {
    QStringList list = format.split('.').last().split(')');
    QString suffix = list.first();

    QFileInfo f(fileName);
    if (f.suffix().compare(suffix)) {
      fileName += "." + suffix;
    }

    pixmap.save(fileName, suffix.toLatin1());

    fileName = path::GetDirectoryName(fileName);

    if (app.userHistory.screenShotPath != fileName) {
      app.userHistory.screenShotPath = fileName;
      app.SaveHistory();
    }
  }
}

} // namespace pv
