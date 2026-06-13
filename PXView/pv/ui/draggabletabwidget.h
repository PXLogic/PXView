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

#ifndef PXVIEW_PV_UI_DRAGGABLETABWIDGET_H
#define PXVIEW_PV_UI_DRAGGABLETABWIDGET_H

#include <QTabWidget>
#include <QList>
#include <QPointer>
#include <QPushButton>

namespace pv {

class SubMainFrame;

namespace ui {

class DraggableTabBar;

class DraggableTabWidget : public QTabWidget
{
    Q_OBJECT

public:
    explicit DraggableTabWidget(QWidget *parent = nullptr);

    int addTab(QWidget *page, const QString &label);
    int addTab(QWidget *page, const QIcon &icon, const QString &label);

    void closeAllDetachedWindows();

signals:
    void tabDetached(int index, QWidget *widget, const QString &title);
    void tabAttached(QWidget *widget, const QString &title);
    void newTabRequested();
    void tabRenameRequested(int index);
    void tabRenamed(int index, const QString &title);
    void tabCloseRequested(int index);
    void tabCloseOthersRequested(int index);
    void tabCloseRightRequested(int index);
    void tabMoved(int from, int to);

private slots:
    void onDetachTab(int index, const QPoint &dropPos);
    void onTabCloseRequested(int index);
    void onTabRenameRequested(int index);
    void onDetachedWindowClosed(QWidget *content, const QString &title);
    void onTabMoveRequested(int from, int to);

private:
    void update_add_button_position();

protected:
    void paintEvent(QPaintEvent *event) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    DraggableTabBar *_draggable_tab_bar;
    QPushButton *_add_button;
    QWidget *_tab_bottom_line;
    QList<QPointer<SubMainFrame>> _detached_windows;
};

} // namespace ui
} // namespace pv

#endif
