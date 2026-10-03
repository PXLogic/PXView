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

#include <memory>

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
#include "pv/session/storesession.h"
#include "pv/core/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/utility/path.h"

#include "pv/core/documentregistry.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/dialogs/storeprogress.h"
#include "pv/dock/protocoldock.h"
#include "pv/view/view.h"

namespace pv {

void MainWindowFileOps::on_load_file(QString file_name) {
  // Rebind model: one file = one data pool slot. Opening an already-open
  // file routes to its existing tab instead of creating a second virtual
  // device + slot for the same file.
  for (pv::TabContext *ctx0 : _wnd->tab_manager()->contexts()) {
    if (QString::compare(ctx0->file_path(), file_name,
                         Qt::CaseInsensitive) == 0) {
      int idx0 = _wnd->tab_manager()->contexts().indexOf(ctx0);
      if (idx0 >= 0)
        _wnd->tab_manager()->tab_widget()->setCurrentIndex(idx0);
      return;
    }
  }

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
    // Rebind model: register this document as the device-keyed data pool slot
    // of the .pxl file device (snapshots + decoder stacks survive switches).
    new_doc->set_file_device_slot(true);
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

bool MainWindowFileOps::reload_file_into_context(pv::TabContext *ctx) {
  if (!ctx || ctx->file_path().isEmpty())
    return false;

  const QString path = ctx->file_path();
  QFileInfo fi(path);
  if (!fi.exists()) {
    pxv_warn("reload_file_into_context: '%s' no longer exists — tab keeps its "
             "config but will not be replayed as a file device",
             path.toUtf8().constData());
    ctx->set_file_path(QString());  // 不再重试
    return false;
  }

  try {
    // A tab whose data came from an *input module* (VCD/CSV/binary/Saleae/...)
    // must be replayed through import_file(), NOT set_file(): set_file() only
    // understands native .pxl session files (sr_session_load_file_device) and
    // fails on a VCD header, so the tab silently stayed empty on restore.
    // Native .pxl session files keep the original set_file() path.
    const bool ok = ctx->is_imported_file()
                        ? _wnd->session()->import_file(path, ctx->import_format(),
                                                       nullptr)
                        : _wnd->session()->set_file(path);
    if (!ok) {
      QString strMsg(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
      strMsg += path;
      MsgBox::Show(strMsg);
      ctx->set_file_path(QString());  // 避免每次切到该 tab 都重试一次失败加载
      return false;
    }

    // 同 on_load_file：set_file() 建出的虚拟设备属于本 tab，记录 handle 供
    // activate() 日后恢复；文档自描述（关闭 tab 时释放自己的设备）。
    ctx->set_device_handle(_wnd->session()->get_device()->handle());
    if (ctx->document()) {
      ctx->document()->set_device_handle(ctx->device_handle());
      ctx->document()->set_file_device_slot(true);
    }
    ctx->make_live();
    ctx->activate();
    _wnd->update_tab_style(_wnd->tab_manager()->contexts().indexOf(ctx));

    // Re-apply the decoder (protocol analyzer) stacks that were open on this tab
    // before the restart.
    //
    // The gate is "does this file carry its own decoders?", NOT "was it
    // imported?". A .pxl written by Save embeds a "decoders" member and
    // set_file() replays it — re-applying the pending list would double-add.
    // But an Open-path .sr archive has no such member: it replays through
    // set_file() (virtual-session) just like .pxl, yet carries no analyzer
    // config, so it needs the pending list exactly like a VCD does. Gating on
    // is_imported_file() silently dropped every .sr tab's decoders.
    //
    // Channels exist only after the replay above, which is why this runs here
    // and not at restore time (contract "File-device tabs restore lazily").
    if (ctx->has_pending_decoders() && !ctx->file_has_embedded_decoders()) {
      StoreSession ss(_wnd->session());
      auto *dock = _wnd->dock_manager()->protocol_widget();
      // The decoders must be filed under THIS tab's document, and read back
      // from the same one. Two things must agree for that:
      //   1. the StoreSession read-back document (set_decoder_doc below), and
      //   2. the View that actually files the stack (named per-call in the
      //      callback — see the comment there). The ProtocolDock is shared, so
      //      its own `_view` is NOT a safe source for this tab's document.
      ss.set_decoder_doc(ctx->document());

      QJsonArray decoders = ctx->pending_decoders();
      // Name THIS tab's View explicitly. The ProtocolDock is shared across
      // tabs and its own `_view` is only swapped by bind_context() on tab
      // switch; this restore runs during the tab change, so the dock can still
      // be bound to another tab. Routing through the dock's `_view` then filed
      // the new DecoderStack under the OTHER tab's document, and this tab read
      // back an empty list.
      view::View *const tab_view = ctx->view();
      ss.load_decoders(
          [dock, tab_view](const QString &id, bool stacked_ok,
                           std::list<pv::data::decode::Decoder *> &subs) {
            return dock->add_protocol_by_id(id, stacked_ok, subs, tab_view);
          },
          decoders);
      ctx->clear_pending_decoders();
      if (auto *v = ctx->view())
        v->update_all_trace_postion();
    }

    pxv_info("reload_file_into_context: reopened '%s' for tab '%s'",
             path.toUtf8().constData(), ctx->title().toUtf8().constData());
    return true;
  } catch (QString e) {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
    strMsg += path;
    MsgBox::Show(strMsg);
    _wnd->session()->set_default_device();
    ctx->set_file_path(QString());
    return false;
  }
}

void MainWindowFileOps::on_import_file(QString file_name, QString format_id,
                                       GHashTable *input_options) {
  // The option table arrives with transferred ownership, but this function has
  // several early returns (duplicate tab, failed import, exception) and the
  // session only borrows it — so hold it in a guard rather than freeing it by
  // hand on each path.
  std::unique_ptr<GHashTable, decltype(&g_hash_table_destroy)> options_guard(
      input_options, &g_hash_table_destroy);

  // Rebind model: one file = one data pool slot (same dedup as on_load_file).
  for (pv::TabContext *ctx0 : _wnd->tab_manager()->contexts()) {
    if (QString::compare(ctx0->file_path(), file_name,
                         Qt::CaseInsensitive) == 0) {
      int idx0 = _wnd->tab_manager()->contexts().indexOf(ctx0);
      if (idx0 >= 0)
        _wnd->tab_manager()->tab_widget()->setCurrentIndex(idx0);
      return;
    }
  }

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
  // Mark the loader: this tab's data comes from an input module (VCD/CSV/...),
  // NOT from a native .pxl session. Workspace restore must replay it through
  // import_file() — set_file() cannot read an input-module file.
  ctx->set_imported_file(true);
  ctx->set_import_format(format_id);

  _wnd->add_tab(ctx);

  try {
    // Import external data file using libsigrok input modules
    // (VCD, CSV, binary, Saleae, etc.) — aligned with PulseView.
    // format_id comes from the toolbar's import menu (empty = auto-detect) and
    // input_options carries the module options the user confirmed.
    if (!_wnd->session()->import_file(
            file_name, format_id, options_guard.get())) {
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
    // Rebind model: register this document as the device-keyed data pool slot
    // of the imported file device (snapshots survive switches, no re-import).
    new_doc->set_file_device_slot(true);
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
