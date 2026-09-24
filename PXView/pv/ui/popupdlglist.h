/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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

#ifndef POPUP_DLG_LIST_H
#define POPUP_DLG_LIST_H

#include <cstddef>
#include <vector>
#include <QPointer>
#include <QWidget>

class QScreen;

struct PopuDlgItem
{
    QScreen *screen;
    // QPointer, not a raw pointer: a dialog can be destroyed without going
    // through RemoveDlgFromList() (e.g. a stack-allocated DSMessageBox whose
    // deferred delete lands out of order). With a raw pointer that entry stays
    // in the list and the next walk dereferences freed memory; with QPointer it
    // auto-nulls and the walk can drop it safely.
    QPointer<QWidget> widget;
};

class PopupDlgList
{
public:
    static void AddDlgTolist(QWidget *w);
    static void RemoveDlgFromList(QWidget *w);
    static void TryCloseAllByScreenChanged(QScreen *windowScreen);
    static void SetCurrentScreen(QScreen *screen);
};

#endif