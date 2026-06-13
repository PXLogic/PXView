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

#include "devmode.h"
#include "view.h"
#include "trace.h"
#include "../sigsession.h" 

#include <assert.h> 
#include <QStyleOption>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <QHBoxLayout>

#include "../config/appconfig.h"
#include "../ui/msgbox.h"
#include "../log.h"
#include "../ui/langresource.h"
#include "../appcontrol.h"
#include "../ui/fn.h"
#include "../ui/iconcache.h"
#include "../ui/dockfonts.h"


static const struct dev_mode_name dev_mode_name_list[] =
{
    {LOGIC, "la.svg"},
    {ANALOG, "daq.svg"},
    {DSO, "osc.svg"},
};
  
namespace pv {
namespace view {

DevMode::DevMode(QWidget *parent, SigSession *session) :
    QWidget(parent) 
{
    _bFile = false;
    _header_collapsed = false;

    _session = session;
    _device_agent = session->get_device();

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(2, 0, 0, 0);

    _close_button = new XToolButton();
    _close_button->setObjectName("FileCloseButton");
    _close_button->setContentsMargins(0, 0, 0, 0);
    _close_button->setFixedWidth(20);
    _close_button->setFixedHeight(height());
    _close_button->setIconSize(QSize(20, 20));
    _close_button->setToolButtonStyle(Qt::ToolButtonIconOnly); 
    _close_button->setMinimumWidth(20);

    _mode_btn = new XToolButton();
    _mode_btn->setObjectName("ModeButton");
    _mode_btn->setIconSize(QSize(height() * 1.5, height()));
    _mode_btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _mode_btn->setContentsMargins(0, 0, 0, 0);  
    _mode_btn->setPopupMode(QToolButton::InstantPopup);
    _mode_btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    _pop_menu = new QMenu(this);
    _pop_menu->setContentsMargins(15,0,0,0);
    _mode_btn->setMenu(_pop_menu);

    _collapse_btn = new XToolButton();
    _collapse_btn->setObjectName("HeaderCollapseButton");
    _collapse_btn->setIcon(getCollapseIcon(false));
    _collapse_btn->setFixedSize(24, 24);
    _collapse_btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    _collapse_btn->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(_close_button);
    layout->addWidget(_mode_btn, 1); 
    layout->addWidget(_collapse_btn);

    setLayout(layout);

    connect(_close_button, &QAbstractButton::clicked, this, &DevMode::on_close);
    connect(_collapse_btn, &QAbstractButton::clicked, this, &DevMode::on_collapse_toggle);

    ADD_UI(this);
}

DevMode::~DevMode()
{
    REMOVE_UI(this);
}

void DevMode::set_device()
{ 
     if (_device_agent->have_instance() == false){
        pxv_detail("DevMode::set_device, Have no device.");   
        return;
     }

    _bFile = false;
   
   //remove all action object
    for(std::map<QAction *, const sr_dev_mode *>::const_iterator i = _mode_list.begin();
        i != _mode_list.end(); i++) {
        (*i).first->setParent(NULL);
        _pop_menu->removeAction((*i).first);
        delete (*i).first;
    }
    _mode_list.clear();

    _close_button->setIcon(QIcon());
    _close_button->setDisabled(true); 

    QString iconPath = GetIconPath() + "/";
    auto dev_mode_list  = _device_agent->get_device_mode_list();

    for (const GSList *l = dev_mode_list; l; l = l->next)
    {
        const sr_dev_mode *mode = (const sr_dev_mode *)l->data;
        auto *mode_name = get_mode_name(mode->mode);
        QString icon_name = QString::fromLocal8Bit(mode_name->_logo);

        QAction *action = new QAction(this);
        action->setIcon(IconCache::Instance().icon(iconPath + "square-" + icon_name));

        int md = mode->mode;

        if (md == LOGIC)
            action->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_LOGIC), "Logic Analyzer"));
        else if (md == ANALOG)
            action->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_ANALOG), "Data Acquisition"));
        else if (md == DSO)
            action->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_DSO), "Oscilloscope"));

        connect(action, &QAction::triggered, this, &DevMode::on_mode_change);

        _mode_list[action] = mode;
        int cur_mode = _device_agent->get_work_mode();
          
        if (cur_mode == _mode_list[action]->mode)
        {
            QString icon_fname = iconPath + icon_name;
            _mode_btn->setIcon(IconCache::Instance().icon(icon_fname));
            
            if (cur_mode == LOGIC)
                _mode_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_LOGIC), "Logic Analyzer"));
            else if (cur_mode == ANALOG)
                _mode_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_ANALOG), "Data Acquisition"));
            else if (cur_mode == DSO)
                _mode_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_DSO), "Oscilloscope"));
        }
        _pop_menu->addAction(action);
    }

    if (_device_agent->is_file()){
        _close_button->setDisabled(false);
        _close_button->setIcon(IconCache::Instance().icon(iconPath + "/close.svg"));
        _bFile = true;
    }

    UpdateFont();
    update();    
}

void DevMode::paintEvent(QPaintEvent*)
{  
    using pv::view::Trace;

    QStyleOption o;
    o.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &o, &painter, this);
}

void DevMode::on_mode_change()
{
    if (_device_agent->have_instance() == false){
        assert(false);
    }
    
    QAction *action = qobject_cast<QAction *>(sender());

    if (_device_agent->get_work_mode() == _mode_list[action]->mode){
        return;
    }

    QString iconPath = GetIconPath();

    for(auto i = _mode_list.begin();i != _mode_list.end(); i++)
    {
        if ((*i).first == action){

            int mode = (*i).second->mode;
            if (_device_agent->get_work_mode() == mode){
                pxv_info("Current mode is set.");
                break;
            }
            
            _session->stop_capture();            
            _session->session_save();                                    
            _session->switch_work_mode(mode);

            auto *mode_name = get_mode_name(mode);
            QString icon_fname = iconPath + "/" + QString::fromLocal8Bit(mode_name->_logo);
            
            _mode_btn->setIcon(IconCache::Instance().icon(icon_fname));
            int cur_mode = mode_name->_mode;

            if (cur_mode == LOGIC)
                _mode_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_LOGIC), "Logic Analyzer"));
            else if (cur_mode == ANALOG)
                _mode_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_ANALOG), "Data Acquisition"));
            else if (cur_mode == DSO)
                _mode_btn->setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_DEVICE_MODE_DSO), "Oscilloscope"));
               
            break;                
        }      
    }

    UpdateFont();
}

void DevMode::on_close()
{
   if (_device_agent->have_instance() == false){
        assert(false);
    }

    if (_bFile && MsgBox::Confirm(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_CLOSE_DEVICE), "Are you sure to close the device?"))){
        _session->close_file(_device_agent->handle());
    }
}

void DevMode::on_collapse_toggle()
{
    _header_collapsed = !_header_collapsed;

    if (_header_collapsed) {
        _collapse_btn->setIcon(getCollapseIcon(true));
        _close_button->hide();
        _mode_btn->hide();
    } else {
        _collapse_btn->setIcon(getCollapseIcon(false));
        _close_button->show();
        _mode_btn->show();
    }

    emit header_collapse_changed(_header_collapsed);
}

void DevMode::mousePressEvent(QMouseEvent *event)
{
	assert(event);
	(void)event;
}

void DevMode::mouseReleaseEvent(QMouseEvent *event)
{
	assert(event);
        (void)event;
}

void DevMode::mouseMoveEvent(QMouseEvent *event)
{
	assert(event);
	_mouse_point = event->position().toPoint();
	update();
}

void DevMode::leaveEvent(QEvent*)
{
	_mouse_point = QPoint(-1, -1);
	update();
}

const struct dev_mode_name* DevMode::get_mode_name(int mode) 
{
    for(auto &o : dev_mode_name_list)
        if (mode == o._mode){
            return &o;
    }
    assert(false);
    return nullptr;
}

void DevMode::UpdateLanguage()
{
    set_device();
}

void DevMode::UpdateTheme()
{
    set_device();
}

void DevMode::UpdateFont()
{
    QFont font = theme_font_toolbar();
    
    auto buttons = this->findChildren<QToolButton*>();
    for(auto o : buttons)
    { 
        o->setFont(font);
    }

    for (auto it = _mode_list.begin(); it != _mode_list.end(); it++)
    { 
        (*it).first->setFont(font);
    }
}

QIcon DevMode::getCollapseIcon(bool expand)
{
    QString iconPath = GetIconPath();
    QString name = expand ? "/header-expand.svg" : "/header-collapse.svg";
    return IconCache::Instance().icon(iconPath + name);
}

} // namespace view
} // namespace pv
