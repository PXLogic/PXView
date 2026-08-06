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

#ifndef PXVIEW_PV_MAINWINDOW_SIGNAL_CONNECTOR_H
#define PXVIEW_PV_MAINWINDOW_SIGNAL_CONNECTOR_H

namespace pv {

class MainWindow;

/**
 * @brief Signal connection delegate for MainWindow.
 *
 * Phase 2 refactoring: extracts the ~140 lines of connect() calls
 * from MainWindow::setup_ui() into a dedicated delegate. The delegate
 * is a friend of MainWindow so it can access private members
 * (toolbars, docks, event object, etc.) directly.
 */
class MainWindowSignalConnector {
public:
    explicit MainWindowSignalConnector(MainWindow *wnd) : _wnd(wnd) {}

    /** Wire all Qt signal/slot connections for the initial view,
     *  toolbars, docks, and event object. Called from setup_ui(). */
    void setup_connections();

private:
    MainWindow *_wnd;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_SIGNAL_CONNECTOR_H
