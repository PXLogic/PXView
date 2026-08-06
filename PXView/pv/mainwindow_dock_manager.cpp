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

// mainwindow_dock_manager.cpp
// Phase 2: DockManager — extracted from MainWindow's dock management code.
// All dock widget creation, SlidingDrawer setup, SideBar management, and
// dock visibility logic live here. References to MainWindow members go
// through _wnd-> (friend access).

#include "mainwindow_dock_manager.h"

#include "mainwindow.h"

#include <QDockWidget>
#include <QObject>
#include <QScrollArea>

#include "appcontrol.h"
#include "config/appconfig.h"
#include "deviceagent.h"
#include "dock/deviceoptionsdock.h"
#include "dock/dsotriggerdock.h"
#include "dock/functiondock.h"
#include "dock/logdock.h"
#include "dock/mcpcontroldock.h"
#include "dock/measuredock.h"
#include "dock/protocoldock.h"
#include "dock/searchdock.h"
#include "dock/triggerdock.h"
#include "log.h"
#include "sigsession.h"
#include "tabcontext.h"
#include "toolbars/samplingbar.h"
#include "toolbars/trigbar.h"
#include "toolbars/filebar.h"
#include "ui/langresource.h"
#include "ui/string_ids.h"
#include "view/view.h"
#include "widgets/slidingdrawer.h"
#include "widgets/sidebar.h"
#include "widgets/smoothscrollarea.h"
#include "pxvdef.h"

namespace pv {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DockManager::DockManager(MainWindow *wnd) : _wnd(wnd) {}

DockManager::~DockManager() {
    // All QWidgets are children of MainWindow and will be deleted by Qt's
    // parent-child mechanism.
}

// ---------------------------------------------------------------------------
// create_docks — creates all dock widgets
// ---------------------------------------------------------------------------

void DockManager::create_docks(pv::view::View *initial_view) {
    SigSession *_session = _wnd->session();

    // ---- Trigger dock (logic analyzer) ----
    _trigger_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."),
        _wnd);
    _trigger_dock->setObjectName("trigger_dock");
    _trigger_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _trigger_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _trigger_dock->setVisible(false);
    _trigger_widget = new dock::TriggerDock(_trigger_dock, _session);
    _trigger_dock->setWidget(_trigger_widget);

    // ---- DSO Trigger dock ----
    _dso_trigger_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."),
        _wnd);
    _dso_trigger_dock->setObjectName("dso_trigger_dock");
    _dso_trigger_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _dso_trigger_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _dso_trigger_dock->setVisible(false);
    _dso_trigger_widget = new dock::DsoTriggerDock(_dso_trigger_dock, _session);
    _dso_trigger_dock->setWidget(_dso_trigger_widget);

    // ---- Protocol dock ----
    _protocol_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE), "Decode Protocol"),
        _wnd);
    _protocol_dock->setObjectName("protocol_dock");
    _protocol_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _protocol_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _protocol_dock->setVisible(false);
    _protocol_widget =
        new dock::ProtocolDock(_protocol_dock, initial_view, _session);
    _protocol_dock->setWidget(_protocol_widget);

    _session->set_decoder_pannel(_protocol_widget);

    // ---- Measure dock ----
    _measure_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"), _wnd);
    _measure_dock->setObjectName("measure_dock");
    _measure_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _measure_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _measure_dock->setVisible(false);
    _measure_widget =
        new dock::MeasureDock(_measure_dock, initial_view, _session);
    _measure_dock->setWidget(_measure_widget);

    // ---- Search dock ----
    _search_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."), _wnd);
    _search_dock->setObjectName("search_dock");
    _search_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _search_dock->setTitleBarWidget(new QWidget(_search_dock));
    _search_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _search_dock->setVisible(false);
    _search_widget = new dock::SearchDock(_search_dock, initial_view, _session);
    _search_dock->setWidget(_search_widget);

    // ---- Device Options dock (includes sampling settings) ----
    _device_options_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"), _wnd);
    _device_options_dock->setObjectName("device_options_dock");
    _device_options_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _device_options_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _device_options_dock->setVisible(false);
    _device_options_widget =
        new dock::DeviceOptionsDock(_device_options_dock, _session);

    QWidget *dock_container = new QWidget();
    QVBoxLayout *dock_lay = new QVBoxLayout(dock_container);
    dock_lay->setContentsMargins(0, 0, 0, 0);
    dock_lay->setSpacing(0);
    dock_lay->setSizeConstraint(QLayout::SetMinimumSize);
    QWidget *sampling_widget =
        _wnd->sampling_bar()->createSamplingSettingsWidget(dock_container);
    dock_lay->addWidget(sampling_widget);
    _device_options_widget->set_sampling_widget(sampling_widget);

    dock_lay->addWidget(_device_options_widget);

    // Wrap the entire dock_container in a SmoothScrollArea.
    pv::widgets::SmoothScrollArea *dock_scroll =
        new pv::widgets::SmoothScrollArea();
    dock_scroll->setWidgetResizable(true);
    dock_scroll->setFrameShape(QFrame::NoFrame);
    dock_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    dock_scroll->setWidget(dock_container);

    // Store the scroll area as a child so setup_drawer can use it.
    _device_options_dock->setProperty("dock_scroll",
                                      QVariant::fromValue(dock_scroll));

    QObject::connect(_device_options_widget, &dock::DeviceOptionsDock::settings_applied,
            _wnd, [this]() {
                if (_wnd->session()->have_view_data() == false)
                    _wnd->sampling_bar()->commit_settings();
                _wnd->sampling_bar()->update_sample_rate_list();
                _wnd->sampling_bar()->reload();
            });

    // ---- Log dock ----
    _log_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_DOCK_TITLE), "Log"), _wnd);
    _log_dock->setObjectName("log_dock");
    _log_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _log_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _log_dock->setVisible(false);
    _log_widget = new dock::LogDock(_log_dock);
    _log_dock->setWidget(_log_widget);

    // ---- MCP control dock ----
    _mcp_control_widget = new dock::McpControlDock(AppControl::Instance(), _wnd);

    // ---- Function dock (FFT / Math / Lissajous) ----
    _function_dock = new QDockWidget(
        L_S(STR_PAGE_DLG, S_ID(IDS_TOOLBAR_FUNCTION), "Function"), _wnd);
    _function_dock->setObjectName("function_dock");
    _function_dock->setFeatures(QDockWidget::DockWidgetMovable);
    _function_dock->setAllowedAreas(Qt::RightDockWidgetArea);
    _function_dock->setVisible(false);
    _function_widget = new dock::FunctionDock(_function_dock, _session);
    _function_dock->setWidget(_function_widget);

    // Do NOT add dock widgets to the main window layout.
    // They are hidden containers; content is shown via SlidingDrawer instead.
    _protocol_dock->setVisible(false);
    _trigger_dock->setVisible(false);
    _dso_trigger_dock->setVisible(false);
    _measure_dock->setVisible(false);
    _search_dock->setVisible(false);
    _device_options_dock->setVisible(false);
    _log_dock->setVisible(false);
    _function_dock->setVisible(false);
}

// ---------------------------------------------------------------------------
// setup_drawer — create SlidingDrawer and add pages
// ---------------------------------------------------------------------------

void DockManager::setup_drawer(QWidget *central_widget, QVBoxLayout *layout) {
    _sliding_drawer = new widgets::SlidingDrawer(central_widget);
    _sliding_drawer->setDrawerWidth(350);
    _sliding_drawer->setAnimationDuration(300);
    _sliding_drawer->setPushLayout(layout);

    // Take content widgets out of QDockWidget and add to SlidingDrawer
    // Protocol
    _protocol_dock->setWidget(nullptr);
    _drawer_page_protocol = _sliding_drawer->addPage(
        _protocol_widget,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE), "Decode Protocol"));

    // Trigger (logic analyzer)
    _trigger_dock->setWidget(nullptr);
    _drawer_page_trigger = _sliding_drawer->addPage(
        _trigger_widget, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE),
                             "Trigger Setting..."));

    // DSO Trigger
    _dso_trigger_dock->setWidget(nullptr);
    _drawer_page_dso_trigger = _sliding_drawer->addPage(
        _dso_trigger_widget, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE),
                                 "Trigger Setting..."));

    // Measure
    _measure_dock->setWidget(nullptr);
    _drawer_page_measure = _sliding_drawer->addPage(
        _measure_widget,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"));

    // Search
    _search_dock->setWidget(nullptr);
    _drawer_page_search = _sliding_drawer->addPage(
        _search_widget,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."));

    // Device Options (includes sampling settings) — use the scroll area
    _device_options_dock->setWidget(nullptr);
    auto scroll_var = _device_options_dock->property("dock_scroll");
    QWidget *dock_scroll = scroll_var.value<QWidget *>();
    _drawer_page_device_options = _sliding_drawer->addPage(
        dock_scroll,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"));

    // Log
    _log_dock->setWidget(nullptr);
    _drawer_page_log = _sliding_drawer->addPage(
        _log_widget, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_DOCK_TITLE), "Log"));

    // MCP Server
    _drawer_page_mcp = _sliding_drawer->addPage(
        _mcp_control_widget,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MCP_DOCK_TITLE), "MCP Server"));

    // Function (FFT / Math / Lissajous)
    _function_dock->setWidget(nullptr);
    _drawer_page_function = _sliding_drawer->addPage(
        _function_widget,
        L_S(STR_PAGE_DLG, S_ID(IDS_TOOLBAR_FUNCTION), "Function"));

    _drawer_current_page = -1;
}

// ---------------------------------------------------------------------------
// setup_side_bar — create sidebar items
// ---------------------------------------------------------------------------

void DockManager::setup_side_bar() {
    _side_bar = new widgets::SideBar(_wnd);

    _side_bar->addItem("zap.svg", S_ID(IDS_TOOLBAR_TRIGGER), "Trigger",
                       widgets::SideBar::DockItem, _drawer_page_trigger);
    _side_bar->addItem("binary.svg", S_ID(IDS_TOOLBAR_DECODE), "Decode",
                       widgets::SideBar::DockItem, _drawer_page_protocol);
    _side_bar->addItem("ruler.svg", S_ID(IDS_TOOLBAR_MEASURE), "Measure",
                       widgets::SideBar::DockItem, _drawer_page_measure);
    _side_bar->addItem("search.svg", S_ID(IDS_TOOLBAR_SEARCH), "Search",
                       widgets::SideBar::DockItem, _drawer_page_search);
    _side_bar->addItem("function.svg", S_ID(IDS_TOOLBAR_FUNCTION), "Function",
                       widgets::SideBar::DockItem, _drawer_page_function);
    _side_bar->addItem("sliders.svg", S_ID(IDS_TOOLBAR_DEVICE_OPTION), "Options",
                       widgets::SideBar::DockItem, _drawer_page_device_options);
    _side_bar->addItem("workflow.svg", S_ID(IDS_TOOLBAR_MCP), "MCP",
                       widgets::SideBar::DockItem, _drawer_page_mcp);
    _side_bar->addItem("scroll-text.svg", S_ID(IDS_TOOLBAR_LOG), "Log",
                       widgets::SideBar::DockItem, _drawer_page_log);
    _side_bar->addSeparator();
    _side_bar->addItem("play.svg", S_ID(IDS_TOOLBAR_RUN_START), "Start",
                       widgets::SideBar::ActionItem, -1, "stop.svg");
    _side_bar->addItem("step-forward.svg", S_ID(IDS_TOOLBAR_ONE_INSTANT),
                       "Instant", widgets::SideBar::ActionItem, -1, "stop.svg");

    _wnd->addToolBar(Qt::RightToolBarArea, _side_bar);
}

// ---------------------------------------------------------------------------
// setup_connections — drawer and sidebar signal connections
// ---------------------------------------------------------------------------

void DockManager::setup_connections() {
    // SideBar connections
    QObject::connect(_side_bar, &widgets::SideBar::dockItemClicked, _wnd,
            &MainWindow::on_side_bar_dock_clicked);
    QObject::connect(_side_bar, &widgets::SideBar::actionItemClicked, _wnd,
            &MainWindow::on_side_bar_action_clicked);

    // When drawer closes, update toolbar state
    QObject::connect(_sliding_drawer, &widgets::SlidingDrawer::drawerClosed, _wnd,
            [this]() {
                _drawer_current_page = -1;
                _side_bar->clearAllChecked();
                _wnd->current_view()->show_search_cursor(false);
                ::DockOptions *opt = getDockOptions();
                if (opt) {
                    opt->decodeDock = false;
                    opt->triggerDock = false;
                    opt->measureDock = false;
                    opt->searchDock = false;
                    opt->deviceOptionsDock = false;
                    AppConfig::Instance().SaveFrame();
                }
                _wnd->current_view()->setFocus();
            });

    QObject::connect(_sliding_drawer, &widgets::SlidingDrawer::drawerOpened, _wnd,
            [this](int page) {
                QWidget *content = _sliding_drawer->page(page);
                if (content) {
                    QWidget *focus_target = content;
                    QScrollArea *scroll = qobject_cast<QScrollArea *>(content);
                    if (scroll && scroll->widget()) {
                        focus_target = scroll->widget();
                    }
                    QWidget *first_focusable = nullptr;
                    QWidget *prev = focus_target;
                    while (prev) {
                        QWidget *next = prev->nextInFocusChain();
                        if (!next || next == focus_target)
                            break;
                        if (next->isVisible() && next->isEnabled() &&
                            next->focusPolicy() & Qt::TabFocus) {
                            if (_sliding_drawer->isAncestorOf(next)) {
                                first_focusable = next;
                                break;
                            }
                        }
                        prev = next;
                    }
                    if (first_focusable)
                        first_focusable->setFocus();
                    else
                        content->setFocus();
                }
            });

    QObject::connect(_sliding_drawer, &widgets::SlidingDrawer::drawerDragFinished, _wnd,
            [this]() {
                if (_wnd->current_view()) {
                    _wnd->current_view()->limit_scale_offset();
                }
            });
}

// ---------------------------------------------------------------------------
// install_event_filters
// ---------------------------------------------------------------------------

void DockManager::install_event_filters(QObject *filter_obj) {
    _dso_trigger_dock->installEventFilter(filter_obj);
    _trigger_dock->installEventFilter(filter_obj);
    _protocol_dock->installEventFilter(filter_obj);
    _measure_dock->installEventFilter(filter_obj);
    _search_dock->installEventFilter(filter_obj);
    _device_options_dock->installEventFilter(filter_obj);
    _sliding_drawer->installEventFilter(filter_obj);
}

// ---------------------------------------------------------------------------
// bind_context / unbind_context / set_view
// ---------------------------------------------------------------------------

void DockManager::bind_context(pv::TabContext *ctx) {
    _measure_widget->bind_context(ctx);
    _search_widget->bind_context(ctx);
    _protocol_widget->bind_context(ctx);
    _device_options_widget->bind_context(ctx);
    _log_widget->bind_context(ctx);
    _trigger_widget->bind_context(ctx);
    _dso_trigger_widget->bind_context(ctx);
}

void DockManager::unbind_context() {
    _measure_widget->unbind_context();
    _search_widget->unbind_context();
    _protocol_widget->unbind_context();
    _device_options_widget->unbind_context();
    _log_widget->unbind_context();
    _trigger_widget->unbind_context();
    _dso_trigger_widget->unbind_context();
}

void DockManager::set_view(pv::view::View *view) {
    _measure_widget->set_view(view);
    _search_widget->set_view(view);
    _protocol_widget->set_view(view);
}

// ---------------------------------------------------------------------------
// retranslateUi
// ---------------------------------------------------------------------------

void DockManager::retranslateUi() {
    _trigger_dock->setWindowTitle(L_S(
        STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."));
    _dso_trigger_dock->setWindowTitle(L_S(
        STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."));
    _protocol_dock->setWindowTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE), "Decode Protocol"));
    _measure_dock->setWindowTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"));
    _search_dock->setWindowTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."));
    _device_options_dock->setWindowTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"));
    _log_dock->setWindowTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_DOCK_TITLE), "Log"));

    // Update drawer page titles
    if (_sliding_drawer) {
        _sliding_drawer->setPageTitle(_drawer_page_protocol,
                                      L_S(STR_PAGE_DLG,
                                          S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE),
                                          "Decode Protocol"));
        _sliding_drawer->setPageTitle(_drawer_page_trigger,
                                      L_S(STR_PAGE_DLG,
                                          S_ID(IDS_DLG_TRIGGER_DOCK_TITLE),
                                          "Trigger Setting..."));
        _sliding_drawer->setPageTitle(_drawer_page_dso_trigger,
                                      L_S(STR_PAGE_DLG,
                                          S_ID(IDS_DLG_TRIGGER_DOCK_TITLE),
                                          "Trigger Setting..."));
        _sliding_drawer->setPageTitle(
            _drawer_page_measure,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"));
        _sliding_drawer->setPageTitle(
            _drawer_page_search,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."));
        _sliding_drawer->setPageTitle(
            _drawer_page_device_options,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"));
        _sliding_drawer->setPageTitle(
            _drawer_page_log,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_DOCK_TITLE), "Log"));
        _sliding_drawer->setPageTitle(
            _drawer_page_function,
            L_S(STR_PAGE_DLG, S_ID(IDS_TOOLBAR_FUNCTION), "Function"));
    }
}

// ---------------------------------------------------------------------------
// restore_dock
// ---------------------------------------------------------------------------

void DockManager::restore_dock() {
    if (_wnd->device_agent()->have_instance())
        _wnd->trig_bar()->reload();

    _side_bar->clearAllChecked();

    ::DockOptions *opt = getDockOptions();
    if (opt) {
        if (opt->decodeDock) {
            _side_bar->setItemChecked(MainWindow::SIDEBAR_DECODE, true);
            _sliding_drawer->open(_drawer_page_protocol);
            _drawer_current_page = _drawer_page_protocol;
        } else if (opt->triggerDock) {
            _side_bar->setItemChecked(MainWindow::SIDEBAR_TRIGGER, true);
            int mode = _wnd->device_agent()->get_work_mode();
            if (mode != DSO) {
                _trigger_widget->update_view();
                _sliding_drawer->open(_drawer_page_trigger);
                _drawer_current_page = _drawer_page_trigger;
            } else {
                _dso_trigger_widget->update_view();
                _sliding_drawer->open(_drawer_page_dso_trigger);
                _drawer_current_page = _drawer_page_dso_trigger;
            }
        } else if (opt->measureDock) {
            _side_bar->setItemChecked(MainWindow::SIDEBAR_MEASURE, true);
            _sliding_drawer->open(_drawer_page_measure);
            _drawer_current_page = _drawer_page_measure;
        } else if (opt->searchDock) {
            _side_bar->setItemChecked(MainWindow::SIDEBAR_SEARCH, true);
            _wnd->current_view()->show_search_cursor(true);
            _sliding_drawer->open(_drawer_page_search);
            _drawer_current_page = _drawer_page_search;
        } else if (opt->deviceOptionsDock) {
            _side_bar->setItemChecked(MainWindow::SIDEBAR_OPTIONS, true);
            _sliding_drawer->open(_drawer_page_device_options);
            _drawer_current_page = _drawer_page_device_options;
        } else if (opt->logDock) {
            _side_bar->setItemChecked(MainWindow::SIDEBAR_LOG, true);
            _sliding_drawer->open(_drawer_page_log);
            _drawer_current_page = _drawer_page_log;
        }
    }
}

// ---------------------------------------------------------------------------
// on_side_bar_dock_clicked
// ---------------------------------------------------------------------------

void DockManager::on_side_bar_dock_clicked(int index) {
    bool isChecked = _side_bar->getItem(index)->button->isChecked();

    if (!isChecked) {
        if (_sliding_drawer->isOpen())
            _sliding_drawer->close();
        _wnd->current_view()->show_search_cursor(false);
        ::DockOptions *opt = getDockOptions();
        if (opt) {
            opt->decodeDock = false;
            opt->triggerDock = false;
            opt->measureDock = false;
            opt->searchDock = false;
            opt->deviceOptionsDock = false;
            opt->logDock = false;
            AppConfig::Instance().SaveFrame();
        }
        _wnd->current_view()->setFocus();
        return;
    }

    int drawerPage = -1;

    switch (index) {
    case MainWindow::SIDEBAR_TRIGGER:
        if (_wnd->device_agent()->get_work_mode() != DSO) {
            _trigger_widget->update_view();
            drawerPage = _drawer_page_trigger;
        } else {
            _dso_trigger_widget->update_view();
            drawerPage = _drawer_page_dso_trigger;
        }
        break;
    case MainWindow::SIDEBAR_DECODE:
        drawerPage = _drawer_page_protocol;
        break;
    case MainWindow::SIDEBAR_MEASURE:
        drawerPage = _drawer_page_measure;
        break;
    case MainWindow::SIDEBAR_SEARCH:
        _wnd->current_view()->show_search_cursor(true);
        drawerPage = _drawer_page_search;
        break;
    case MainWindow::SIDEBAR_FUNCTION:
        _function_widget->reload();
        drawerPage = _drawer_page_function;
        break;
    case MainWindow::SIDEBAR_OPTIONS:
        /* Don't call update_view() here — it does a full nuke-and-rebuild that
         * can lose Mode section selections (e.g. SR_CONF_FILTER resets to
         * default because the Enum combo box defaults to index 0 when the
         * driver getter returns nullptr during rebuild). The UI is already
         * built; mode_check_timeout() timer detects operation_mode changes
         * and rebuilds when needed. */
        drawerPage = _drawer_page_device_options;
        break;
    case MainWindow::SIDEBAR_MCP:
        _mcp_control_widget->refresh_status();
        drawerPage = _drawer_page_mcp;
        break;
    case MainWindow::SIDEBAR_LOG:
        drawerPage = _drawer_page_log;
        break;
    }

    if (drawerPage >= 0) {
        _sliding_drawer->open(drawerPage);
        _drawer_current_page = drawerPage;
    } else if (_sliding_drawer->isOpen()) {
        _sliding_drawer->close();
    }

    ::DockOptions *opt = getDockOptions();
    if (opt) {
        opt->decodeDock = (index == MainWindow::SIDEBAR_DECODE);
        opt->triggerDock = (index == MainWindow::SIDEBAR_TRIGGER);
        opt->measureDock = (index == MainWindow::SIDEBAR_MEASURE);
        opt->searchDock = (index == MainWindow::SIDEBAR_SEARCH);
        opt->deviceOptionsDock = (index == MainWindow::SIDEBAR_OPTIONS);
        opt->logDock = (index == MainWindow::SIDEBAR_LOG);
        AppConfig::Instance().SaveFrame();
    }

    _wnd->current_view()->setFocus();
}

// ---------------------------------------------------------------------------
// on_side_bar_action_clicked
// ---------------------------------------------------------------------------

void DockManager::on_side_bar_action_clicked(int index) {
    switch (index) {
    case MainWindow::SIDEBAR_RUNSTOP:
        if (_wnd->session()->is_working()) {
            _wnd->session()->stop_capture();
        } else {
            _wnd->sampling_bar()->run_or_stop();
        }
        break;
    case MainWindow::SIDEBAR_INSTANT:
        if (_wnd->session()->is_working() && _wnd->session()->is_instant()) {
            _wnd->session()->stop_capture();
        } else {
            _wnd->sampling_bar()->run_or_stop_instant();
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// getDockOptions
// ---------------------------------------------------------------------------

::DockOptions *DockManager::getDockOptions() {
    AppConfig &app = AppConfig::Instance();
    int mode = _wnd->device_agent()->get_work_mode();
    if (mode == LOGIC)
        return &app.frameOptions._logicDock;
    else if (mode == DSO)
        return &app.frameOptions._dsoDock;
    else
        return &app.frameOptions._analogDock;
}

// ---------------------------------------------------------------------------
// update_toolbar_view_status — extracted from MainWindow (Phase 2).
// Updates the enabled and visible state of all sidebar items and toolbars
// based on the current capture status (is_working) and work mode
// (LOGIC / ANALOG / DSO). Also closes the SlidingDrawer if the currently
// open page belongs to a sidebar item that has become invisible.
// ---------------------------------------------------------------------------
void DockManager::update_toolbar_view_status() {
    _wnd->sampling_bar()->update_view_status();
    _wnd->file_bar()->update_view_status();
    _wnd->trig_bar()->update_view_status();

    bool bEnable = _wnd->session()->is_working() == false;
    int mode = _wnd->device_agent()->get_work_mode();

    _side_bar->setItemEnabled(_wnd->SIDEBAR_TRIGGER, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_DECODE, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_MEASURE, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_SEARCH, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_FUNCTION, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_OPTIONS, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_MCP, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_LOG, bEnable);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_RUNSTOP, true);
    _side_bar->setItemEnabled(_wnd->SIDEBAR_INSTANT, true);

    if (_wnd->session()->is_working() && mode == DSO) {
        if (_wnd->session()->is_instant() == false) {
            _side_bar->setItemEnabled(_wnd->SIDEBAR_TRIGGER, true);
            _side_bar->setItemEnabled(_wnd->SIDEBAR_MEASURE, true);
            _side_bar->setItemEnabled(_wnd->SIDEBAR_FUNCTION, true);
            _side_bar->setItemEnabled(_wnd->SIDEBAR_OPTIONS, true);
        }
    }

    if (mode == LOGIC) {
        _side_bar->setItemVisible(_wnd->SIDEBAR_TRIGGER, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_DECODE, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_MEASURE, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_SEARCH, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_FUNCTION, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_OPTIONS, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_MCP, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_LOG, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_RUNSTOP, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_INSTANT, true);
    } else if (mode == ANALOG) {
        _side_bar->setItemVisible(_wnd->SIDEBAR_TRIGGER, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_DECODE, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_MEASURE, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_SEARCH, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_FUNCTION, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_OPTIONS, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_MCP, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_LOG, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_RUNSTOP, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_INSTANT, false);
    } else if (mode == DSO) {
        _side_bar->setItemVisible(_wnd->SIDEBAR_TRIGGER, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_DECODE, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_MEASURE, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_SEARCH, false);
        _side_bar->setItemVisible(_wnd->SIDEBAR_FUNCTION, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_OPTIONS, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_MCP, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_LOG, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_RUNSTOP, true);
        _side_bar->setItemVisible(_wnd->SIDEBAR_INSTANT, true);
    }

    /* If the currently-open drawer page belongs to a sidebar item that is
     * now invisible (e.g. switching DSO→ANALOG hides SIDEBAR_TRIGGER while
     * the DsoTriggerDock drawer is still open), close the drawer so the user
     * doesn't see stale content from the previous mode. Without this, the
     * drawer remains open but the sidebar button to close it is invisible. */
    if (_sliding_drawer && _sliding_drawer->isOpen()) {
        int cp = _drawer_current_page;
        bool should_close = false;
        if (cp == _drawer_page_trigger || cp == _drawer_page_dso_trigger)
            should_close = !_side_bar->isItemVisible(_wnd->SIDEBAR_TRIGGER);
        else if (cp == _drawer_page_protocol)
            should_close = !_side_bar->isItemVisible(_wnd->SIDEBAR_DECODE);
        else if (cp == _drawer_page_search)
            should_close = !_side_bar->isItemVisible(_wnd->SIDEBAR_SEARCH);
        else if (cp == _drawer_page_function)
            should_close = !_side_bar->isItemVisible(_wnd->SIDEBAR_FUNCTION);
        if (should_close) {
            _sliding_drawer->close();
            _side_bar->clearAllChecked();
            _drawer_current_page = -1;
        }
    }
}

} // namespace pv
