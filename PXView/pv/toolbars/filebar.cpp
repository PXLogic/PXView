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

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>   
#include <QMetaObject>
#include <QFileDialog> 
#include <deque>
#include <QApplication>

#include "pv/toolbars/filebar.h"
#include "pv/ui/msgbox.h"
#include "pv/ui/dockfonts.h"
#include "pv/config/appconfig.h"
#include "pv/utility/path.h"
#include "pv/core/langresource.h"
#include "pv/base/log.h"
#include "pv/ui/fn.h"
#include "pv/ui/iconcache.h"
#include "pv/data/sr_options.h"
#include "pv/dialogs/inputoutputoptions.h"
#include <libsigrok/libsigrok.h>
#include <QFileInfo>
#include <QStringList>


namespace pv {
namespace toolbars {

FileBar::FileBar(SigSession *session, QWidget *parent) :
    QToolBar("File Bar", parent),
    _session(session), _signals(session), _capture(session)
    // _file_button(this)
{
    setMovable(false);
    setContentsMargins(0,0,0,0);

    _action_load = new QAction(this);
    _action_load->setObjectName(QString::fromUtf8("actionLoad"));
 
    _action_store = new QAction(this);
    _action_store->setObjectName(QString::fromUtf8("actionStore"));
 
    _action_default = new QAction(this);
    _action_default->setObjectName(QString::fromUtf8("actionDefault"));
  
    //second level menu
    _menu_session = new QMenu(this);
    _menu_session->setObjectName(QString::fromUtf8("menuSession"));
    _menu_session->addAction(_action_load);
    _menu_session->addAction(_action_store);
    _menu_session->addAction(_action_default);

    _action_open = new QAction(this);
    _action_open->setObjectName(QString::fromUtf8("actionOpen"));
    
    _action_save = new QAction(this);
    _action_save->setObjectName(QString::fromUtf8("actionSave"));
     
    _action_export = new QAction(this);
    _action_export->setObjectName(QString::fromUtf8("actionExport"));

    _action_import = new QAction(this);
    _action_import->setObjectName(QString::fromUtf8("actionImport"));
     
    _action_capture = new QAction(this);
    _action_capture->setObjectName(QString::fromUtf8("actionCapture"));
 
    // _file_button.setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    // _file_button.setPopupMode(QToolButton::InstantPopup);

    // Import lives in a submenu: one entry per libsigrok input module plus the
    // auto-detect entry. Picking a format explicitly is the only way to import
    // a headerless file whose extension another module also claims (e.g. ".bin"
    // vs Saleae) — and it is what makes the module's options dialog possible.
    _menu_import = new QMenu(this);
    _menu_import->setObjectName(QString::fromUtf8("menuImport"));

    _menu = new QMenu(this);
    _menu->addMenu(_menu_session);
    _menu->addAction(_action_open);
    _menu->addAction(_action_save);
    _menu->addAction(_action_export);
    _menu->addMenu(_menu_import);
    _menu->addAction(_action_capture);

    build_import_menu();
    // _file_button.setMenu(_menu);
    // addWidget(&_file_button);

    connect(_action_load, &QAction::triggered, this, &FileBar::on_actionLoad_triggered);
    connect(_action_store, &QAction::triggered, this, &FileBar::on_actionStore_triggered);
    connect(_action_default, &QAction::triggered, this, &FileBar::on_actionDefault_triggered);
    connect(_action_open, &QAction::triggered, this, &FileBar::on_actionOpen_triggered);
    connect(_action_save, &QAction::triggered, this, &FileBar::sig_save);
    connect(_action_export, &QAction::triggered, this, &FileBar::sig_export);
    connect(_action_import, &QAction::triggered, this, &FileBar::on_actionImport_triggered);
    connect(_action_capture, &QAction::triggered, this, &FileBar::on_actionCapture_triggered);

    ADD_UI(this);
}

FileBar::~FileBar()
{
    REMOVE_UI(this);
}

void FileBar::retranslateUi()
{
    // _file_button.setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE), "File"));
    _menu_session->setTitle(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_CONFIG), "Con&fig...")); //load,save session file
    _action_load->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_CONFIG_LOAD), "&Load..."));
    _action_store->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_CONFIG_STORE), "S&tore..."));
    _action_default->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_CONFIG_DEFAULT), "&Default..."));
    _action_open->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_OPEN), "&Open..."));
    _action_save->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_SAVE), "&Save..."));
    _action_export->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_EXPORT), "&Export..."));
    // Submenu title keeps the historic id; the first entry is the old
    // single-action behaviour (module detected from the file content).
    _menu_import->setTitle(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_IMPORT), "&Import..."));
    // This action is the title bar's import entry: it opens the system file
    // dialog right away (all formats in one filter, module detected from the
    // file), keeping the historic "导入" wording. Inside the submenu it doubles
    // as the "auto detect" entry.
    _action_import->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_IMPORT),
                                "&Import..."));
    _action_capture->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE_CAPTURE), "&Capture..."));
}

void FileBar::reStyle()
{
    QString iconPath = GetIconPath();
    QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");

    auto getIcon = [&](const QString &name) {
        return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor)
                                   : IconCache::Instance().icon(iconPath + name);
    };

    _action_load->setIcon(getIcon("/open.svg"));
    _action_store->setIcon(getIcon("/save.svg"));
    _action_default->setIcon(getIcon("/gear.svg"));
    _menu_session->setIcon(getIcon("/gear.svg"));
    _action_open->setIcon(getIcon("/open.svg"));
    _action_save->setIcon(getIcon("/save.svg"));
    _action_export->setIcon(getIcon("/export.svg"));
    _action_import->setIcon(getIcon("/import.svg"));
    _menu_import->setIcon(getIcon("/import.svg"));
    _action_capture->setIcon(getIcon("/capture.svg"));
    // _file_button.setIcon(QIcon(iconPath+"/file.svg"));
}

void FileBar::on_actionOpen_triggered()
{
    //open data file
    AppConfig &app = AppConfig::Instance(); 

    // 多 tab 架构：打开文件创建新 tab，旧 tab 数据保留在文档中。
    // 旧版单 tab 设计会提示是否保存（打开文件后数据被替换），
    // 当前多 tab 下此提示是误导性的（用户以为数据会丢，实际不会）。
    // 因此移除保存提示，直接弹出文件选择对话框。

    // Show the dialog.
    // The loader behind this entry is sr_session_load_file_device() (see
    // SigSession::set_file()), which understands BOTH containers:
    //   * PXView's own data file (.pxl, chunks L-<ch>/<n>), and
    //   * upstream sigrok session archives (.sr / .srzip, version/metadata +
    //     logic-N — what libsigrok's srzip output module writes).
    // Only *.pxl used to be offered here, so an archive coming from sigrok or
    // from another PXView build could not be picked without typing its name.
    const QString file_name = QFileDialog::getOpenFileName(
        this, 
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OPEN_FILE), "Open File"), 
        app.userHistory.openDir,
        "PXView Data (*.pxl);;"
        "Sigrok Session (*.sr *.srzip);;"
        "All Supported (*.pxl *.sr *.srzip)");

    if (!file_name.isEmpty()) { 
        QString fname = path::GetDirectoryName(file_name);
        if (fname != app.userHistory.openDir){
            app.userHistory.openDir = fname;
            app.SaveHistory();
        }

        sig_load_file(file_name);
    }
}

void FileBar::build_import_menu()
{
    if (!_menu_import)
        return;

    // First entry = the old single "Import..." action: the module is detected
    // from the file content. Everything below it forces one specific module.
    _menu_import->addAction(_action_import);
    _menu_import->addSeparator();

    const struct sr_input_module **imods = sr_input_list();
    if (!imods)
        return;

    for (int i = 0; imods[i]; i++) {
        const struct sr_input_module *imod = imods[i];
        const char *id = sr_input_id_get(imod);
        if (!id)
            continue;

        const char *name = sr_input_name_get(imod);
        const QString format_id = QString::fromUtf8(id);

        QStringList extensions;
        const char *const *exts = sr_input_extensions_get(imod);
        for (int e = 0; exts && exts[e]; e++)
            extensions << QString::fromUtf8(exts[e]);

        QAction *action = new QAction(
            name ? QString::fromUtf8(name) : format_id, this);
        action->setObjectName("actionImportFormat_" + format_id);
        action->setData(format_id);
        // The file dialog of an explicitly chosen format must only offer that
        // module's own extensions, otherwise the choice would be pointless.
        action->setProperty("extensions", extensions);
        connect(action, &QAction::triggered, this, [this, format_id]() {
            on_import_format_triggered(format_id);
        });

        _menu_import->addAction(action);
        _import_format_actions.push_back(action);
    }
}

void FileBar::on_actionImport_triggered()
{
    //import external data file (VCD, CSV, binary, etc.)
    //aligned with PulseView's import data functionality
    AppConfig &app = AppConfig::Instance();

    // 多 tab 架构：导入文件创建新 tab，旧 tab 数据保留在文档中，
    // 因此移除旧版单 tab 的"是否保存数据"提示（见 on_actionOpen_triggered）。

    // Build file filter from libsigrok input modules
    QStringList filters;
    QStringList allExtensions;
    const struct sr_input_module **imods = sr_input_list();
    if (imods) {
        while (*imods) {
            const struct sr_input_module *imod = *imods;
            const char *id = sr_input_id_get(imod);
            const char *name = sr_input_name_get(imod);
            const char *const *exts = sr_input_extensions_get(imod);

            if (!id)
                continue;

            // Collect extensions for "All supported formats" filter
            if (exts) {
                for (int i = 0; exts[i]; i++) {
                    allExtensions << QString("*.") + exts[i];
                }
            }

            // Build per-module filter: "Module Name (*.ext1 *.ext2)"
            QString filter = QString(name ? name : id) + " (";
            if (exts) {
                for (int i = 0; exts[i]; i++) {
                    if (i > 0)
                        filter += " ";
                    filter += "*." + QString(exts[i]);
                }
            }
            filter += ")";
            filters << filter;

            imods++;
        }
    }

    // Build the complete filter string
    QString filterStr;
    if (!allExtensions.isEmpty()) {
        filterStr = QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IMPORT_FILE), "Import File")) +
                    " (" + allExtensions.join(" ") + ")";
        filterStr += ";;";
    }
    filterStr += filters.join(";;");

    const QString file_name = QFileDialog::getOpenFileName(
        this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IMPORT_FILE), "Import File"),
        app.userHistory.openDir,
        filterStr);

    if (file_name.isEmpty())
        return;

    QString fname = path::GetDirectoryName(file_name);
    if (fname != app.userHistory.openDir){
        app.userHistory.openDir = fname;
        app.SaveHistory();
    }

    // Ask which module claims this file *before* importing: only then can the
    // module's option dialog be shown (picking "Binary" in the menu and picking
    // a .binary file here end up in the same place).
    const QString format_id = _session->probe_import_format(file_name);
    if (format_id.isEmpty()) {
        pxv_warn("Import file: no input module matches \"%s\"; "
                 "letting libsigrok decide during the import",
                 file_name.toUtf8().constData());
    }

    run_import(file_name, format_id);
}

void FileBar::on_import_format_triggered(const QString &format_id)
{
    AppConfig &app = AppConfig::Instance();

    QStringList patterns;
    for (QAction *action : _import_format_actions) {
        if (action->data().toString() != format_id)
            continue;
        for (const QString &ext : action->property("extensions").toStringList())
            patterns << "*." + ext;
        break;
    }

    QString filter = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IMPORT_FILE), "Import File");
    // A module may declare no extension at all: then the user picks freely.
    filter += patterns.isEmpty() ? " (*)" : " (" + patterns.join(" ") + ")";

    const QString file_name = QFileDialog::getOpenFileName(
        this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IMPORT_FILE), "Import File"),
        app.userHistory.openDir,
        filter);

    if (file_name.isEmpty())
        return;

    QString fname = path::GetDirectoryName(file_name);
    if (fname != app.userHistory.openDir){
        app.userHistory.openDir = fname;
        app.SaveHistory();
    }

    run_import(file_name, format_id);
}

void FileBar::run_import(const QString &file_name, const QString &format_id)
{
    GHashTable *options = nullptr;

    // Show the module's own options (numchannels/samplerate for "binary", ...)
    // when it declares any. The values are pre-filled with what the currently
    // open device suggests -- a hint the user can override, which is exactly
    // what the old "detect and hope" path could not do.
    if (!format_id.isEmpty()) {
        const struct sr_input_module *module =
            sr_input_find(format_id.toUtf8().constData());
        if (module) {
            data::sr_options::OptionsHandle handle =
                data::sr_options::OptionsHandle::for_input(module);

            if (!handle.empty()) {
                QString title =
                    L_S(STR_PAGE_DLG, S_ID(IDS_DLG_IMPORT_FILE), "Import File");
                const char *module_name = sr_input_name_get(module);
                if (module_name)
                    title += " - " + QString::fromUtf8(module_name);

                // The file name is passed along so a name carrying PXView's
                // hint block ("-32ch-1000000Hz") pre-fills channel count and
                // sample rate instead of the "current device" guess — but only
                // for modules whose files carry no metadata of their own
                // (binary/chronovu-la8/raw_analog). VCD et al. describe
                // themselves; seeding them would override the file, and for VCD
                // "numchannels" is a maximum that must not be lowered to the
                // currently open device's channel count.
                dialogs::InputOutputOptions dlg(
                    this, title, handle.options(),
                    _session->import_option_prefill(file_name,
                                                   sr_input_id_get(module)));

                // The dialog copied every value it needs while constructing;
                // release the module's static option array before showing it.
                handle.reset();

                if (dlg.exec() != QDialog::Accepted)
                    return;  // user cancelled the import

                options = dlg.make_options_table();
                pxv_info("Import file: \"%s\" as \"%s\" with %d option(s)",
                         file_name.toUtf8().constData(), format_id.toUtf8().constData(),
                         dlg.option_count());
            }
        } else {
            pxv_warn("Import file: unknown input module \"%s\", falling back to "
                     "detection", format_id.toUtf8().constData());
        }
    }

    // Ownership of `options` travels with the signal; the receiving slot
    // destroys it (see MainWindowFileOps::on_import_file).
    emit sig_import_file(file_name, format_id, options);
}

void FileBar::on_actionLoad_triggered()
{ 
    //load session file
    AppConfig &app = AppConfig::Instance();      
    const QString file_name = QFileDialog::getOpenFileName(
        this, 
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OPEN_SEESION), "Open Session"), 
        app.userHistory.sessionDir, 
        "PXView Session (*.pxc)");

    if (!file_name.isEmpty()) {
        QString fname = path::GetDirectoryName(file_name);
        if (fname != app.userHistory.sessionDir){
            app.userHistory.sessionDir = fname;
            app.SaveHistory();
        }
         
        sig_load_session(file_name);
    }
}

void FileBar::on_actionDefault_triggered()
{ 
    QDir dir(GetFirmwareDir());
    if (!dir.exists()) { 
          MsgBox::Show(nullptr, L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NOT_FOND_DEFAULT_PROFILE),
             "Cannot find default profile for this device!"), this);
          return;
    }
   
    QString file_name = genDefaultSessionFile();
    
    sig_load_session(file_name);
}

QString FileBar::genDefaultSessionFile()
{   
    QDir dir(GetFirmwareDir());

    QString driver_name = _signals->device()->driver_name();
    QString mode_name = QString::number(_signals->device()->get_work_mode());
    QString file_name = dir.absolutePath() + "/" + driver_name + mode_name +".def.pxc";

    return file_name;
}

void FileBar::on_actionStore_triggered()
{
    //store session file
  
      AppConfig &app = AppConfig::Instance();  

    QString file_name = QFileDialog::getSaveFileName(
                this, 
                L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAVE_SEESION), "Save Session"),
                app.userHistory.sessionDir,
                "PXView Session (*.pxc)");

    if (!file_name.isEmpty()) {
        QFileInfo f(file_name);
        if(f.suffix().compare("pxc"))
            file_name.append(".pxc");

        QString fname = path::GetDirectoryName(file_name);
        if (fname != app.userHistory.sessionDir){
            app.userHistory.sessionDir = fname;
            app.SaveHistory();
        }

        // 命令/通知拆分（2026-09-16）：保存前的前置提交经 store_conf_pending()
        // 信号直连 MainWindow（同步执行采样栏设置提交，保证 sig_store_session
        // 读到最新值）。原 StoreConfPrev 事件已退役 —— async 派发下提交落在
        // 读取之后，"前置"保证名存实亡。
        emit store_conf_pending();

        sig_store_session(file_name);
    }
}

void FileBar::on_actionCapture_triggered()
{
    // _file_button.close();
    QCoreApplication::sendPostedEvents();
    QTimer::singleShot(100, this, &FileBar::sig_screenShot);
}

void FileBar::update_view_status()
{
    bool bEnable = _capture->is_working() == false;
    bool is_hardware = _signals->device()->is_hardware();
    // _file_button.setEnabled(bEnable);
    _menu_session->setEnabled(bEnable && is_hardware); 
}

void FileBar::UpdateLanguage()
{
    retranslateUi();
}

void FileBar::UpdateTheme()
{
    reStyle();

    QList<QAction*> actionList = _menu->actions();
    for (int i = 0; i < actionList.size(); i++) {
        QFont font = theme_font_toolbar();
        actionList.at(i)->setFont(font);
    }
}

void FileBar::UpdateFont()
{ 
    QFont font = theme_font_toolbar();
    ui::set_toolbar_font(this, font);
}

} // namespace toolbars
} // namespace pv
