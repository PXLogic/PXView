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

#include "pv/ui/popupdlglist.h"
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QTimer>

#include "pv/base/log.h"

namespace{
    std::vector<PopuDlgItem> g_popup_dlg_list;
    QScreen     *currentScreen = nullptr;
}

void PopupDlgList::AddDlgTolist(QWidget *w)
{   
    if (w != nullptr){
        PopuDlgItem item;
        item.screen = currentScreen;
        item.widget = w;
        g_popup_dlg_list.push_back(item);
    }
}

void PopupDlgList::RemoveDlgFromList(QWidget *w)
{
    if (w != nullptr){
        for (auto it = g_popup_dlg_list.begin(); it != g_popup_dlg_list.end(); ++it)
        {
            if ((*it).widget == w){
                g_popup_dlg_list.erase(it);
                break;
            }
        }
    }
}

void PopupDlgList::TryCloseAllByScreenChanged(QScreen *windowScreen)
{
    // This walk must survive re-entrant mutation of the list.
    //
    // close() runs the dialog's closeEvent(), which destroys it, and
    // ~PxDialog / ~DSMessageBox call RemoveDlgFromList() from inside that
    // destruction. So the container can be modified *while* close() is on the
    // stack: holding an iterator (or a size captured up front) across the call
    // leaves it dangling, and erasing with a dangling iterator corrupts the
    // heap. Take the entry out of the list FIRST, then close the widget, and
    // re-read the size every iteration.
    for (std::size_t i = 0; i < g_popup_dlg_list.size(); ) {
        const PopuDlgItem item = g_popup_dlg_list[i];

        // widget is a QPointer: a dialog destroyed by any other path auto-nulls,
        // so a stale entry can never be dereferenced -- just drop it.
        if (item.widget.isNull()) {
            g_popup_dlg_list.erase(g_popup_dlg_list.begin() +
                                   static_cast<std::ptrdiff_t>(i));
            continue;
        }

        if (item.screen != windowScreen && item.widget->isVisible()) {
            // Remove before closing; the destructor may call
            // RemoveDlgFromList() re-entrantly, which is now a harmless no-op.
            g_popup_dlg_list.erase(g_popup_dlg_list.begin() +
                                   static_cast<std::ptrdiff_t>(i));
            item.widget->close(); //Close the dialog.
            continue;
        }

        ++i;
    }
}

void PopupDlgList::SetCurrentScreen(QScreen *screen)
{
    currentScreen = screen;
}
