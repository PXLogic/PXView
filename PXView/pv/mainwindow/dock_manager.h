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

#ifndef PXVIEW_PV_MAINWINDOW_DOCK_MANAGER_H
#define PXVIEW_PV_MAINWINDOW_DOCK_MANAGER_H

#include <QVBoxLayout>
#include <QWidget>
#include <QPointer>

class QDockWidget;
struct DockOptions; // global-namespace forward declaration

namespace pv {

class MainWindow;
class SigSession;
class TabContext;

namespace view {
class View;
}

namespace dock {
class ProtocolDock;
class TriggerDock;
class DsoTriggerDock;
class MeasureDock;
class SearchDock;
class DeviceOptionsDock;
class LogDock;
class McpControlDock;
class FunctionDock;
}

namespace widgets {
class SlidingDrawer;
class SideBar;
}

/**
 * @brief Dock management delegate for MainWindow.
 *
 * Phase 2 refactoring: extracts all dock-related state and logic from
 * MainWindow. Holds every QDockWidget pointer, the SlidingDrawer and its
 * page indices, the SideBar, and all dock operation methods.
 *
 * DockManager is a friend of MainWindow so it can access the session,
 * device agent, toolbars, and current view needed for dock operations.
 */
class DockManager {
public:
    explicit DockManager(MainWindow *wnd);
    ~DockManager();

    // ---- Initialization ----

    /** Create all dock widgets (trigger, dso_trigger, protocol, measure,
     *  search, device_options, log, mcp, function).  Must be called after
     *  the initial View exists (some docks need it for their constructor). */
    void create_docks(pv::view::View *initial_view);

    /** Create the SlidingDrawer, move dock content widgets into it as
     *  pages, and configure the push layout. */
    void setup_drawer(QWidget *central_widget, QVBoxLayout *layout);

    /** Create the SideBar with all dock/action items. */
    void setup_side_bar();

    /** Wire up SlidingDrawer and SideBar signal/slot connections. */
    void setup_connections();

    /** Install event filters on all dock widgets and the sliding drawer. */
    void install_event_filters(QObject *filter_obj);

    /** Bind/unbind a TabContext on all dock widgets. */
    void bind_context(pv::TabContext *ctx);
    void unbind_context();

    /** Set the active View on all dock widgets that need it. */
    void set_view(pv::view::View *view);

    // ---- Retranslate / restore ----

    /** Retranslate all dock window titles and drawer page titles. */
    void retranslateUi();

    /** Restore dock visibility from saved DockOptions config. */
    void restore_dock();

    // ---- Sidebar slot handlers ----

    void on_side_bar_dock_clicked(int index);
    void on_side_bar_action_clicked(int index);

    // ---- Config helper ----

    ::DockOptions *getDockOptions();

    // ---- Capture UI status ----

    /** Update toolbar and sidebar enabled/visible state based on the
     *  current capture status and work mode. Extracted from MainWindow
     *  during Phase 2 modernization (~85 lines). */
    void update_toolbar_view_status();

    // ---- Accessors ----

    dock::ProtocolDock      *protocol_widget()       { return _protocol_widget; }
    dock::TriggerDock       *trigger_widget()        { return _trigger_widget; }
    dock::DsoTriggerDock    *dso_trigger_widget()    { return _dso_trigger_widget; }
    dock::MeasureDock       *measure_widget()        { return _measure_widget; }
    dock::SearchDock        *search_widget()         { return _search_widget; }
    dock::DeviceOptionsDock *device_options_widget() { return _device_options_widget; }
    dock::LogDock           *log_widget()            { return _log_widget; }
    dock::McpControlDock    *mcp_control_widget()    { return _mcp_control_widget; }
    dock::FunctionDock      *function_widget()       { return _function_widget; }

    widgets::SlidingDrawer  *sliding_drawer()        { return _sliding_drawer; }
    widgets::SideBar        *side_bar()              { return _side_bar; }

    int drawer_current_page() const { return _drawer_current_page; }
    void set_drawer_current_page(int p) { _drawer_current_page = p; }

    // Drawer page index accessors
    int drawer_page_protocol() const { return _drawer_page_protocol; }
    int drawer_page_trigger() const { return _drawer_page_trigger; }
    int drawer_page_dso_trigger() const { return _drawer_page_dso_trigger; }
    int drawer_page_measure() const { return _drawer_page_measure; }
    int drawer_page_search() const { return _drawer_page_search; }
    int drawer_page_device_options() const { return _drawer_page_device_options; }
    int drawer_page_log() const { return _drawer_page_log; }
    int drawer_page_mcp() const { return _drawer_page_mcp; }
    int drawer_page_function() const { return _drawer_page_function; }

private:
    QPointer<MainWindow> _wnd;

    // Dock widgets (QDockWidget containers are kept for event-filter
    // compatibility even though content is shown via SlidingDrawer).
    QDockWidget             *_trigger_dock = nullptr;
    dock::TriggerDock       *_trigger_widget = nullptr;
    QDockWidget             *_dso_trigger_dock = nullptr;
    dock::DsoTriggerDock    *_dso_trigger_widget = nullptr;
    QDockWidget             *_protocol_dock = nullptr;
    dock::ProtocolDock      *_protocol_widget = nullptr;
    QDockWidget             *_measure_dock = nullptr;
    dock::MeasureDock       *_measure_widget = nullptr;
    QDockWidget             *_search_dock = nullptr;
    dock::SearchDock        *_search_widget = nullptr;
    QDockWidget             *_device_options_dock = nullptr;
    dock::DeviceOptionsDock *_device_options_widget = nullptr;
    QDockWidget             *_log_dock = nullptr;
    dock::LogDock           *_log_widget = nullptr;
    dock::McpControlDock    *_mcp_control_widget = nullptr;
    QDockWidget             *_function_dock = nullptr;
    dock::FunctionDock      *_function_widget = nullptr;

    // Sliding drawer + page indices
    widgets::SlidingDrawer  *_sliding_drawer = nullptr;
    int _drawer_page_protocol     = -1;
    int _drawer_page_trigger      = -1;
    int _drawer_page_dso_trigger  = -1;
    int _drawer_page_measure      = -1;
    int _drawer_page_search       = -1;
    int _drawer_page_device_options = -1;
    int _drawer_page_log          = -1;
    int _drawer_page_mcp          = -1;
    int _drawer_page_function     = -1;
    int _drawer_current_page      = -1; // -1 = no page open

    // Sidebar
    widgets::SideBar        *_side_bar = nullptr;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_DOCK_MANAGER_H
