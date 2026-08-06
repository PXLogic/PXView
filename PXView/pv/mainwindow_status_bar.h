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

#ifndef PXVIEW_PV_MAINWINDOW_STATUS_BAR_H
#define PXVIEW_PV_MAINWINDOW_STATUS_BAR_H

class QLabel;

namespace pv {

class MainWindow;

/**
 * @brief Status bar management delegate for MainWindow.
 *
 * Phase 2 refactoring: extracts the status bar update logic from
 * MainWindow. Manages the disk cache status label, FPS counter,
 * and sample period display.
 */
class MainWindowStatusBar {
public:
    explicit MainWindowStatusBar(MainWindow *wnd);

    /** Create and add all status bar labels. Called during setup_ui(). */
    void init(QLabel *disk_cache_label, QLabel *trig_time_label,
              QLabel *sample_period_label, QLabel *fps_label);

    /** Update disk cache status, trigger time, and related info.
     *  Called periodically by _disk_cache_status_timer (500ms). */
    void update_disk_cache_status();

    /** Update FPS display.
     *  Called periodically by _fps_timer (1000ms). */
    void update_fps();

    /** Update sample period label from current context. */
    void update_sample_period();

private:
    MainWindow *_wnd;
    QLabel *_disk_cache_label = nullptr;
    QLabel *_trig_time_label = nullptr;
    QLabel *_sample_period_label = nullptr;
    QLabel *_fps_label = nullptr;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_STATUS_BAR_H
