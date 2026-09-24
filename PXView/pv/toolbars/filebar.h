/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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


#ifndef PXVIEW_PV_TOOLBARS_FILEBAR_H
#define PXVIEW_PV_TOOLBARS_FILEBAR_H

#include <QToolBar>
#include <QAction>
#include <QMenu>
#include <QVector>

#include <glib.h>

#include "pv/session/sigsession.h" 
#include "pv/interface/icallbacks.h"
#include "pv/ui/xtoolbutton.h"
#include "pv/ui/uimanager.h"

namespace pv {
namespace toolbars {

//toolbar button,referenced by MainWindow
//TODO: load session file, sorte session, load log data file, sorte data, export data
class FileBar : public QToolBar, public IUiWindow
{
    Q_OBJECT

public:
    explicit FileBar(SigSession *session, QWidget *parent = 0);

    ~FileBar();

    void update_view_status();

    QString genDefaultSessionFile();

private:
    void retranslateUi();
    void reStyle(); 

    //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

signals:
    void sig_load_file(QString);
    void sig_save();
    void sig_export();
    // Import a file through a libsigrok input module.
    //   format_id     : empty = detect from the file, else the chosen module id
    //   input_options : the user's option values (sunk GVariant values), or
    //                   nullptr when the module takes none. OWNERSHIP IS
    //                   TRANSFERRED: the receiving slot destroys the table.
    void sig_import_file(QString file_name, QString format_id,
                         GHashTable *input_options);
    void sig_screenShot(); //post screen capture event message
    void sig_load_session(QString); //post load session event message
    void sig_store_session(QString); //post store session event message
    // 命令/通知拆分（2026-09-16）：保存流程的前置提交命令通道。由
    // MainWindow 直连执行采样栏设置提交（保证 sig_store_session 读到
    // 最新值），取代原 StoreConfPrev 事件 —— 异步事件的"前置"保证
    // 名存实亡（提交落在读取之后）。
    void store_conf_pending();

private:
    /* Fill the "Import..." submenu from sr_input_list(): one entry per input
     * module plus the auto-detect entry. Rebuilt only in the constructor —
     * libsigrok's module list is static. */
    void build_import_menu();

    /* Import using one explicitly chosen input module (its option dialog is
     * shown before the import when the module declares options). */
    void on_import_format_triggered(const QString &format_id);

    /* Shared tail of both import entries: ask for the module's options and
     * hand file + format + options to the session (see sig_import_file). */
    void run_import(const QString &file_name, const QString &format_id);

private slots:
    void on_actionLoad_triggered();
    void on_actionStore_triggered();
    void on_actionDefault_triggered();
    void on_actionOpen_triggered();
    void on_actionImport_triggered();
    void on_actionCapture_triggered();

public:
    SigSession *_session;
    data::ISignalSource *_signals = nullptr;
  data::ICaptureControl *_capture = nullptr;

    // XToolButton _file_button;
    QMenu   *_menu;
    QMenu   *_menu_session; //when the hardware device is connected,it will be enable
    QMenu   *_menu_import;  //one entry per libsigrok input module + auto-detect
    QVector<QAction *> _import_format_actions;
    QAction *_action_load;
    QAction *_action_store;
    QAction *_action_default;
    QAction *_action_open;
    QAction *_action_save;
    QAction *_action_export;
    QAction *_action_import;
    QAction *_action_capture;
};

} // namespace toolbars
} // namespace pv

#endif // PXVIEW_PV_TOOLBARS_FILEBAR_H
