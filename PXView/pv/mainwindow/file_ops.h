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

#ifndef PXVIEW_PV_MAINWINDOW_FILE_OPS_H
#define PXVIEW_PV_MAINWINDOW_FILE_OPS_H

#include <QString>

#include <glib.h>

namespace pv {

class MainWindow;
class TabContext;

/**
 * @brief File operations delegate for MainWindow.
 *
 * Phase 2 refactoring: extracts on_load_file(), on_import_file(),
 * on_save(), on_export(), and on_screenShot() from MainWindow.
 * The delegate is a friend of MainWindow so it can access private
 * members (session, device_agent, tab_manager, etc.) directly.
 */
class MainWindowFileOps {
public:
    explicit MainWindowFileOps(MainWindow *wnd) : _wnd(wnd) {}

    void on_load_file(QString file_name);

    /**
     * 把一个"已存在但数据尚未重放"的文件设备 tab 按 filePath 重新打开。
     *
     * workspace 恢复出来的文件设备 tab 不在启动时读文件（读 .pxl 会触发整条
     * 采集/回放管线），而是在用户首次切到该 tab 时才调用本方法（懒加载）。
     * 不新建 tab —— 复用调用方传入的 ctx。
     *
     * @return 重开成功（虚拟设备已绑定并激活）返回 true；文件不存在或
     *         set_file 失败返回 false（调用方保留该 tab，仅清掉 filePath）。
     */
    bool reload_file_into_context(pv::TabContext *ctx);

    /**
     * Import an external data file.
     *
     * @param format_id     the input module the user picked (empty = detect).
     * @param input_options the user's option values, or nullptr. OWNERSHIP IS
     *                      TAKEN: the table is destroyed when this returns,
     *                      whatever the outcome (the session only borrows it).
     */
    void on_import_file(QString file_name, QString format_id,
                        GHashTable *input_options);
    void on_save();
    void on_export();
    void on_screenShot();

private:
    MainWindow *_wnd;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_FILE_OPS_H
