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


#include "pv/dialogs/pxdialog.h"
#include "pv/dialogs/shadow.h"
#include "pv/platform/winframeless.h"

#include <QObject>
#include <QEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QAbstractButton>
#include "pv/base/pxvdef.h"
#include "pv/config/appconfig.h"
#include "pv/ui/fn.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/popupdlglist.h"

namespace pv {
namespace dialogs {

PxDialog::PxDialog() : 
 PxDialog(nullptr, false, false)
{
}

PxDialog::PxDialog(QWidget *parent):
 PxDialog(parent, false, false)
{
}

PxDialog::PxDialog(QWidget *parent, bool hasClose):
 PxDialog(parent, hasClose, false)
{
}

PxDialog::PxDialog(QWidget *parent, bool hasClose, bool bBaseButton) :
#ifdef Q_OS_LINUX
    QDialog(nullptr),  //enable the popup dialog draged.
#else
    QDialog(parent),
#endif
    m_bBaseButton(bBaseButton)
{
    (void)parent;

    _base_layout = nullptr;
    _main_layout = nullptr;
    _main_widget = nullptr;
    _titlebar = nullptr;
    _shadow = nullptr; 
    _base_button = nullptr;
    _titleSpaceLine = nullptr;

    m_callback = nullptr; 
    _clickYes = false;
    _nativeFrameApplied = false;
    
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);

#ifndef _WIN32
    // Outside Windows there is no DWM shadow, so the shadow is drawn by Qt
    // (see build_base()) and needs transparent space around the content.
    setAttribute(Qt::WA_TranslucentBackground);
#endif

    build_base(hasClose); 
}

PxDialog::~PxDialog()
{ 
    DESTROY_QT_OBJECT(_base_layout);
    DESTROY_QT_OBJECT(_main_layout);
    DESTROY_QT_OBJECT(_main_widget);
    DESTROY_QT_OBJECT(_titlebar);
    DESTROY_QT_OBJECT(_shadow);
    DESTROY_QT_OBJECT(_base_button);

    PopupDlgList::RemoveDlgFromList(this);
}

void PxDialog::accept()
{  
    _clickYes = true;
    if (m_callback){
        m_callback->OnDlgResult(true);
    }


    QDialog::accept();
}

void PxDialog::reject()
{ 
    _clickYes = false;

    if (m_callback){
        m_callback->OnDlgResult(false);
    } 

    QDialog::reject();
}
  
void PxDialog::setTitle(QString title)
{
    if (_titlebar){
         _titlebar->setTitle(title);
    } 
}

void PxDialog::reload()
{
    show();
}

int PxDialog::exec()
{ 
      //ok,cancel
    if (m_bBaseButton){
        _base_button = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,Qt::Horizontal, this);
         _main_layout->addWidget(_base_button);//, 5, 1, 1, 1, Qt::AlignHCenter | Qt::AlignBottom);
        //_main_layout->addWidget(_base_button,0, Qt::AlignHCenter | Qt::AlignBottom);
        connect(_base_button, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(_base_button, &QDialogButtonBox::accepted, this, &QDialog::accept);
    }

    update_font();

    PopupDlgList::AddDlgTolist(this);
 
    return QDialog::exec();
}

 void PxDialog::SetTitleSpace(int h)
 {
     if (_titleSpaceLine != nullptr){ 
         if (h > 0){
             _titleSpaceLine->setFixedHeight(h);
             _titleSpaceLine->setVisible(true);
         }
         else{
             _titleSpaceLine->setVisible(false);
         }        
     }
 }

void PxDialog::build_base(bool hasClose)
{    
    _main_widget = new QWidget(this);
    _main_layout = new QVBoxLayout(_main_widget);
    _main_widget->setLayout(_main_layout);

    _main_widget->setAutoFillBackground(true);

#ifdef _WIN32
    // Windows: WinFrameless::apply() gives the HWND back a frame so DWM draws
    // its native shadow. The frame itself is removed again in WM_NCCALCSIZE,
    // so there is no visible border. A QGraphicsEffect on the top-level window
    // would be clipped by the window rectangle, which is why no shadow was
    // ever visible before.
#else
    // Other platforms: draw shadow and frame ourselves. The effect must live
    // on the inner widget, and the outer layout has to leave room for it
    // (blurRadius 10 + distance 3 = 13px).
    _shadow = new Shadow(this);
    _shadow->setBlurRadius(10.0);
    _shadow->setDistance(3.0);
    _shadow->setColor(QColor(0, 0, 0, 80));
    _main_widget->setGraphicsEffect(_shadow);
#endif

    _titlebar = new toolbars::TitleBar(false, this, nullptr,hasClose, false);
    _main_layout->addWidget(_titlebar);

    _titleSpaceLine = new QWidget(this);
    _titleSpaceLine->setFixedHeight(15);
    _main_layout->addWidget(_titleSpaceLine);

    _base_layout = new QVBoxLayout(this);   
#ifndef _WIN32
    // Room for the self-drawn shadow, see build_base().
    _base_layout->setContentsMargins(14, 14, 14, 14);
#endif
    _base_layout->addWidget(_main_widget);
    setLayout(_base_layout); 

    _main_layout->setAlignment(Qt::AlignCenter | Qt::AlignTop);
    _main_layout->setContentsMargins(10,5,10,10);   
} 

void PxDialog::update_font()
{
    QFont font = theme_font_dialog();
    ui::set_form_font(this, font);

    if (_titlebar != nullptr){
        _titlebar->update_font();
    }
}

void PxDialog::show()
{
    update_font();
    
    QWidget::show();
}

void PxDialog::showEvent(QShowEvent *event)
{
    if (!_nativeFrameApplied){
        _nativeFrameApplied = true;
        // Only now the HWND exists. Doing this in the constructor would force
        // the native window to be created too early.
        // No border, shadow only: WM_NCCALCSIZE removes the non-client area
        // (see WinFrameless), so nothing is drawn around the content.
        WinFrameless::apply(this);

        // Safety net: paint the frame in the window background color, so any
        // line the system still draws around the window stays invisible.
        WinFrameless::setBorderColor(this, AppConfig::Instance().GetStyleColor());
    }

    QDialog::showEvent(event);
}

bool PxDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (WinFrameless::handleMessage(message, result, this))
        return true;

    return QDialog::nativeEvent(eventType, message, result);
}

} // namespace dialogs
} // namespace pv
