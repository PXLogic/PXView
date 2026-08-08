/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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
#include "pv/dialogs/dsmessagebox.h"
#include "pv/dialogs/shadow.h"

#include <QObject>
#include <QEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QAbstractButton>
#include "pv/base/pxvdef.h"
#include "pv/ui/langresource.h"
#include "pv/config/appconfig.h"
#include "pv/ui/fn.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/popupdlglist.h"

namespace pv {
namespace dialogs {

DSMessageBox::DSMessageBox(QWidget *parent,const QString title) :
#ifdef Q_OS_LINUX
    QDialog(nullptr)  //enable the popup dialog draged.
#else
    QDialog(parent)
#endif
{
    (void)parent;
    _layout = nullptr;
    _main_widget = nullptr;
    _msg = nullptr;
    _titlebar = nullptr;
    _shadow = nullptr;
    _main_layout = nullptr;

    _bClickYes = false;

    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);

    _main_widget = new QWidget(this);
    _main_layout = new QVBoxLayout(_main_widget);
    _main_widget->setLayout(_main_layout);

    _shadow = new Shadow(this);
    _msg = new QMessageBox(this);
    _titlebar = new toolbars::TitleBar(false, this, nullptr, false, false);
    _layout = new QVBoxLayout(this);

    _shadow->setBlurRadius(10.0);
    _shadow->setDistance(3.0);
    _shadow->setColor(QColor(0, 0, 0, 80));

    _main_widget->setAutoFillBackground(true);
    this->setGraphicsEffect(_shadow);

    _msg->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);

    if (!title.isEmpty()){
        _titlebar->setTitle(title);
    }
    else{
        _titlebar->setTitle(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MESSAGE), "Message"));
    }

    _main_layout->addWidget(_titlebar);
    _main_layout->addWidget(_msg);
    _layout->addWidget(_main_widget);

    setLayout(_layout);

    connect(_msg, &QMessageBox::buttonClicked, this, &DSMessageBox::on_button);
}


DSMessageBox::~DSMessageBox()
{
    DESTROY_QT_OBJECT(_layout);
    DESTROY_QT_OBJECT(_main_widget);
    DESTROY_QT_OBJECT(_msg);
    DESTROY_QT_OBJECT(_titlebar);
    DESTROY_QT_OBJECT(_shadow);
    DESTROY_QT_OBJECT(_main_layout);

    PopupDlgList::RemoveDlgFromList(this);
}

void DSMessageBox::accept()
{
    using namespace Qt;

    QDialog::accept();
}

void DSMessageBox::reject()
{
    using namespace Qt;

    QDialog::reject();
}

QMessageBox* DSMessageBox::mBox()
{
    return _msg;
}

void DSMessageBox::on_button(QAbstractButton *btn)
{
    QMessageBox::ButtonRole role = _msg->buttonRole(btn);

    if (role == QMessageBox::AcceptRole || role == QMessageBox::YesRole){
        _bClickYes = true;
         accept();
    }
    else
        reject();
}

int DSMessageBox::exec()
{
    QFont font = theme_font_dialog();
    ui::set_form_font(this, font);

    if (_titlebar != nullptr){
        _titlebar->update_font();
    }

    PopupDlgList::AddDlgTolist(this);

    return QDialog::exec();
}

} // namespace dialogs
} // namespace pv
