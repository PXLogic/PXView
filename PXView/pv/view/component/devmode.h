/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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


#ifndef PXVIEW_PV_VIEW_DEVMODE_H
#define PXVIEW_PV_VIEW_DEVMODE_H
 
#include <list>
#include <utility>
#include <map>
#include <set>
#include <memory>
#include <vector>
#include <QWidget>
#include <QPushButton>
#include <QVector> 
#include <QLabel>
#include <QIcon>
#include <libsigrok/libsigrok.h>

#include "pv/base/pxvdef.h"
#include "pv/interface/icallbacks.h"
#include "pv/ui/xtoolbutton.h"
#include "pv/ui/uimanager.h"

struct dev_mode_name{
    int _mode;
    const char *_logo;
};
 
class DeviceAgent;

namespace pv {

class SigSession;

namespace view {

//devece work mode select list
class DevMode : public QWidget, public IUiWindow
{
	Q_OBJECT

private:
    static const int GRID_COLS = 3;

public:
    DevMode(QWidget *parent, SigSession *session);

    ~DevMode();

private:
	void paintEvent(QPaintEvent *event);

private:
	void mousePressEvent(QMouseEvent * event);
	void mouseReleaseEvent(QMouseEvent *event);
	void mouseMoveEvent(QMouseEvent *event);
	void leaveEvent(QEvent *event); 
    const dev_mode_name* get_mode_name(int mode);
    QIcon getCollapseIcon(bool expand);

     //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

public slots:
    void set_device();
    void on_mode_change();
    void on_close();
    void on_collapse_toggle();

private slots:



signals:
    void header_collapse_changed(bool collapsed);
    void mode_change_requested(int mode);
    void stop_capture_requested();
    void save_session_requested();
    void close_file_requested(ds_device_handle dev_handle);

private:
    SigSession *_session;
    std::vector<std::unique_ptr<QAction>> _owned_mode_actions; // Spec v2 Task 6: RAII ownership
    std::map <QAction *, const sr_dev_mode *> _mode_list;  // non-owning lookup
    XToolButton     *_mode_btn;
    QMenu           *_pop_menu;
    QPoint          _mouse_point;
    XToolButton     *_close_button;
    XToolButton     *_collapse_btn;
    bool            _bFile;
    bool            _header_collapsed;

    DeviceAgent     *_device_agent;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_DEVMODE_H
