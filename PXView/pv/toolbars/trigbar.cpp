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

#include "trigbar.h"

#include <QBitmap>
#include <QPainter>
#include <QEvent>

#include "../sigsession.h"
#include "../dialogs/fftoptions.h"
#include "../dialogs/lissajousoptions.h"
#include "../dialogs/mathoptions.h"
#include "../view/trace.h"
#include "../dialogs/applicationpardlg.h"
#include "../ui/langresource.h"
#include "../config/appconfig.h"
#include "../ui/fn.h"
#include "../ui/dockfonts.h"
#include "../ui/iconcache.h"

namespace pv {
namespace toolbars {

TrigBar::TrigBar(SigSession *session, QWidget *parent) :
    QToolBar("Trig Bar", parent),
    _session(session)
{
    _enable = true;

    setMovable(false);
    setContentsMargins(0,0,0,0);

    _action_fft = new QAction(this);
    _action_fft->setObjectName(QString::fromUtf8("actionFft"));

    _action_math = new QAction(this);
    _action_math->setObjectName(QString::fromUtf8("actionMath"));

    _function_menu = new QMenu(this);
    _function_menu->setContentsMargins(0,0,0,0);
    _function_menu->addAction(_action_fft);
    _function_menu->addAction(_action_math);

    _action_lissajous = new QAction(this);
    _action_lissajous->setObjectName(QString::fromUtf8("actionLissajous"));

    _action_dispalyOptions = new QAction(this);

    _display_menu = new QMenu(this);
    _display_menu->setContentsMargins(0,0,0,0);

    _display_menu->addAction(_action_lissajous);
    _display_menu->addAction(_action_dispalyOptions);

    connect(_action_fft, &QAction::triggered, this, &TrigBar::on_actionFft_triggered);
    connect(_action_math, &QAction::triggered, this, &TrigBar::on_actionMath_triggered);
    connect(_action_lissajous, &QAction::triggered, this, &TrigBar::on_actionLissajous_triggered);
    connect(_action_dispalyOptions, &QAction::triggered, this, &TrigBar::on_display_setting);

    ADD_UI(this);
}

TrigBar::~TrigBar()
{
    REMOVE_UI(this);
}

void TrigBar::retranslateUi()
{
    _action_lissajous->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DISPLAY_LISSAJOUS), "Lissajous"));

    _action_fft->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FUNCTION_FFT), "FFT"));
    _action_math->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FUNCTION_MATH), "Math"));

    _action_dispalyOptions->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DISPLAY_OPTIONS), "Options"));
}

void TrigBar::reStyle()
{
    QString iconPath = GetIconPath();
    QColor iconColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");

    auto getIcon = [&](const QString &name) {
        return iconColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, iconColor)
                                   : IconCache::Instance().icon(iconPath + name);
    };

    _action_fft->setIcon(getIcon("/fft.svg"));
    _action_math->setIcon(getIcon("/math.svg"));
    _action_lissajous->setIcon(getIcon("/lissajous.svg"));
    _action_dispalyOptions->setIcon(getIcon("/gear.svg"));
}

void TrigBar::reload()
{
    update_view_status();
    update();
}

void TrigBar::on_actionFft_triggered()
{
    pv::dialogs::FftOptions fft_dlg(this, _session);
    fft_dlg.exec();
}

void TrigBar::on_actionMath_triggered()
{
    pv::dialogs::MathOptions math_dlg(_session, this);
    if (math_dlg.exec() == QDialog::Accepted)
    {
        math_dlg.Apply();
    }
}



void TrigBar::on_actionLissajous_triggered()
{
    pv::dialogs::LissajousOptions lissajous_dlg(_session, this);
    lissajous_dlg.exec();
}

 void TrigBar::on_display_setting()
 {
    pv::dialogs::ApplicationParamDlg dlg;
    dlg.ShowDlg(this);
 }

 void TrigBar::update_view_status()
 {
 }

void TrigBar::UpdateLanguage()
{
    retranslateUi();
}

void TrigBar::UpdateTheme()
{
    reStyle();
}

void TrigBar::UpdateFont()
{
    QFont font = theme_font_toolbar();
    ui::set_toolbar_font(this, font);
}

} // namespace toolbars
} // namespace pv
