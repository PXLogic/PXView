/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#ifndef PXVIEW_PV_MAINWINDOW_TAB_MANAGER_H
#define PXVIEW_PV_MAINWINDOW_TAB_MANAGER_H

#include <QList>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <QPointer>

class Q_WIDGETS_EXPORT QDragEnterEvent;

namespace pv {

class MainWindow;
class SigSession;
class TabContext;

namespace view {
class View;
}

namespace ui {
class DraggableTabWidget;
}

/**
 * @brief Tab management delegate for MainWindow.
 *
 * Phase 2 refactoring: extracts all tab-related state and logic from
 * MainWindow. Holds the DraggableTabWidget, the list of TabContexts,
 * and the current tab index. All tab operation methods (add_tab,
 * remove_tab, on_tab_changed, etc.) live here.
 *
 * TabManager is a friend of MainWindow so it can access the dock
 * widgets and toolbars needed for context binding on tab switch.
 */
class TabManager {
public:
    explicit TabManager(MainWindow *wnd);
    ~TabManager();

    // ---- Initialization ----

    /** Create the DraggableTabWidget and add it to the parent layout. */
    void create_tab_widget(QWidget *parent, QVBoxLayout *layout);

    /** Create the initial "File" tab during setup_ui. */
    void init_initial_tab();

    /** Wire up all DnDTabWidget signal/slot connections. */
    void setup_connections();

    /** Close all detached tab windows (called during shutdown). */
    void close_detached_windows();

    // ---- Accessors ----

    pv::ui::DraggableTabWidget *tab_widget() { return _tab_widget; }
    const QList<pv::TabContext *> &contexts() const { return _tab_contexts; }
    QList<pv::TabContext *> &contexts() { return _tab_contexts; }
    int current_index() const { return _current_tab_index; }
    void set_current_index(int idx) { _current_tab_index = idx; }
    int tab_count() const { return _tab_contexts.size(); }

    pv::view::View *current_view();
    pv::TabContext *current_context();

    // ---- Tab operations ----

    void add_tab(pv::TabContext *ctx);
    void remove_tab(int index);
    void update_tab_style(int index);

    // Bind/unbind dock widgets and toolbars to a context.
    void bind_docks(pv::TabContext *ctx);
    void unbind_docks();
    void set_view_on_docks(pv::view::View *view);

    // ---- Slot handlers (called from MainWindow slots) ----

    void on_tab_changed(int index);
    void on_tab_moved(int from, int to);
    void on_tab_detach(int index, QWidget *widget, const QString &title);
    void on_tab_attached(QWidget *widget, const QString &title);
    void on_new_tab_requested();

    // ---- Tab renamed handler ----
    void on_tab_renamed(int index, const QString &title);

    // ---- Detached-window reattach handler (lambda logic) ----
    void on_tab_attached_extended(QWidget *widget, const QString &title);

private:
    QPointer<MainWindow> _wnd;
    pv::ui::DraggableTabWidget *_tab_widget = nullptr;
    QList<pv::TabContext *> _tab_contexts;
    int _current_tab_index = -1;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_TAB_MANAGER_H
