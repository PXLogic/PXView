/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#include "pv/ui/msgbox.h"
#include "pv/dialogs/dsmessagebox.h"
#include <cassert>
#include <QMessageBox>
#include "pv/base/pxvdef.h"
#include "pv/mainwindow/appcontrol.h"
#include "pv/ui/langresource.h"

//QMessageBox::information(nullptr, "Title", "Content",QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
//QMessageBox::information(nullptr, "Title", "Content",QMessageBox::Yes|QMessageBox::No);
//QMessageBox::information(nullptr, "Title", "Content");
//QMessageBox::information(nullptr, "Title", "Content",QMessageBox::Yes|QMessageBox::No|QMessageBox::Abort);

void MsgBox::Show(const QString text)
{
    MsgBox::Show("", text, "", nullptr, nullptr);
}

void MsgBox::Show(const QString title, const QString text, QWidget *parent)
{
    MsgBox::Show(title, text, "", parent,nullptr);
}

void Show(const QString title, const QString text, const QString infoText)
{
    MsgBox::Show(title, text, infoText, nullptr, nullptr);
}

void MsgBox::Show(const QString title, const QString text, 
        QWidget *parent, pv::dialogs::DSMessageBox **box)
{
    MsgBox::Show(title, text, "", parent, box);
}

void MsgBox::Show(const QString title, const QString text, const QString infoText, 
        QWidget *parent, pv::dialogs::DSMessageBox **box)
{
    assert(!text.isEmpty());

    QString str;
    str.append("\n");
    str.append(text);

    if (parent == nullptr){
        parent = AppControl::Instance()->GetTopWindow();
    }

    pv::dialogs::DSMessageBox msg(parent, title);

    if (box != nullptr){
        *box = &msg;
    }

    msg.mBox()->setText(str);
    msg.mBox()->setStandardButtons(QMessageBox::Ok);
    msg.mBox()->setIcon(QMessageBox::Warning);

    if (infoText != ""){
        msg.mBox()->setInformativeText(infoText);
    }

    msg.exec();     
}

bool MsgBox::Confirm(const QString text, QWidget *parent)
{
    return MsgBox::Confirm(text, "", nullptr, parent);
}

bool MsgBox::Confirm(const QString text, const QString infoText, 
        pv::dialogs::DSMessageBox **box, QWidget *parent)
{
    assert(!text.isEmpty());

    QString str;
    str.append("\n");
    str.append(text);

    if (parent == nullptr){
        parent = AppControl::Instance()->GetTopWindow();
    }

    pv::dialogs::DSMessageBox msg(parent, "");
    msg.mBox()->setText(str);
    msg.mBox()->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msg.mBox()->setIcon(QMessageBox::Question);

    if (infoText != ""){
        msg.mBox()->setInformativeText(infoText);
    }

    if (box != nullptr){
        *box = &msg;
    }

    msg.exec();
    return msg.IsYes();
}
