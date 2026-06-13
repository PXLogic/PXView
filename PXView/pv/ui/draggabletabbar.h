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

#ifndef PXVIEW_PV_UI_DRAGGABLETABBAR_H
#define PXVIEW_PV_UI_DRAGGABLETABBAR_H

#include <QTabBar>
#include <QPoint>
#include <QLabel>
#include <QPixmap>

namespace pv {
namespace ui {

class DraggableTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit DraggableTabBar(QWidget *parent = nullptr);
    ~DraggableTabBar();

signals:
    void detachTab(int index, const QPoint &dropPos);
    void tabContextMenuRequested(int index, const QPoint &pos);
    void tabRenameRequested(int index);
    void tabCloseOthersRequested(int index);
    void tabCloseRightRequested(int index);
    void tabMoveRequested(int from, int to);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void create_drag_preview(int index);
    void update_drag_preview_pos(const QPoint &global_pos);
    void destroy_drag_preview();

    bool _drag_started;
    int _drag_index;
    QPoint _drag_start_pos;
    static const int _drag_threshold = 20;

    QLabel *_drag_preview;
    bool _drag_outside;
    QPoint _drag_offset;
    QPixmap _drag_pixmap;
    int _drop_index;
};

} // namespace ui
} // namespace pv

#endif
