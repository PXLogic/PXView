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


#ifndef PXVIEW_PV_TOOLBARS_TRIGBAR_H
#define PXVIEW_PV_TOOLBARS_TRIGBAR_H

#include <QToolBar>
#include <QAction>
#include <QMenu>
#include "../interface/icallbacks.h"
#include "../ui/uimanager.h"

namespace pv {

class SigSession;

namespace toolbars {

class TrigBar : public QToolBar, public IUiWindow
{
    Q_OBJECT

public:
    explicit TrigBar(SigSession *session, QWidget *parent = 0);
    ~TrigBar();
    void reload();
    void update_view_status();

private:
    void retranslateUi();
    void reStyle();

     //IUiWindow
    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

signals:
    void sig_setTheme(QString style);
    void sig_show_lissajous(bool visible);

private slots:
    void on_actionLissajous_triggered();
    void on_actionFft_triggered();
    void on_actionMath_triggered();
    void on_display_setting();

// private:
public:
    SigSession  *_session;
    bool        _enable;

    QMenu       *_function_menu;
    QAction     *_action_fft;
    QAction     *_action_math;

    QMenu       *_display_menu;
    QAction     *_action_dispalyOptions;
    QAction     *_action_lissajous;
};

} // namespace toolbars
} // namespace pv

#endif // PXVIEW_PV_TOOLBARS_TRIGBAR_H
