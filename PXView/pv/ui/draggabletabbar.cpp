/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
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

#include "draggabletabbar.h"

#include <QMouseEvent>
#include <QApplication>
#include <QMenu>
#include <QContextMenuEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QStyleOption>
#include <QStylePainter>

#include "../config/appconfig.h"

namespace pv {
namespace ui {

DraggableTabBar::DraggableTabBar(QWidget *parent)
    : QTabBar(parent),
      _drag_started(false),
      _drag_index(-1),
      _drag_preview(nullptr),
      _drag_outside(false),
      _drop_index(-1)
{
    setDrawBase(false);
    setAttribute(Qt::WA_StyledBackground, true);
}

DraggableTabBar::~DraggableTabBar()
{
    destroy_drag_preview();
}

void DraggableTabBar::paintEvent(QPaintEvent *event)
{
    QTabBar::paintEvent(event);

    if (_drag_started && !_drag_outside && _drop_index >= 0) {
        QPainter p(this);
        
        QString accentStr = AppConfig::Instance().GetThemeTokenValue("@accent");
        QColor accentColor(accentStr);
        if (!accentColor.isValid()) accentColor = QColor(0, 120, 215);
        
        p.setPen(QPen(accentColor, 3));
        
        int x = 0;
        if (_drop_index < count()) {
            QRect r = tabRect(_drop_index);
            if (r.isValid()) {
                x = r.left();
            } else {
                for (int i = _drop_index + 1; i < count(); ++i) {
                    if (tabRect(i).isValid()) {
                        x = tabRect(i).left();
                        break;
                    }
                }
            }
        } else if (count() > 0) {
            for (int i = count() - 1; i >= 0; --i) {
                if (tabRect(i).isValid()) {
                    x = tabRect(i).right();
                    break;
                }
            }
        }
        p.drawLine(x, 0, x, height());
    }
}


void DraggableTabBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        _drag_index = tabAt(event->position().toPoint());
        _drag_start_pos = event->position().toPoint();
        _drag_started = false;

        if (_drag_index >= 0) {
            QRect tr = tabRect(_drag_index);
            _drag_offset = event->position().toPoint() - tr.topLeft();
            _drag_pixmap = this->grab(tr);

            // Check if click is on the close button
            QWidget *closeBtn = tabButton(_drag_index, QTabBar::RightSide);
            if (!closeBtn)
                closeBtn = tabButton(_drag_index, QTabBar::LeftSide);
            if (closeBtn && closeBtn->isVisible()) {
                QPoint closePos = closeBtn->mapFromParent(event->position().toPoint());
                if (closeBtn->rect().contains(closePos)) {
                    _drag_index = -1; // Don't initiate drag if clicking close button
                }
            }
        }
    }
    QTabBar::mousePressEvent(event);
}

void DraggableTabBar::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton && _drag_index >= 0) {
        QPoint delta = event->position().toPoint() - _drag_start_pos;
        int distance = delta.manhattanLength();

        if (!_drag_started && distance > _drag_threshold) {
            _drag_started = true;
            create_drag_preview(_drag_index);
        }

        if (_drag_started) {
            QPoint global_pos = mapToGlobal(event->position().toPoint());
            QRect bar_rect = rect();

            if (count() > 0) {
                bar_rect.adjust(0, 0, 0, tabRect(count() - 1).bottom() - bar_rect.bottom());
            }

            QRect detach_bounds = bar_rect.adjusted(-20, -50, 20, 50);

            if (!detach_bounds.contains(event->position().toPoint())) {
                _drag_outside = true;
                if (_drop_index != -1) {
                    _drop_index = -1;
                    update();
                }
            } else {
                _drag_outside = false;
                int new_drop_index = count();
                for (int i = 0; i < count(); ++i) {
                    QRect r = tabRect(i);
                    if (r.isValid() && event->position().toPoint().x() < r.center().x()) {
                        new_drop_index = i;
                        break;
                    }
                }
                if (_drop_index != new_drop_index) {
                    _drop_index = new_drop_index;
                    update();
                }
            }
            update_drag_preview_pos(global_pos);
            return;
        }
    }
    QTabBar::mouseMoveEvent(event);
}

void DraggableTabBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && _drag_started) {
        QPoint global_pos = mapToGlobal(event->position().toPoint());
        int detach_idx = _drag_index;
        int drop_idx = _drop_index;
        bool was_outside = _drag_outside;
        
        destroy_drag_preview();
        
        _drag_started = false;
        _drag_index = -1;
        _drag_outside = false;
        _drop_index = -1;
        update();
        
        QTabBar::mouseReleaseEvent(event);
        
        if (was_outside) {
            emit detachTab(detach_idx, global_pos);
        } else if (drop_idx >= 0) {
            emit tabMoveRequested(detach_idx, drop_idx);
        }
        return;
    }
    
    QTabBar::mouseReleaseEvent(event);
}

void DraggableTabBar::contextMenuEvent(QContextMenuEvent *event)
{
    int index = tabAt(event->pos());
    if (index < 0) {
        QTabBar::contextMenuEvent(event);
        return;
    }

    QMenu menu(this);
    QAction *rename_action = menu.addAction(tr("Rename"));
    menu.addSeparator();
    QAction *close_action = menu.addAction(tr("Close"));
    QAction *close_others_action = menu.addAction(tr("Close Others"));
    QAction *close_right_action = menu.addAction(tr("Close All to the Right"));

    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == rename_action) {
        emit tabRenameRequested(index);
    } else if (chosen == close_action) {
        emit tabCloseRequested(index);
    } else if (chosen == close_others_action) {
        emit tabCloseOthersRequested(index);
    } else if (chosen == close_right_action) {
        emit tabCloseRightRequested(index);
    }
}

void DraggableTabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    int index = tabAt(event->position().toPoint());
    if (index >= 0) {
        emit tabRenameRequested(index);
    }
    QTabBar::mouseDoubleClickEvent(event);
}

void DraggableTabBar::create_drag_preview(int index)
{
    destroy_drag_preview();

    if (index < 0 || index >= count())
        return;

    setTabVisible(index, false);

    _drag_preview = new QLabel(nullptr,
        static_cast<Qt::WindowFlags>(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint));
    _drag_preview->setAttribute(Qt::WA_ShowWithoutActivating);
    _drag_preview->setPixmap(_drag_pixmap);
    _drag_preview->setFixedSize(_drag_pixmap.size() / _drag_pixmap.devicePixelRatioF());
    _drag_preview->show();
}

void DraggableTabBar::update_drag_preview_pos(const QPoint &global_pos)
{
    if (_drag_preview) {
        _drag_preview->move(global_pos - _drag_offset);
    }
}

void DraggableTabBar::destroy_drag_preview()
{
    if (_drag_preview) {
        _drag_preview->close();
        _drag_preview->deleteLater();
        _drag_preview = nullptr;
    }
    if (_drag_index >= 0 && _drag_index < count()) {
        setTabVisible(_drag_index, true);
    }
}

} // namespace ui
} // namespace pv
