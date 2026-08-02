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

#include "widgets/searchpatterninput.h"
#include "widgets/sidebar.h"
#include "widgets/smoothscrollarea.h"
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>
#include <algorithm>
#include <functional>
#include <libusb-1.0/libusb.h>
#include <stdexcept>

#include "log.h"
#include "mainwindow.h"

#include "data/analogsnapshot.h"
#include "data/dsosnapshot.h"
#include "data/logicsnapshot.h"

#include "dialogs/about.h"
#include "dialogs/deviceoptions.h"
#include "dialogs/regionoptions.h"
#include "dialogs/storeprogress.h"

#include "toolbars/filebar.h"
#include "toolbars/logobar.h"
#include "toolbars/samplingbar.h"
#include "toolbars/titlebar.h"
#include "toolbars/trigbar.h"

#include "dock/deviceoptionsdock.h"
#include "dock/logdock.h"
#include "dock/mcpcontroldock.h"
#include "dock/functiondock.h"
#include "dock/measuredock.h"
#include "dock/protocoldock.h"
#include "dock/searchdock.h"
#include "dock/dsotriggerdock.h"
#include "dock/triggerdock.h"


#include "data/decoderstack.h"
#include "data/sessiondocument.h"
#include "core/documentregistry.h"
#include "interface/icontextaware.h"
#include "sessionmanager.h"
#include "tabcontext.h"
#include "ui/draggabletabwidget.h"
#include "view/analogsignal.h"
#include "view/dsosignal.h"
#include "view/logicsignal.h"
#include "view/signal.h"
#include "view/trace.h"
#include "view/view.h"
#include "view/viewstatus.h"
#include "view/viewport.h"

/* __STDC_FORMAT_MACROS is required for PRIu64 and friends (in C++). */
#include "ZipMaker.h"
#include "api/app_service.h"
#include "appcontrol.h"
#include "config/appconfig.h"
#include "config/shortcutdefs.h"
#include "deviceagent.h"
#include "pxvdef.h"
#include "log.h"
#include "mainframe.h"
#include "sigsession.h"
#include "ui/langresource.h"
#include "ui/msgbox.h"
#include "ui/uimanager.h"
#include "utility/encoding.h"
#include "utility/path.h"
#include <glib.h>
#include <inttypes.h>
#include <list>
#include <stdarg.h>
#include <cstdint>
#include <cstdlib>
#include <thread>

#ifdef ENABLE_DEBUG_HELPER
#include "ui/widgetinspector.h"
#endif

#include <QShortcut>
#include <QWidgetAction>

#include <QLabel>
#include <QScrollArea>
#include <QTabBar>
#include <map>

// The Windows SDK (pulled in transitively via mainframe.h -> wintaskbarprogress.h
// -> shobjidl.h, included below mainwindow.h) defines `interface` as a
// preprocessor macro. events.h (included via mainwindow.h) clears it, but only
// if it was defined at that point — in this TU mainframe.h is included AFTER
// mainwindow.h, so the macro is defined after events.h's #undef runs. Clear it
// again here so `pv::interface::` qualified names in the code below parse
// correctly. PXView does not use the `interface` COM macro anywhere.
#ifdef interface
#  undef interface
#endif

namespace pv {

namespace {
QString tmp_file;

/** Build a channel-index → ChannelLayoutState map from the View's signal list.
 * Task 7 (unify-signal-layout-state): persists per-signal UI layout so the
 * session can restore view_index / v_offset / own_height after reload. */
std::map<int, pv::data::ChannelLayoutState>
build_channel_layout(pv::view::View *view) {
  std::map<int, pv::data::ChannelLayoutState> layout;
  if (view) {
    for (auto *sig : view->get_own_signals()) {
      pv::data::ChannelLayoutState s;
      s.view_index = sig->get_view_index();
      s.v_offset = sig->get_v_offset();
      s.own_height = sig->get_own_height();
      layout[sig->get_index()] = s;
    }
  }
  return layout;
}

/** Build a channel-index → colour-string map from the View's signal list.
 * Task 3 (purify-architecture-concepts): collects per-signal colour so
 * SignalConfigStore can serialize it as the single .pxc channel config path
 * (replaces the old MainWindow::gen_config_json direct view::Signal access).
 * Returns QColor::name() (hex "#RRGGBB") or "default" when invalid. */
std::map<int, std::string>
build_channel_colours(pv::view::View *view) {
  std::map<int, std::string> colours;
  if (view) {
    for (auto *sig : view->get_own_signals()) {
      QColor c = sig->get_colour();
      colours[sig->get_index()] = c.isValid() ? c.name().toStdString() : "default";
    }
  }
  return colours;
}
} // namespace

void MainWindow::MainWindowRibbonHelper() {
  _category_file_index = _title_bar->addCategory(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE), "File"));
  _category_display_index = _title_bar->addCategory(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DISPLAY), "Settings"));
  _category_help_index = _title_bar->addCategory(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP), "Help"));
}

void MainWindow::Ribbon_setupUi() {
  setupFileCategory();
  setupDisplayCategory();
  setupHelpCategory();
}
// void MainWindow::setupQuickAccessBar()
// {

// }

void MainWindow::setupSideBar() {
  _side_bar = new widgets::SideBar(this);

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

  addToolBar(Qt::RightToolBarArea, _side_bar);

  connect(_side_bar, &widgets::SideBar::dockItemClicked, this,
          &MainWindow::on_side_bar_dock_clicked);
  connect(_side_bar, &widgets::SideBar::actionItemClicked, this,
          &MainWindow::on_side_bar_action_clicked);
}

void MainWindow::setupFileCategory() {
  _title_bar->addAction(_category_file_index, _file_bar->_action_load);
  _title_bar->addAction(_category_file_index, _file_bar->_action_store);
  _title_bar->addAction(_category_file_index, _file_bar->_action_default);

  _title_bar->addSeparator(_category_file_index);

  _title_bar->addAction(_category_file_index, _file_bar->_action_open);
  _title_bar->addAction(_category_file_index, _file_bar->_action_save);
  _title_bar->addSeparator(_category_file_index);

  _title_bar->addAction(_category_file_index, _file_bar->_action_export);
  _title_bar->addAction(_category_file_index, _file_bar->_action_import);
  _title_bar->addAction(_category_file_index, _file_bar->_action_capture);
}

void MainWindow::setupDisplayCategory() {
  _title_bar->addAction(_category_display_index, _logo_bar->_action_cn);
  _title_bar->addAction(_category_display_index,
                        _logo_bar->_action_traditional);
  _title_bar->addAction(_category_display_index, _logo_bar->_action_en);

  _title_bar->addSeparator(_category_display_index);

  _title_bar->addAction(_category_display_index,
                        _trig_bar->_action_dispalyOptions);
}

void MainWindow::setupHelpCategory() {
  _title_bar->addAction(_category_help_index, _logo_bar->_about);
  _title_bar->addAction(_category_help_index, _logo_bar->_manual);
  _title_bar->addAction(_category_help_index, _logo_bar->_issue);
  _title_bar->addAction(_category_help_index, _logo_bar->_update);
}

void MainWindow::Ribbon_retranslateUi() {
  if (_title_bar) {
    _title_bar->retranslateUi(
        _category_file_index,
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE), "File"));
    _title_bar->retranslateUi(
        _category_display_index,
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_DISPLAY), "Settings"));
    _title_bar->retranslateUi(
        _category_help_index,
        L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP), "Help"));
  }
}

MainWindow::MainWindow(toolbars::TitleBar *title_bar, QWidget *parent)
    : QMainWindow(parent) {
  pxv_info("DBG MainWindow::MainWindow() START");
  _msg = nullptr;
  _frame = parent;
  _category_file_index = -1;
  _category_display_index = -1;
  _category_help_index = -1;

  if (!title_bar) {
    pxv_warn("%s", "MainWindow::MainWindow: title_bar is nullptr");
    throw std::invalid_argument("MainWindow: title_bar is nullptr");
  }
  if (!_frame) {
    pxv_warn("%s", "MainWindow::MainWindow: _frame is nullptr");
    throw std::invalid_argument("MainWindow: _frame is nullptr");
  }
  assert(title_bar);
  assert(_frame);

  _title_bar = title_bar;

  _session = ::AppControl::Instance()->GetSession();
  _session->add_callback(this);
  _device_agent = _session->get_device();
  // Register as a typed event listener for all notification events.
  _session->add_event_listener(this);

  _is_auto_switch_device = false;
  _is_save_confirm_msg = false;
  _disk_cache_status_label = nullptr;
  _trig_time_label = nullptr;
  _sample_period_label = nullptr;

  _pattern_mode = "random";
  setup_ui();
  setMenuBar(nullptr);

  setContextMenuPolicy(Qt::NoContextMenu);

  _key_vaild = false;
  _last_key_press_time = high_resolution_clock::now();

  update_title_bar_text();

  // Register new-tab callback with AppService so MCP API can create tabs
  auto *app_svc = ::AppControl::Instance()->GetAppService();
  if (app_svc) {
    auto *concrete = dynamic_cast<pv::api::AppService *>(app_svc);
    if (concrete) {
      concrete->set_new_tab_callback([this]() { on_new_tab_requested(); });
    }
  }
}

MainWindow::~MainWindow() {
  // B1.2: unregister the typed event listener before destruction. The
  // SigSession outlives this MainWindow (it is owned by AppControl), so
  // failing to unregister would leave a dangling pointer in the listener
  // vector.
  if (_session) {
    _session->remove_event_listener(this);
  }
}

void MainWindow::setup_ui() {
  setObjectName(QString::fromUtf8("MainWindow"));
  setContentsMargins(0, 0, 0, 0);
  layout()->setSpacing(0);

  // Setup the central widget
  _central_widget = new QWidget(this);
  _vertical_layout = new QVBoxLayout(_central_widget);
  _vertical_layout->setSpacing(0);
  _vertical_layout->setContentsMargins(0, 0, 0, 0);
  setCentralWidget(_central_widget);

  // Setup the sampling bar
  _sampling_bar = new toolbars::SamplingBar(_session, this);
  _sampling_bar->setObjectName("sampling_bar");
  _trig_bar = new toolbars::TrigBar(_session, this);
  _trig_bar->setObjectName("trig_bar");
  _file_bar = new toolbars::FileBar(_session, this);
  _file_bar->setObjectName("file_bar");
  _logo_bar = new toolbars::LogoBar(_session, this);
  _logo_bar->setObjectName("logo_bar");

  _sampling_bar->setAllowedAreas(Qt::RightToolBarArea);
  _sampling_bar->hide();
  _trig_bar->setFloatable(false);
  _trig_bar->hide();
  _file_bar->setFloatable(false);
  _file_bar->hide();
  _logo_bar->setFloatable(false);
  _logo_bar->hide();

  // trigger dock
  _trigger_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."),
      this);
  _trigger_dock->setObjectName("trigger_dock");
  _trigger_dock->setFeatures(QDockWidget::DockWidgetMovable);
  _trigger_dock->setAllowedAreas(Qt::RightDockWidgetArea);
  _trigger_dock->setVisible(false);
  _trigger_widget = new dock::TriggerDock(_trigger_dock, _session);
  _trigger_dock->setWidget(_trigger_widget);

  _dso_trigger_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_DOCK_TITLE), "Trigger Setting..."),
      this);
  _dso_trigger_dock->setObjectName("dso_trigger_dock");
  _dso_trigger_dock->setFeatures(QDockWidget::DockWidgetMovable);
  _dso_trigger_dock->setAllowedAreas(Qt::RightDockWidgetArea);
  _dso_trigger_dock->setVisible(false);
  _dso_trigger_widget = new dock::DsoTriggerDock(_dso_trigger_dock, _session);
  _dso_trigger_dock->setWidget(_dso_trigger_widget);

  _tab_widget = new pv::ui::DraggableTabWidget(this);
  _vertical_layout->addWidget(_tab_widget);

  pv::view::View *initial_view =
      new pv::view::View(_session, _sampling_bar, this);
  // phase 2: document ownership moved into DocumentRegistry. take_document
  // returns a stable index; get_document_by_index yields a weak pointer.
  size_t initial_doc_idx = _session->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_session));
  pv::data::SessionDocument *initial_doc =
      _session->document_registry()->get_document_by_index(initial_doc_idx);

  if (_device_agent && _device_agent->have_instance()) {
    initial_doc->save_signal_config(_session->get_signal_models(), {});
    pxv_info("MainWindow::setup_ui() saved initial signal config, mode=%d "
             "ch_count=%d",
             initial_doc->get_signal_config().work_mode,
             (int)initial_doc->get_signal_config().channels.size());
  }

  pv::TabContext *initial_ctx = SessionManager::instance()->create_context(
      initial_view, _session, initial_doc, initial_doc_idx,
      _session->document_registry());
  initial_ctx->set_title(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE), "File"));
  _tab_contexts.append(initial_ctx);
  qDebug() << "MainWindow::setup_ui() before addTab, initial_doc="
           << initial_doc << "has_config=" << initial_doc->has_signal_config();
  pxv_info("DBG before addTab has_config=%d", initial_doc->has_signal_config());
  _tab_widget->addTab(initial_view, initial_ctx->title());
  pxv_info("DBG after addTab");
  fprintf(stderr, "DBG MainWindow::setup_ui() after addTab\n");
  fflush(stderr);
  _current_tab_index = 0;

  initial_ctx->activate();

  // setIconSize(QSize(40, 40));
  // addToolBar(Qt::TopToolBarArea, _sampling_bar);  // moved into
  // device_options_dock addToolBar(_trig_bar); addToolBar(_file_bar);
  // addToolBar(_logo_bar);

  MainWindowRibbonHelper();
  Ribbon_setupUi();
  setIconSize(QSize(16, 16));
  // addToolBar(Qt::TopToolBarArea,_sampling_bar);
  // addToolBar(Qt::LeftToolBarArea,_trig_bar);
  // addToolBar(Qt::LeftToolBarArea,_file_bar);
  // addToolBar(Qt::LeftToolBarArea, _logo_bar);

  // Setup the dockWidget
  _protocol_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PROTOCOL_DOCK_TITLE), "Decode Protocol"),
      this);
  _protocol_dock->setObjectName("protocol_dock");
  _protocol_dock->setFeatures(QDockWidget::DockWidgetMovable);
  _protocol_dock->setAllowedAreas(Qt::RightDockWidgetArea);
  _protocol_dock->setVisible(false);
  _protocol_widget =
      new dock::ProtocolDock(_protocol_dock, initial_view, _session);
  _protocol_dock->setWidget(_protocol_widget);

  _session->set_decoder_pannel(_protocol_widget);

  // measure dock
  _measure_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_MEASURE_DOCK_TITLE), "Measurement"), this);
  _measure_dock->setObjectName("measure_dock");
  _measure_dock->setFeatures(QDockWidget::DockWidgetMovable);
  _measure_dock->setAllowedAreas(Qt::RightDockWidgetArea);
  _measure_dock->setVisible(false);
  _measure_widget =
      new dock::MeasureDock(_measure_dock, initial_view, _session);
  _measure_dock->setWidget(_measure_widget);

  // search dock
  _search_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SEARCH_DOCK_TITLE), "Search..."), this);
  _search_dock->setObjectName("search_dock");
  // _search_dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  _search_dock->setFeatures(QDockWidget::DockWidgetMovable);
  _search_dock->setTitleBarWidget(new QWidget(_search_dock));
  // _search_dock->setAllowedAreas(Qt::BottomDockWidgetArea);
  _search_dock->setAllowedAreas(Qt::RightDockWidgetArea);
  _search_dock->setVisible(false);

  _search_widget = new dock::SearchDock(_search_dock, initial_view, _session);
  _search_dock->setWidget(_search_widget);

  _device_options_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DEVICE_OPTIONS), "Device Options"), this);
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
      _sampling_bar->createSamplingSettingsWidget(dock_container);
  dock_lay->addWidget(sampling_widget);
  _device_options_widget->set_sampling_widget(sampling_widget);

  dock_lay->addWidget(_device_options_widget);

  // Wrap the entire dock_container (sampling bar + device options) in a
  // SmoothScrollArea. This provides smooth scrolling animation.
  pv::widgets::SmoothScrollArea *dock_scroll =
      new pv::widgets::SmoothScrollArea();
  dock_scroll->setWidgetResizable(true);
  dock_scroll->setFrameShape(QFrame::NoFrame);
  dock_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  dock_scroll->setWidget(dock_container);

  connect(_device_options_widget, &dock::DeviceOptionsDock::settings_applied,
          this, [this]() {
            if (_session->have_view_data() == false)
              _sampling_bar->commit_settings();
            _sampling_bar->update_sample_rate_list();
            _sampling_bar->reload();
          });

  // log dock
  _log_dock = new QDockWidget(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_DOCK_TITLE), "Log"), this);
  _log_dock->setObjectName("log_dock");
  _log_dock->setFeatures(QDockWidget::DockWidgetMovable);
  _log_dock->setAllowedAreas(Qt::RightDockWidgetArea);
  _log_dock->setVisible(false);
  _log_widget = new dock::LogDock(_log_dock);
  _log_dock->setWidget(_log_widget);

// MCP control dock
_mcp_control_widget = new dock::McpControlDock(AppControl::Instance(), this);

// Function dock (FFT / Math / Lissajous inline controls)
_function_dock = new QDockWidget(
    L_S(STR_PAGE_DLG, S_ID(IDS_TOOLBAR_FUNCTION), "Function"), this);
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

  // --- Create SlidingDrawer (overlay child of _central_widget, push via
  // margin) ---
  _sliding_drawer = new widgets::SlidingDrawer(_central_widget);
  _sliding_drawer->setDrawerWidth(350);
  _sliding_drawer->setAnimationDuration(300);
  _sliding_drawer->setPushLayout(_vertical_layout);

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

  // Device Options (includes sampling settings)
  _device_options_dock->setWidget(nullptr);
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

  setupSideBar();

  // When drawer closes, update toolbar state
  connect(_sliding_drawer, &widgets::SlidingDrawer::drawerClosed, this,
          [this]() {
            _drawer_current_page = -1;
            _side_bar->clearAllChecked();
            current_view()->show_search_cursor(false);
            ::DockOptions *opt = getDockOptions();
            if (opt) {
              opt->decodeDock = false;
              opt->triggerDock = false;
              opt->measureDock = false;
              opt->searchDock = false;
              opt->deviceOptionsDock = false;
              AppConfig::Instance().SaveFrame();
            }
            current_view()->setFocus();
          });

  connect(_sliding_drawer, &widgets::SlidingDrawer::drawerOpened, this,
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

  connect(_sliding_drawer, &widgets::SlidingDrawer::drawerDragFinished, this,
          [this]() {
            if (current_view()) {
              current_view()->limit_scale_offset();
            }
          });

  // event filter
  initial_view->installEventFilter(this);
  _sampling_bar->installEventFilter(this);
  _trig_bar->installEventFilter(this);
  _file_bar->installEventFilter(this);
  _logo_bar->installEventFilter(this);
  _dso_trigger_dock->installEventFilter(this);
  _trigger_dock->installEventFilter(this);
  _protocol_dock->installEventFilter(this);
  _measure_dock->installEventFilter(this);
  _search_dock->installEventFilter(this);
  _device_options_dock->installEventFilter(this);
  _sliding_drawer->installEventFilter(this);

  // defaut language
  AppConfig &app = AppConfig::Instance();
  switchLanguage(app.frameOptions.language);
  switchTheme(app.frameOptions.style);

  _sampling_bar->set_view(initial_view);

  // event
  connect(&_event, &EventObject::session_error, this,
          &MainWindow::on_session_error);
  connect(&_event, &EventObject::signals_changed, this,
          &MainWindow::on_signals_changed);
  connect(&_event, &EventObject::signals_changed, _search_widget,
          &dock::SearchDock::on_device_updated);
  connect(&_event, &EventObject::frame_ended, _search_widget,
          &dock::SearchDock::on_frame_ended);
  connect(&_event, &EventObject::receive_trigger, this,
          &MainWindow::on_receive_trigger);
  connect(&_event, &EventObject::frame_ended, this, &MainWindow::on_frame_ended,
          Qt::QueuedConnection);
  connect(&_event, &EventObject::frame_began, this, &MainWindow::on_frame_began,
          Qt::QueuedConnection);
  connect(&_event, &EventObject::decode_done, this,
          &MainWindow::on_decode_done);
  // C5 fix: on_data_updated is the no-arg Qt slot connected to
  // EventObject::data_updated. Use QOverload<>::of to select it.
  connect(&_event, &EventObject::data_updated, this,
          QOverload<>::of(&MainWindow::on_data_updated));
  connect(&_event, &EventObject::cur_snap_samplerate_changed, this,
          &MainWindow::on_cur_snap_samplerate_changed);
  connect(&_event, &EventObject::receive_data_len, this,
          &MainWindow::on_receive_data_len);
  // Task 1.3: ICaptureCallback signals are emitted from Core capture thread;
  // route through Qt::QueuedConnection so the on_* slots touch View on GUI
  // thread.
  connect(&_event, &EventObject::update_capture_sig, this,
          &MainWindow::on_update_capture, Qt::QueuedConnection);
  connect(&_event, &EventObject::show_region_sig, this,
          &MainWindow::on_show_region, Qt::QueuedConnection);
  connect(&_event, &EventObject::show_wait_trigger_sig, this,
          &MainWindow::on_show_wait_trigger, Qt::QueuedConnection);
  connect(&_event, &EventObject::repeat_hold_sig, this,
          &MainWindow::on_repeat_hold, Qt::QueuedConnection);

  // view
  connect(initial_view, &view::View::prgRate, this, &MainWindow::prgRate);
  connect(initial_view, &view::View::auto_trig, _dso_trigger_widget,
          &dock::DsoTriggerDock::auto_trig);

  // trig_bar
  connect(_trig_bar, &toolbars::TrigBar::sig_setTheme, this,
          &MainWindow::switchTheme);
  connect(_trig_bar, &toolbars::TrigBar::sig_show_lissajous, initial_view,
          &view::View::show_lissajous);

  // file toolbar
  connect(_file_bar, &toolbars::FileBar::sig_load_file, this,
          &MainWindow::on_load_file);
  connect(_file_bar, &toolbars::FileBar::sig_save, this, &MainWindow::on_save);
  connect(_file_bar, &toolbars::FileBar::sig_export, this,
          &MainWindow::on_export);
  connect(_file_bar, &toolbars::FileBar::sig_import_file, this,
          &MainWindow::on_import_file);
  connect(_file_bar, &toolbars::FileBar::sig_screenShot, this,
          &MainWindow::on_screenShot, Qt::QueuedConnection);
  connect(_file_bar, &toolbars::FileBar::sig_load_session, this,
          &MainWindow::on_load_session);
  connect(_file_bar, &toolbars::FileBar::sig_store_session, this,
          &MainWindow::on_store_session);

  // logobar
  connect(_logo_bar, &toolbars::LogoBar::sig_open_doc, this,
          &MainWindow::on_open_doc);

  connect(_protocol_widget, &dock::ProtocolDock::protocol_updated, this,
          &MainWindow::on_signals_changed);

  // SamplingBar
  connect(_sampling_bar, &toolbars::SamplingBar::sig_store_session_data, this,
          &MainWindow::on_save);

  //
  connect(_dso_trigger_widget, &dock::DsoTriggerDock::set_trig_pos,
          initial_view, &view::View::set_trig_pos);

  _delay_prop_msg_timer.SetCallback(
      std::bind(&MainWindow::on_delay_prop_msg, this));

  _logo_bar->set_mainform_callback(this);

  // Bind initial context to docks
  _sampling_bar->bind_context(initial_ctx);
  _measure_widget->bind_context(initial_ctx);
  _search_widget->bind_context(initial_ctx);
  _protocol_widget->bind_context(initial_ctx);
  _device_options_widget->bind_context(initial_ctx);
  _log_widget->bind_context(initial_ctx);
  _trigger_widget->bind_context(initial_ctx);
  _dso_trigger_widget->bind_context(initial_ctx);

  connect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, this,
          &MainWindow::on_tab_changed);
  connect(_tab_widget, &pv::ui::DraggableTabWidget::tabMoved, this,
          &MainWindow::on_tab_moved);
  connect(_tab_widget, &pv::ui::DraggableTabWidget::tabDetached, this,
          &MainWindow::on_tab_detach);
  connect(_tab_widget, &pv::ui::DraggableTabWidget::tabAttached, this,
          &MainWindow::on_tab_attached);
  connect(_tab_widget, &pv::ui::DraggableTabWidget::newTabRequested, this,
          &MainWindow::on_new_tab_requested);
  connect(_tab_widget, &pv::ui::DraggableTabWidget::tabCloseRequested, this,
          &MainWindow::remove_tab);
  connect(_tab_widget, &pv::ui::DraggableTabWidget::tabRenamed, this,
          [this](int index, const QString &title) {
            if (index >= 0 && index < _tab_contexts.size()) {
              _tab_contexts[index]->set_title(title);
              update_tab_style(index);
            }
          });
  connect(_tab_widget, &pv::ui::DraggableTabWidget::tabAttached, this,
          [this](QWidget *widget, const QString &title) {
            pv::view::View *view = qobject_cast<pv::view::View *>(widget);
            if (view) {
              pv::TabContext *existing_ctx = nullptr;
              for (auto c : _tab_contexts) {
                if (c->view() == view) {
                  existing_ctx = c;
                  break;
                }
              }
              if (!existing_ctx) {
                QVariant var = view->property("detached_ctx");
                if (var.isValid()) {
                  existing_ctx = (pv::TabContext *)(var.value<quintptr>());
                  if (existing_ctx) {
                    existing_ctx->set_title(title);
                    _tab_contexts.append(existing_ctx);
                    view->setProperty("detached_ctx", QVariant());
                  }
                }
                if (!existing_ctx) {
                  // phase 2: document owned by DocumentRegistry.
                  size_t doc_idx = _session->document_registry()->take_document(
                      std::make_unique<pv::data::SessionDocument>(_session));
                  pv::data::SessionDocument *doc =
                      _session->document_registry()->get_document_by_index(doc_idx);
                  pv::TabContext *ctx =
                      SessionManager::instance()->create_context(view, _session,
                                                                 doc, doc_idx,
                                                                 _session->document_registry());
                  ctx->set_title(title);
                  _tab_contexts.append(ctx);
                }
              }
            }
          });

  // Try load from file.
  QString ldFileName(::AppControl::Instance()->_open_file_name.c_str());
  if (ldFileName != "") {
    std::string file_name = pv::path::ToUnicodePath(ldFileName);

    if (QFile::exists(ldFileName)) {
      pxv_info("Auto load file:%s", file_name.c_str());
      tmp_file = ldFileName;
    } else {
      pxv_err("file is not exists:%s", file_name.c_str());
      MsgBox::Show(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_OPEN_FILE_ERROR), "Open file error!"),
          ldFileName, nullptr);
    }
  }

  on_load_device_first();

  _disk_cache_status_label = new QLabel(this);
  statusBar()->addWidget(_disk_cache_status_label);
  _disk_cache_status_label->hide();

  _sample_period_label = new QLabel(this);
  _sample_period_label->setText("采样周期: --");
  statusBar()->addPermanentWidget(_sample_period_label);
  _sample_period_label->show();

  _trig_time_label = new QLabel(this);
  statusBar()->addPermanentWidget(_trig_time_label);
  _trig_time_label->hide();

  _fps_label = new QLabel(this);
  _fps_label->setText("UI: --ms | Dock: --ms");
  statusBar()->addPermanentWidget(_fps_label);
  _fps_label->show();

  _acq_count = 0;
  connect(&_fps_timer, &QTimer::timeout, this, &MainWindow::update_fps);
  _fps_timer.start(1000);

  connect(&_disk_cache_status_timer, &QTimer::timeout, this,
          &MainWindow::update_disk_cache_status);
  _disk_cache_status_timer.start(500);

  if (!_tab_contexts.isEmpty()) {
    _tab_contexts[0]->activate();
  }
}

void MainWindow::on_load_device_first() {
  if (tmp_file != "") {
    on_load_file(tmp_file);
    tmp_file = "";
  } else {
    _session->set_default_device();
  }
}

void MainWindow::retranslateUi() {
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

  Ribbon_retranslateUi();
}

void MainWindow::on_load_file(QString file_name) {
  pv::view::View *new_view = new pv::view::View(_session, _sampling_bar, this);
  // phase 2: document owned by DocumentRegistry.
  size_t new_doc_idx = _session->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_session));
  pv::data::SessionDocument *new_doc =
      _session->document_registry()->get_document_by_index(new_doc_idx);
  pv::TabContext *ctx =
      SessionManager::instance()->create_context(new_view, _session, new_doc,
                                                 new_doc_idx,
                                                 _session->document_registry());

  QFileInfo fi(file_name);
  ctx->set_title(fi.baseName());
  ctx->set_file_path(file_name);

  add_tab(ctx);

  try {
    if (_device_agent->is_hardware()) {
      save_config();
    }

    // 架构修复：检查 set_file 返回值，失败时不创建空白 tab
    if (!_session->set_file(file_name)) {
      QString strMsg(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
      strMsg += file_name;
      MsgBox::Show(strMsg);
      // 回滚已创建的 tab
      int idx = _tab_contexts.indexOf(ctx);
      if (idx >= 0)
        remove_tab(idx);
      _session->set_default_device();
      return;
    }
    ctx->make_live();
    ctx->activate();
    update_tab_style(_tab_contexts.indexOf(ctx));
  } catch (QString e) {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
    strMsg += file_name;
    MsgBox::Show(strMsg);
    _session->set_default_device();
  }
}

void MainWindow::on_import_file(QString file_name) {
  pv::view::View *new_view = new pv::view::View(_session, _sampling_bar, this);
  // phase 2: document owned by DocumentRegistry.
  size_t new_doc_idx = _session->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_session));
  pv::data::SessionDocument *new_doc =
      _session->document_registry()->get_document_by_index(new_doc_idx);
  pv::TabContext *ctx =
      SessionManager::instance()->create_context(new_view, _session, new_doc,
                                                 new_doc_idx,
                                                 _session->document_registry());

  QFileInfo fi(file_name);
  ctx->set_title(fi.baseName());
  ctx->set_file_path(file_name);

  add_tab(ctx);

  try {
    // Import external data file using libsigrok input modules
    // (VCD, CSV, binary, Saleae, etc.) — aligned with PulseView.
    if (!_session->import_file(file_name)) {
      QString strMsg(
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
      strMsg += file_name;
      MsgBox::Show(strMsg);
      // 回滚已创建的 tab
      int idx = _tab_contexts.indexOf(ctx);
      if (idx >= 0)
        remove_tab(idx);
      _session->set_default_device();
      return;
    }
    ctx->make_live();
    ctx->activate();
    update_tab_style(_tab_contexts.indexOf(ctx));
  } catch (QString e) {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FAIL_TO_LOAD), "Failed to load "));
    strMsg += file_name;
    MsgBox::Show(strMsg);
    _session->set_default_device();
  }
}

void MainWindow::session_error() { _event.session_error(); }

void MainWindow::session_save() { save_config(); }

void MainWindow::on_session_error() {
  QString title;
  QString details;
  QString ch_status = "";

  switch (_session->get_error()) {
  case SigSession::Hw_err:
    pxv_info("MainWindow::on_session_error(),Hw_err, stop capture");
    _session->stop_capture();
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_HARDWARE_ERROR),
                "Hardware Operation Failed");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_HARDWARE_ERROR_DET),
                  "Please replug device to refresh hardware configuration!");
    break;
  case SigSession::Malloc_err:
    pxv_info("MainWindow::on_session_error(),Malloc_err, stop capture");
    _session->stop_capture();
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MALLOC_ERROR), "Malloc Error");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MALLOC_ERROR_DET),
                  "Memory is not enough for this sample!\nPlease reduce the "
                  "sample depth!");
    break;
  case SigSession::Pkt_data_err:
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PACKET_ERROR), "Packet Error");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PACKET_ERROR_DET),
                  "the content of received packet are not expected!");
    _session->refresh(0);
    break;
  case SigSession::Data_overflow:
    pxv_info("MainWindow::on_session_error(),Data_overflow, stop capture");
    _session->stop_capture();
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DATA_OVERFLOW), "Data Overflow");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DATA_OVERFLOW_DET),
                  "USB bandwidth can not support current sample rate! \nPlease "
                  "reduce the sample rate!");
    break;
  default:
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_UNDEFINED_ERROR), "Undefined Error");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_UNDEFINED_ERROR_DET),
                  "Not expected error!");
    break;
  }

  pv::dialogs::DSMessageBox msg(this, title);
  msg.mBox()->setText(details);
  msg.mBox()->setStandardButtons(QMessageBox::Ok);
  msg.mBox()->setIcon(QMessageBox::Warning);
  connect(_session->device_event_object(), &DeviceEventObject::device_updated,
          &msg, &QDialog::accept);
  _msg = &msg;
  msg.exec();
  _msg = nullptr;

  _session->clear_error();
}

void MainWindow::save_config() {
  pxv_info("save_config: ENTER, have_instance=%d, is_hardware=%d", _device_agent->have_instance(), _device_agent->is_hardware());
  if (_device_agent->have_instance() == false) {
    pxv_info("There is no need to save the configuration");
    return;
  }

  AppConfig &app = AppConfig::Instance();

  // Always persist the last-used device driver name so the next launch
  // can prefer this device. Without this, switching to demo and exiting
  // would leave lastDeviceDriver stale (still pointing to the hardware
  // device), causing the app to jump back to hardware on restart.
  app.deviceOptions.lastDeviceDriver = _device_agent->driver_name();
  app.SaveDevice();

  if (_device_agent->is_hardware() && !_device_agent->is_demo()) {
    // Persist connection ID for hardware devices to distinguish multiple
    // devices of the same model.
    struct sr_dev_inst *sdi = _device_agent->inst();
    if (sdi) {
      const char *cid = sr_dev_inst_connid_get(sdi);
      if (cid)
        app.deviceOptions.lastDeviceConnId = QString::fromLocal8Bit(cid);
    }

    QString sessionFile = gen_config_file_path(true);
    save_config_to_file(sessionFile);
  } else if (_device_agent->is_demo()) {
    // Demo device: save channel/trigger/decoder config to its own .pxc file
    // so demo setups (channel enable, trigger, decoders) persist across restarts.
    QDir dir(GetFirmwareDir());
    if (dir.exists()) {
      QString ses_name = dir.absolutePath() + "/" +
                         _device_agent->driver_name() +
                         QString::number(_device_agent->get_work_mode()) +
                         ".pxc";
      save_config_to_file(ses_name);
    }
  }

  app.frameOptions.windowState = saveState();
  app.SaveFrame();
}

QString MainWindow::gen_config_file_path(bool isNewFormat) {
  AppConfig &app = AppConfig::Instance();

  QString file = GetProfileDir();
  QDir dir(file);
  if (dir.exists() == false) {
    dir.mkpath(file);
  }

  QString driver_name = _device_agent->driver_name();
  QString mode_name = QString::number(_device_agent->get_work_mode());
  QString lang_name;
  QString base_path = dir.absolutePath() + "/" + driver_name + mode_name;

  if (!isNewFormat) {
    lang_name = QString::number(app.frameOptions.language);
  }

  return base_path + ".ses" + lang_name + ".pxc";
}

bool MainWindow::able_to_close() {
  // Only commit UI settings to device when the device has no prior capture
  // data. If the device has data, the settings were already committed during
  // capture setup. Calling commit_settings() unconditionally would overwrite
  // device values (e.g., sample limit loaded from .pxc) with UI dropdown
  // values, which may not have the exact same option (e.g., 200M vs 1G).
  if (_device_agent->is_hardware() && _session->have_hardware_data() == false) {
    _sampling_bar->commit_settings();
  }

  _tab_widget->closeAllDetachedWindows();

  save_config();

  // Check if the user has disabled the save prompt on exit
  if (!AppConfig::Instance().appOptions.promptSaveOnExit) {
    return true;
  }

  if (confirm_to_store_data()) {
    on_save();
    return false;
  }
  return true;
}

void MainWindow::on_side_bar_dock_clicked(int index) {
  bool isChecked = _side_bar->getItem(index)->button->isChecked();

  if (!isChecked) {
    if (_sliding_drawer->isOpen())
      _sliding_drawer->close();
    current_view()->show_search_cursor(false);
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
    current_view()->setFocus();
    return;
  }

  int drawerPage = -1;

  switch (index) {
  case SIDEBAR_TRIGGER:
    if (_device_agent->get_work_mode() != DSO) {
      _trigger_widget->update_view();
      drawerPage = _drawer_page_trigger;
    } else {
      _dso_trigger_widget->update_view();
      drawerPage = _drawer_page_dso_trigger;
    }
    break;
  case SIDEBAR_DECODE:
    drawerPage = _drawer_page_protocol;
    break;
  case SIDEBAR_MEASURE:
    drawerPage = _drawer_page_measure;
    break;
  case SIDEBAR_SEARCH:
    current_view()->show_search_cursor(true);
    drawerPage = _drawer_page_search;
    break;
  case SIDEBAR_FUNCTION:
    _function_widget->reload();
    drawerPage = _drawer_page_function;
    break;
  case SIDEBAR_OPTIONS:
    _device_options_widget->update_view();
    drawerPage = _drawer_page_device_options;
    break;
  case SIDEBAR_MCP:
    _mcp_control_widget->refresh_status();
    drawerPage = _drawer_page_mcp;
    break;
  case SIDEBAR_LOG:
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
    opt->decodeDock = (index == SIDEBAR_DECODE);
    opt->triggerDock = (index == SIDEBAR_TRIGGER);
    opt->measureDock = (index == SIDEBAR_MEASURE);
    opt->searchDock = (index == SIDEBAR_SEARCH);
    opt->deviceOptionsDock = (index == SIDEBAR_OPTIONS);
    opt->logDock = (index == SIDEBAR_LOG);
    AppConfig::Instance().SaveFrame();
  }

  current_view()->setFocus();
}

void MainWindow::on_side_bar_action_clicked(int index) {
  switch (index) {
  case SIDEBAR_RUNSTOP:
    if (_session->is_working()) {
      _session->stop_capture();
    } else {
      _sampling_bar->run_or_stop();
    }
    break;
  case SIDEBAR_INSTANT:
    if (_session->is_working() && _session->is_instant()) {
      _session->stop_capture();
    } else {
      _sampling_bar->run_or_stop_instant();
    }
    break;
  }
}

void MainWindow::on_screenShot() {
  AppConfig &app = AppConfig::Instance();
  QString default_name =
      app.userHistory.screenShotPath + "/" + APP_NAME +
      QDateTime::currentDateTime().toString("-yyMMdd-hhmmss");

  int x = parentWidget()->pos().x();
  int y = parentWidget()->pos().y();
  int w = parentWidget()->frameGeometry().width();
  int h = parentWidget()->frameGeometry().height();

  (void)h;
  (void)w;
  (void)x;
  (void)y;

#ifdef _WIN32
  QPixmap pixmap = parentWidget()->grab();
#elif __APPLE__
  x += MainFrame::Margin;
  y += MainFrame::Margin;
  w -= MainFrame::Margin * 2;
  h -= MainFrame::Margin * 2;

  QPixmap pixmap =
      QGuiApplication::primaryScreen()->grabWindow(winId(), x, y, w, h);
#else
  QPixmap pixmap = parentWidget()->grab();
#endif

  QString format = "png";
  QString fileName = QFileDialog::getSaveFileName(
      this, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAVE_AS), "Save As"), default_name,
      "png file(*.png);;jpeg file(*.jpeg)", &format);

  if (!fileName.isEmpty()) {
    QStringList list = format.split('.').last().split(')');
    QString suffix = list.first();

    QFileInfo f(fileName);
    if (f.suffix().compare(suffix)) {
      // tr
      fileName += "." + suffix;
    }

    pixmap.save(fileName, suffix.toLatin1());

    fileName = path::GetDirectoryName(fileName);

    if (app.userHistory.screenShotPath != fileName) {
      app.userHistory.screenShotPath = fileName;
      app.SaveHistory();
    }
  }
}

// save file
void MainWindow::on_save() {
  using pv::dialogs::StoreProgress;

  if (_device_agent->have_instance() == false) {
    pxv_info("Have no device, can't to save data.");
    return;
  }

  if (_session->is_working()) {
    pxv_info("Save data: stop the current device.");
    _session->stop_capture();
  }

  _session->set_saving(true);

  StoreProgress *dlg = new StoreProgress(_session, this);
  dlg->SetView(current_view());
  dlg->save_run(this);
}

void MainWindow::on_export() {
  using pv::dialogs::StoreProgress;

  if (_session->is_working()) {
    pxv_info("Export data: stop the current device.");
    _session->stop_capture();
  }

  StoreProgress *dlg = new StoreProgress(_session, this);
  dlg->SetView(current_view());
  dlg->export_run();
}

bool MainWindow::on_load_session(QString name) {
  return load_config_from_file(name);
}

bool MainWindow::load_config_from_file(QString file) {
  if (file == "") {
    pxv_err("File name is empty.");
    assert(false);
  }

  _protocol_widget->del_all_protocol();

  std::string file_name = pv::path::ToUnicodePath(file);
  pxv_info("Load device profile: \"%s\"", file_name.c_str());

  QFile sf(file);

  if (!sf.exists()) {
    pxv_warn("Warning: device profile is not exists: \"%s\"",
             file_name.c_str());
    return false;
  }

  if (!sf.open(QIODevice::ReadOnly)) {
    pxv_warn("Warning: Couldn't open device profile to load!");
    return false;
  }

  QString data = QString::fromUtf8(sf.readAll());
  QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
  sf.close();

  bool bDecoder = false;
  int ret = load_config_from_json(doc, bDecoder);

  if (ret && _device_agent->get_work_mode() == DSO) {
    _dso_trigger_widget->update_view();
  }

  if (_device_agent->is_hardware()) {
    _title_ext_string = file;
    update_title_bar_text();
  }

  return ret;
}

bool MainWindow::gen_config_json(QJsonObject &sessionVar) {
  AppConfig &app = AppConfig::Instance();

  QString title = QApplication::applicationName() + " v" +
                  QApplication::applicationVersion();

  sessionVar["Version"] = QJsonValue::fromVariant(SESSION_FORMAT_VERSION);
  sessionVar["Device"] = QJsonValue::fromVariant(_device_agent->driver_name());
  sessionVar["DeviceMode"] =
      QJsonValue::fromVariant(_device_agent->get_work_mode());
  sessionVar["Language"] = QJsonValue::fromVariant(app.frameOptions.language);
  sessionVar["Title"] = QJsonValue::fromVariant(title);

  if (_device_agent->is_hardware() && _device_agent->get_work_mode() == LOGIC) {
    sessionVar["CollectMode"] = _session->get_collect_mode();
  }

  // --- Device instance session config (sample rate, limit_samples, operation_mode, etc.) ---
  GVariant *gvar_opts =
      _device_agent->get_config_list(nullptr, SR_CONF_DEVICE_SESSIONS);
  GVariant *gvar;
  gsize num_opts;

  pxv_info("gen_config_json: querying SR_CONF_DEVICE_SESSIONS, gvar_opts=%p", gvar_opts);

  if (gvar_opts != nullptr) {
    /* Driver implements SR_CONF_DEVICE_SESSIONS with an int32[] array
     * (e.g., fork's std::opts_config_list). The array contains bare keys like
     * SR_CONF_SAMPLERATE, NOT packed with flags. */
    const int *const options = (const int32_t *)g_variant_get_fixed_array(
        gvar_opts, &num_opts, sizeof(int32_t));

    for (unsigned int i = 0; i < num_opts; i++) {
      const struct sr_config_info *const info =
          _device_agent->get_config_info(options[i]);
      if (!info || !info->name)
        continue;
      gvar = _device_agent->get_config(info->key);
      if (gvar != nullptr) {
        if (info->datatype == SR_T_BOOL)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_boolean(gvar));
        else if (info->datatype == SR_T_UINT64)
          sessionVar[info->name] = QJsonValue::fromVariant(
              QString::number(g_variant_get_uint64(gvar)));
        else if (info->datatype == SR_T_UINT8)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_byte(gvar));
        else if (info->datatype == SR_T_INT16)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_int16(gvar));
        else if (info->datatype == SR_T_FLOAT)
          sessionVar[info->name] = QJsonValue::fromVariant(
              QString::number(g_variant_get_double(gvar)));
        else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_string(gvar, nullptr));
        else if (info->datatype == SR_T_INT32)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_int32(gvar));
        else if (info->datatype == SR_T_UINT32)
          sessionVar[info->name] =
              QJsonValue::fromVariant((uint32_t)g_variant_get_uint32(gvar));
        else if (info->datatype == SR_T_LIST)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_int16(gvar));
        else {
          pxv_err("Unkown config info type:%d", info->datatype);
          assert(false);
        }
        g_variant_unref(gvar);
      }
    }
    g_variant_unref(gvar_opts);
  } else if (_device_agent->is_hardware()) {
    /* Driver does not implement SR_CONF_DEVICE_SESSIONS. Use SR_CONF_DEVICE_OPTIONS (uint32_t
     * packed entries with capability flags like SR_CONF_SAMPLERATE | SR_CONF_GET).
     * DeviceAgent::get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS) returns a uint32_t array
     * (see pxlogic.h devopts[] declaration), each element is key|flags.
     * We mask with 0x1fffffff to extract the bare key for sr_key_info_get(),
     * and iterate the list to save ALL device options, not just a hardcoded subset. */
    pxv_info("gen_config_json: falling back to SR_CONF_DEVICE_OPTIONS");
    gvar_opts = _device_agent->get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);

    if (!gvar_opts) {
      pxv_warn("No SR_CONF_DEVICE_OPTIONS available, skipping per-device config section.");
    } else {
      const uint32_t *const options = (const uint32_t *)g_variant_get_fixed_array(
          gvar_opts, &num_opts, sizeof(uint32_t));

      for (unsigned int i = 0; i < num_opts; i++) {
        /* Mask off capability bits (SR_CONF_GET/SET/LIST, top 3 bits)
         * to get the bare config key. sr_key_info_get only recognizes bare keys.
         * SR_CONF_MASK = 0x1fffffff (libsigrok-internal.h, not public). */
        const int key = (int)(options[i] & 0x1fffffff);

        const struct sr_config_info *const info =
            _device_agent->get_config_info(key);
        if (!info || !info->name)
          continue;

        gvar = _device_agent->get_config(info->key);
        if (gvar != nullptr) {
          if (info->datatype == SR_T_BOOL)
            sessionVar[info->name] =
                QJsonValue::fromVariant(g_variant_get_boolean(gvar));
          else if (info->datatype == SR_T_UINT64)
            sessionVar[info->name] = QJsonValue::fromVariant(
                QString::number(g_variant_get_uint64(gvar)));
          else if (info->datatype == SR_T_UINT8)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_byte(gvar));
          else if (info->datatype == SR_T_INT16)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int16(gvar));
          else if (info->datatype == SR_T_FLOAT)
            sessionVar[info->name] = QJsonValue::fromVariant(
                QString::number(g_variant_get_double(gvar)));
          else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING)
            sessionVar[info->name] =
                QJsonValue::fromVariant(g_variant_get_string(gvar, nullptr));
          else if (info->datatype == SR_T_INT32)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int32(gvar));
          else if (info->datatype == SR_T_UINT32)
            sessionVar[info->name] = QJsonValue::fromVariant((uint32_t)g_variant_get_uint32(gvar));
          else if (info->datatype == SR_T_LIST)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int16(gvar));
          else {
            pxv_err("Unkown config info type:%d", info->datatype);
            assert(false);
          }
          g_variant_unref(gvar);
        }
      }
      g_variant_unref(gvar_opts);
    }
  }

  // Task 3 (purify-architecture-concepts): channel 段改为通过 SignalConfigStore
  // 序列化（单一序列化路径），不再直访 view::Signal。先调用 save_signal_config
  // 从当前 device + View 状态填充 _signal_config，再 signal_config_to_json 产出
  // channels[] 数组。顶层 key 仍为 "channel"（单数）以保持 .pxc 外层结构不变；
  // 数组内字段统一使用 ChannelConfig 字段名（不保留 strigger/trigValue/zeroPos/
  // mapUnit/mapMin/mapMax/mapDefault/colour/type/name/vfactor 等 MainWindow 旧 key）。
  pv::TabContext *ctx = current_context();
  pv::data::SessionDocument *doc = ctx ? ctx->document() : nullptr;
  if (doc) {
    doc->save_signal_config(_session->get_signal_models(),
                            build_channel_layout(current_view()),
                            build_channel_colours(current_view()));
    QJsonObject sig_cfg = doc->signal_config_to_json();
    sessionVar["channel"] = sig_cfg["channels"].toArray();
  } else {
    pxv_warn("MainWindow::gen_config_json: no active document, writing empty "
             "channel array");
    sessionVar["channel"] = QJsonArray();
  }

  if (_device_agent->get_work_mode() == LOGIC) {
    // Task 6 (purify-architecture-concepts): trigger 序列化改走 Core
    // TriggerConfig（唯一真相源），不再调用 _trigger_widget->get_session()
    // 经 View 层产出旧 JSON key。to_json() 写入 mode/trigger_pos/stage_count/
    // stages[]/adv_enabled/adv_tab_index/serial_* 新结构。
    sessionVar["trigger"] = _session->trigger_config().to_json();
  }

  // 毛刺滤波配置持久化：保存阈值/模式/auto_apply 到 .pxl/.pxc 文件，
  // 重新打开时恢复，避免面板关闭后配置丢失导致光标位置改变。
  if (_session->is_glitch_filter_active() ||
      _session->glitch_filter_auto_apply() ||
      !_session->glitch_filter_thresholds().empty()) {
    QJsonObject glitchObj;
    glitchObj["auto_apply"] = _session->glitch_filter_auto_apply();
    glitchObj["show_overlay"] = _session->show_glitch_filter_overlay();
    glitchObj["active"] = _session->is_glitch_filter_active();
    QJsonArray thrArray;
    QJsonArray modeArray;
    const auto &thresholds = _session->glitch_filter_thresholds();
    const auto &modes = _session->glitch_filter_modes();
    for (const auto &kv : thresholds) {
      QJsonObject entry;
      entry["ch"] = kv.first;
      entry["threshold"] = (int)kv.second;
      thrArray.append(entry);
    }
    for (const auto &kv : modes) {
      QJsonObject entry;
      entry["ch"] = kv.first;
      entry["mode"] = (int)kv.second;
      modeArray.append(entry);
    }
    glitchObj["thresholds"] = thrArray;
    glitchObj["modes"] = modeArray;
    sessionVar["glitch_filter"] = glitchObj;
  }

  StoreSession ss(_session);
  QJsonArray decodeJson;
  ss.gen_decoders_json(decodeJson);
  sessionVar["decoder"] = decodeJson;

  if (_device_agent->get_work_mode() == DSO) {
    sessionVar["measure"] = current_view()->get_viewstatus()->get_session();
  }

  return true;
}

bool MainWindow::load_config_from_json(QJsonDocument &doc, bool &haveDecoder) {
  haveDecoder = false;

  // DeviceConfigChanged broadcasts are now ASYNC (queued on qApp via
  // Qt::QueuedConnection by EventBus), so the previous
  // SuppressConfigBroadcastGuard (which prevented nested reload ->
  // signals_changed -> View AllReplaced deleting the DsoSignal mid-method) is
  // no longer needed: the caller's stack frame completes before any listener
  // processes the message. Device config is still written; reload() at the
  // end rebuilds from it.

  QJsonObject sessionObj = doc.object();

  int mode = _device_agent->get_work_mode();

  // check config file version
  if (!sessionObj.contains("Version")) {
    pxv_dbg("Profile version is not exists!");
    return false;
  }

  int format_ver = sessionObj["Version"].toInt();

  if (format_ver < 2) {
    pxv_err("Profile version is error!");
    return false;
  }

  if (sessionObj.contains("CollectMode") && _device_agent->is_hardware()) {
    int collect_mode = sessionObj["CollectMode"].toInt();
    _session->set_collect_mode((DEVICE_COLLECT_MODE)collect_mode);
  }

  int conf_dev_mode = sessionObj["DeviceMode"].toInt();

  if (_device_agent->is_hardware()) {
    QString driverName = _device_agent->driver_name();
    QString sessionDevice = sessionObj["Device"].toString();
    // check device and mode
    if (driverName != sessionDevice || mode != conf_dev_mode) {
      MsgBox::Show(
          nullptr,
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PROFILE_NOT_COMPATIBLE),
              "Profile is not compatible with current device or mode!"),
          this);
      return false;
    }
  }

  // load device settings
  GVariant *gvar_opts =
      _device_agent->get_config_list(nullptr, SR_CONF_DEVICE_SESSIONS);
  gsize num_opts;

  if (gvar_opts != nullptr) {
    /* Driver implements SR_CONF_DEVICE_SESSIONS with an int32[] array
     * (e.g., fork's std::opts_config_list). The array contains bare keys like
     * SR_CONF_SAMPLERATE, NOT packed with flags. */
    const int *const options = (const int32_t *)g_variant_get_fixed_array(
        gvar_opts, &num_opts, sizeof(int32_t));

    for (unsigned int i = 0; i < num_opts; i++) {
      /* SR_CONF_DEVICE_SESSIONS returns bare keys (no capability flags).
       * We cannot check SR_CONF_SET here, so we attempt set_config and
       * silently skip GET-only keys that reject the SET. */
      const int key = options[i];
      const struct sr_config_info *info =
          _device_agent->get_config_info(key);

      if (!info || !info->name)
        continue;

      if (!sessionObj.contains(info->name))
        continue;

      GVariant *gvar = nullptr;
      int id = 0;

      if (info->datatype == SR_T_BOOL) {
        gvar = g_variant_new_boolean(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_UINT64) {
        // from string text.
        gvar = g_variant_new_uint64(
            sessionObj[info->name].toString().toULongLong());
      } else if (info->datatype == SR_T_UINT8) {
        if (sessionObj[info->name].toString() != "")
          gvar = g_variant_new_byte(sessionObj[info->name].toString().toUInt());
        else
          gvar = g_variant_new_byte(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_INT16) {
        gvar = g_variant_new_int16(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_FLOAT) {
        if (sessionObj[info->name].toString() != "")
          gvar = g_variant_new_double(
              sessionObj[info->name].toString().toDouble());
        else
          gvar = g_variant_new_double(sessionObj[info->name].toDouble());
      } else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING) {
        gvar = g_variant_new_string(
            sessionObj[info->name].toString().toLocal8Bit().data());
      } else if (info->datatype == SR_T_INT32) {
        gvar = g_variant_new_int32(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_UINT32) {
        gvar = g_variant_new_uint32(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_LIST) {
        id = 0;

        if (format_ver > 2) {
          // Is new version format.
          id = sessionObj[info->name].toInt();
        } else {
          const char *fd_key =
              sessionObj[info->name].toString().toLocal8Bit().data();
          id = _device_agent->option_value_to_code(conf_dev_mode, info->key, fd_key);
          if (id == -1) {
            pxv_err("Convert failed, key:\"%s\", value:\"%s\"", info->name,
                    fd_key);
            id = 0; // set default value.
          } else {
            pxv_info("Convert success, key:\"%s\", value:\"%s\", get code:%d",
                     info->name, fd_key, id);
          }
        }
        gvar = g_variant_new_int16(id);
      }

      if (gvar == nullptr) {
        pxv_warn("Warning: Profile failed to parse key:'%s'", info->name);
        continue;
      }

      bool bFlag = _device_agent->set_config(info->key, gvar);
      if (!bFlag) {
        /* GET-only keys (SR_CONF_REF_MAX, SR_CONF_MAX_DSO_SAMPLERATE,
         * SR_CONF_LOAD_DECODER, SR_CONF_HAVE_ZERO, SR_CONF_MAX_DSO_SAMPLELIMITS)
         * are listed in SR_CONF_DEVICE_SESSIONS but don't support SET.
         * Silently skip — the driver keeps its default value. */
        pxv_dbg("load_config: key '%s' (id=%d) rejected SET, skipping",
                info->name, info->key);
      }
    }
    g_variant_unref(gvar_opts);
  } else if (_device_agent->is_hardware()) {
    /* Driver does not implement SR_CONF_DEVICE_SESSIONS. Fall back to SR_CONF_DEVICE_OPTIONS
     * (uint32_t packed entries with capability flags). Mirrors the save-side fallback
     * in gen_config_json(). */
    gvar_opts = _device_agent->get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);

    if (!gvar_opts) {
      pxv_warn("No SR_CONF_DEVICE_OPTIONS available, skipping per-device config load.");
    } else {
      const uint32_t *const options = (const uint32_t *)g_variant_get_fixed_array(
          gvar_opts, &num_opts, sizeof(uint32_t));

      for (unsigned int i = 0; i < num_opts; i++) {
        /* Mask off capability bits — see gen_config_json() for details. */
        const int key = (int)(options[i] & 0x1fffffff);
        /* Skip GET-only keys — SR_CONF_SET bit is bit 30. */
        if (!(options[i] & SR_CONF_SET))
          continue;
        const struct sr_config_info *info =
            _device_agent->get_config_info(key);

        if (!info || !info->name)
          continue;

        if (!sessionObj.contains(info->name))
          continue;

        GVariant *gvar = nullptr;
        int id = 0;

        if (info->datatype == SR_T_BOOL) {
          gvar = g_variant_new_boolean(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_UINT64) {
          // from string text.
          gvar = g_variant_new_uint64(
              sessionObj[info->name].toString().toULongLong());
        } else if (info->datatype == SR_T_UINT8) {
          if (sessionObj[info->name].toString() != "")
            gvar = g_variant_new_byte(sessionObj[info->name].toString().toUInt());
          else
            gvar = g_variant_new_byte(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_INT16) {
          gvar = g_variant_new_int16(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_FLOAT) {
          if (sessionObj[info->name].toString() != "")
            gvar = g_variant_new_double(
                sessionObj[info->name].toString().toDouble());
          else
            gvar = g_variant_new_double(sessionObj[info->name].toDouble());
        } else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING) {
          gvar = g_variant_new_string(
              sessionObj[info->name].toString().toLocal8Bit().data());
        } else if (info->datatype == SR_T_INT32) {
          gvar = g_variant_new_int32(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_UINT32) {
          gvar = g_variant_new_uint32(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_LIST) {
          id = 0;

          if (format_ver > 2) {
            // Is new version format.
            id = sessionObj[info->name].toInt();
          } else {
            const char *fd_key =
                sessionObj[info->name].toString().toLocal8Bit().data();
            id = _device_agent->option_value_to_code(conf_dev_mode, info->key, fd_key);
            if (id == -1) {
              pxv_err("Convert failed, key:\"%s\", value:\"%s\"", info->name,
                      fd_key);
              id = 0; // set default value.
            } else {
              pxv_info("Convert success, key:\"%s\", value:\"%s\", get code:%d",
                       info->name, fd_key, id);
            }
          }
          gvar = g_variant_new_int16(id);
        }

        if (gvar == nullptr) {
          pxv_warn("Warning: Profile failed to parse key:'%s'", info->name);
          continue;
        }

        bool bFlag = _device_agent->set_config(info->key, gvar);
        if (!bFlag) {
          pxv_err("Set device config option failed, id:%d, code:%d", info->key,
                  id);
        }
      }
      g_variant_unref(gvar_opts);
    }
  }

  // load channel settings
  // Task 3 (purify-architecture-concepts): channel 段改走 SignalConfigStore 单一
  // 路径。原代码按 DSO/非 DSO 两分支直改 sr_channel->vdiv/coupling/vfactor/
  // trig_value/map_*/enabled/name，现统一为：signal_config_from_json 解析
  // channels[] 数组到 _signal_config，apply_signal_config 应用到 sr_channel。
  // 顶层 key 仍是 "channel"（单数），此处包成 {"channels": [...]} 喂给 store。
  // work_mode/operation_mode/channel_mode/is_demo 取当前 device 已应用的值，
  // 避免 apply_signal_config 误改 device mode（device settings 循环已设置）。
  if (sessionObj.contains("channel")) {
    pv::TabContext *ctx = current_context();
    pv::data::SessionDocument *doc = ctx ? ctx->document() : nullptr;
    if (doc) {
      QJsonObject sig_obj;
      sig_obj["channels"] = sessionObj["channel"].toArray();
      doc->signal_config_from_json(sig_obj);
      // 用当前 device 已应用的 mode/op_mode/ch_mode/is_demo 覆盖，保证
      // apply_signal_config 不会改变 device mode（仅应用 per-channel 字段）。
      auto &cfg = doc->signal_config_store()->get_signal_config();
      cfg.work_mode = _device_agent->get_work_mode();
      // OPERATION_MODE/CHANNEL_MODE are PXLogic fork keys — only DSL/PXLogic
      // devices implement them. Task 10/Phase 3: read as strings.
      if (_device_agent->is_dsl_device()) {
        _device_agent->get_config_string(SR_CONF_OPERATION_MODE, cfg.operation_mode);
        _device_agent->get_config_string(SR_CONF_CHANNEL_MODE, cfg.channel_mode);
      }
      cfg.is_demo = _device_agent->is_demo();
      doc->apply_signal_config();
    } else {
      pxv_warn("MainWindow::load_config_from_json: no active document, "
               "skipping channel apply");
    }
  }

  // reload() rebuilds SignalModels from the (just-updated) sr_channel state
  // (probe->enabled/name/vdiv/coupling/vfactor set above). The DSO loop below
  // then operates on the NEW DsoSignal + NEW _model. Note: set_zero_ratio etc.
  // now use a local shared_ptr copy of _model (see dsosignal.cpp), so even if
  // set_config_* triggers a synchronous nested broadcast that deletes this
  // DsoSignal mid-method, the local copy keeps the SignalModel alive.
  _session->reload();

  // 毛刺滤波配置恢复：从 .pxl/.pxc 文件恢复阈值/模式/auto_apply。
  // 在 reload() 之后恢复，因为 reload 重建了 SignalModel，但滤波配置
  // 存储在 SessionData 中（不受 reload 影响）。
  // 实际滤波应用延迟到采集完成（auto_apply 路径）或用户打开面板时。
  if (sessionObj.contains("glitch_filter")) {
    QJsonObject glitchObj = sessionObj["glitch_filter"].toObject();
    _session->set_glitch_filter_auto_apply(glitchObj["auto_apply"].toBool(false));
    _session->set_show_glitch_filter_overlay(glitchObj["show_overlay"].toBool(true));

    // 恢复阈值/模式到 SessionData（不立即应用，等采集后 auto-apply 或用户手动应用）
    if (glitchObj["active"].toBool(false)) {
      std::map<int, uint32_t> thresholds;
      std::map<int, GlitchFilterMode> modes;
      QJsonArray thrArray = glitchObj["thresholds"].toArray();
      QJsonArray modeArray = glitchObj["modes"].toArray();
      for (const QJsonValue &v : thrArray) {
        QJsonObject e = v.toObject();
        thresholds[e["ch"].toInt()] = (uint32_t)e["threshold"].toInt();
      }
      for (const QJsonValue &v : modeArray) {
        QJsonObject e = v.toObject();
        modes[e["ch"].toInt()] = (GlitchFilterMode)e["mode"].toInt();
      }
      // 写入 SessionData 但不触发实际滤波（数据可能还没加载）
      // FilterProcessor 会读取这些值在 auto-apply 时使用
      _session->restore_glitch_filter_config(thresholds, modes);
    }
  }

  // load signal setting
  // Task 3: set_colour/set_trig(set_trig_type)/set_zero_ratio/set_trig_ratio
  // 等 view::Signal 调用予以保留（Task 13 进一步改走 SignalModel）。仅将 JSON
  // key 从 MainWindow 旧名（strigger/zeroPos/trigValue）改为 ChannelConfig 字段
  // 名（trig_type/zero_offset/trig_value）。其中 zero_offset/trig_value 在新格式
  // 下存的是 sr_channel 原始值（uint16_t/uint8_t），而 set_zero_ratio/
  // set_trig_ratio 期望 [0,1] 比例；apply_signal_config + load_settings 已从
  // probe 原始值恢复 _zero_offset/_trig_value，故仅当值落在 (0,1) 区间（旧比例
  // 格式）时才调用 set_*/set_trig_ratio，避免把原始值当比例写入导致错乱。
  if (mode == DSO) {
    for (auto s : current_view()->get_own_signals()) {
      for (const QJsonValue &value : sessionObj["channel"].toArray()) {
        QJsonObject obj = value.toObject();

        if (s->get_name() == obj["name"].toString() &&
            s->get_type() == obj["type"].toDouble()) {
          QString colourStr = obj["colour"].toString();
          // "default" 表示使用主题色板,不覆盖构造函数的颜色
          if (colourStr != "default")
            s->set_colour(QColor(colourStr));

          if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal *)s;
            dsoSig->load_settings();
            double zr = obj["zero_offset"].toDouble();
            if (zr > 0.0 && zr < 1.0)
              dsoSig->set_zero_ratio(zr);
            double tr = obj["trig_value"].toDouble();
            if (tr > 0.0 && tr < 1.0)
              dsoSig->set_trig_ratio(tr);
            dsoSig->commit_settings();
          }
          break;
        }
      }
    }
  } else {
    for (auto s : current_view()->get_own_signals()) {
      for (const QJsonValue &value : sessionObj["channel"].toArray()) {
        QJsonObject obj = value.toObject();
        if ((s->get_index() == obj["index"].toInt()) &&
            (s->get_type() == obj["type"].toInt())) {
          QString chan_name = obj["name"].toString().trimmed();
          if (chan_name == "") {
            chan_name = QString::number(s->get_index());
          }

          QString colourStr = obj["colour"].toString();
          // "default" 表示使用主题色板,不覆盖构造函数的颜色
          if (colourStr != "default")
            s->set_colour(QColor(colourStr));
          s->set_name(chan_name);

          view::LogicSignal *logicSig = nullptr;
          if ((logicSig = dynamic_cast<view::LogicSignal *>(s))) {
            // strigger → trig_type（ChannelConfig 字段名，int）
            logicSig->set_trig(obj["trig_type"].toInt());
          }

          if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal *)s;
            dsoSig->load_settings();
            double zr = obj["zero_offset"].toDouble();
            if (zr > 0.0 && zr < 1.0)
              dsoSig->set_zero_ratio(zr);
            double tr = obj["trig_value"].toDouble();
            if (tr > 0.0 && tr < 1.0)
              dsoSig->set_trig_ratio(tr);
            dsoSig->commit_settings();
          }

          if (s->signal_type() == SR_CHANNEL_ANALOG) {
            view::AnalogSignal *analogSig = (view::AnalogSignal *)s;
            // AnalogSignal 无 load_settings()，且构造函数读 model->zero_offset
            // (reload 从 driver PROBE_OFFSET 填充)，故 _zero_offset 不会由
            // apply_signal_config + reload 自动恢复。这里把存为原始 uint16_t 的
            // zero_offset 经 value2ratio 转成比例后用 set_zero_ratio 还原。
            // 若值落在 (0,1)（旧比例格式）则直接当比例用（无兼容性要求，仅稳健）。
            // 特殊情况：zv == 0 表示 ANALOG 通道无硬件偏移（demo 驱动返回 0），
            // value2ratio(0) 会 clamp 到 0.0（顶部），导致游标在顶部而非中心。
            // 此时用 0.5（中心）作为默认值。
            double zv = obj["zero_offset"].toDouble();
            double ratio_z;
            if (zv > 0.0 && zv < 1.0) {
              ratio_z = zv;
            } else if (zv == 0.0) {
              ratio_z = 0.5;
            } else {
              ratio_z = analogSig->value2ratio((int)zv);
            }
            analogSig->set_zero_ratio(ratio_z);
            analogSig->commit_settings();
          }

          break;
        }
      }
    }
  }

  // update UI settings
  _sampling_bar->update_sample_rate_list();
  _trigger_widget->device_updated();
  current_view()->header_updated();

  // load trigger settings
  // Task 6: trigger 反序列化改走 Core TriggerConfig（唯一真相源）。
  // from_json() 读 to_json() 写入的新结构；set_trigger_config() broadcasts
  // TriggerConfigChanged；随后 refresh_ui_from_core() 把 Core
  // 状态映射到 TriggerDock 控件（View 层不再解析 trigger JSON）。
  if (sessionObj.contains("trigger")) {
    _session->set_trigger_config(
        data::TriggerConfig::from_json(sessionObj["trigger"].toObject()));
    _trigger_widget->refresh_ui_from_core();
  }

  // load decoders
  if (sessionObj.contains("decoder")) {
    QJsonArray deArray = sessionObj["decoder"].toArray();
    if (deArray.empty() == false) {
      haveDecoder = true;
      StoreSession ss(_session);
      ss.load_decoders(_protocol_widget, deArray);
      current_view()->update_all_trace_postion();
    }
  }

  // load measure
  if (sessionObj.contains("measure")) {
    auto *bottom_bar = current_view()->get_viewstatus();
    bottom_bar->load_session(sessionObj["measure"].toArray(), format_ver);
  }

  return true;
}

bool MainWindow::on_store_session(QString name) {
  return save_config_to_file(name);
}

bool MainWindow::save_config_to_file(QString name) {
  if (name == "") {
    pxv_err("Session file name is empty.");
    assert(false);
  }

  std::string file_name = pv::path::ToUnicodePath(name);
  pxv_info("Store session to file: \"%s\"", file_name.c_str());

  QFile sf(name);
  if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
    pxv_warn("Warning: Couldn't open profile to write!");
    return false;
  }

  QTextStream outStream(&sf);
  encoding::set_utf8(outStream);

  QJsonObject sessionVar;
  if (!gen_config_json(sessionVar)) {
    return false;
  }

  QJsonDocument sessionDoc(sessionVar);
  outStream << QString::fromUtf8(sessionDoc.toJson());
  sf.close();
  return true;
}

bool MainWindow::genSessionData(std::string &str) {
  QJsonObject sessionVar;
  if (!gen_config_json(sessionVar)) {
    return false;
  }

  QJsonDocument sessionDoc(sessionVar);
  QString data = QString::fromUtf8(sessionDoc.toJson());
  str.append(data.toLocal8Bit().data());
  return true;
}

::DockOptions *MainWindow::getDockOptions() {
  AppConfig &app = AppConfig::Instance();
  int mode = _device_agent->get_work_mode();
  if (mode == LOGIC)
    return &app.frameOptions._logicDock;
  else if (mode == DSO)
    return &app.frameOptions._dsoDock;
  else
    return &app.frameOptions._analogDock;
}

void MainWindow::restore_dock() {
  if (_device_agent->have_instance())
    _trig_bar->reload();

  _side_bar->clearAllChecked();

  ::DockOptions *opt = getDockOptions();
  if (opt) {
    if (opt->decodeDock) {
      _side_bar->setItemChecked(SIDEBAR_DECODE, true);
      _sliding_drawer->open(_drawer_page_protocol);
      _drawer_current_page = _drawer_page_protocol;
    } else if (opt->triggerDock) {
      _side_bar->setItemChecked(SIDEBAR_TRIGGER, true);
      int mode = _device_agent->get_work_mode();
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
      _side_bar->setItemChecked(SIDEBAR_MEASURE, true);
      _sliding_drawer->open(_drawer_page_measure);
      _drawer_current_page = _drawer_page_measure;
    } else if (opt->searchDock) {
      _side_bar->setItemChecked(SIDEBAR_SEARCH, true);
      current_view()->show_search_cursor(true);
      _sliding_drawer->open(_drawer_page_search);
      _drawer_current_page = _drawer_page_search;
    } else if (opt->deviceOptionsDock) {
      _side_bar->setItemChecked(SIDEBAR_OPTIONS, true);
      _device_options_widget->update_view();
      _sliding_drawer->open(_drawer_page_device_options);
      _drawer_current_page = _drawer_page_device_options;
    } else if (opt->logDock) {
      _side_bar->setItemChecked(SIDEBAR_LOG, true);
      _sliding_drawer->open(_drawer_page_log);
      _drawer_current_page = _drawer_page_log;
    }
  }
}

int MainWindow::resolveShortcutAction(int key, int modifiers) {
  AppConfig &app = AppConfig::Instance();
  int count = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&count);

  for (int i = 0; i < count; i++) {
    QString keySeqStr;

    bool found = false;
    for (int j = 0; j < app.shortcutOptions.items.size(); j++) {
      if (app.shortcutOptions.items[j].actionId == infos[i].actionId) {
        keySeqStr = app.shortcutOptions.items[j].keySequence;
        found = true;
        break;
      }
    }

    if (!found || keySeqStr.isEmpty()) {
      keySeqStr = infos[i].keySequence;
    }

    QKeySequence seq(keySeqStr);
    if (seq.count() > 0) {
      QKeyCombination combined = seq[0];
      int combinedInt = combined.toCombined();
      int seqKey = combinedInt & ~Qt::KeyboardModifierMask;
      int seqMods = combinedInt & Qt::KeyboardModifierMask;

      if (seqMods == 0 && modifiers == 0 && seqKey == key) {
        return infos[i].actionId;
      }

      if (seqMods != 0) {
        bool modsMatch = true;
        if ((seqMods & Qt::ShiftModifier) && !(modifiers & Qt::ShiftModifier))
          modsMatch = false;
        if ((seqMods & Qt::ControlModifier) &&
            !(modifiers & Qt::ControlModifier))
          modsMatch = false;
        if ((seqMods & Qt::AltModifier) && !(modifiers & Qt::AltModifier))
          modsMatch = false;
        if (modsMatch && seqKey == key) {
          return infos[i].actionId;
        }
      }
    }
  }

  return 0;
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
  (void)object;

  if (event->type() == QEvent::KeyPress) {
    static bool in_filter = false;
    if (in_filter)
      return false;

    QKeyEvent *ke = (QKeyEvent *)event;
    QWidget *focused = qApp->focusWidget();

    pxv_info("MainWindow::eventFilter key=%d, object=%p (%s), focused=%p (%s)",
             ke->key(), object, object->metaObject()->className(), focused,
             focused ? focused->metaObject()->className() : "nullptr");

    if (focused && qobject_cast<pv::widgets::SearchPatternInput *>(focused)) {
      in_filter = true;
      qApp->sendEvent(focused, event);
      in_filter = false;
      return true;
    }

    // Manually forward events to focus widget if it's an input or in the drawer
    if (focused &&
        (qobject_cast<QLineEdit *>(focused) ||
         qobject_cast<QAbstractSpinBox *>(focused) ||
         qobject_cast<QComboBox *>(focused) ||
         qobject_cast<QAbstractButton *>(focused) ||
         (_sliding_drawer && _sliding_drawer->isAncestorOf(focused)) ||
         (_device_options_widget &&
          _device_options_widget->isAncestorOf(focused)) ||
         (_search_widget && _search_widget->isAncestorOf(focused)) ||
         (_trigger_widget && _trigger_widget->isAncestorOf(focused)) ||
         (_protocol_widget && _protocol_widget->isAncestorOf(focused)) ||
         (_dso_trigger_widget && _dso_trigger_widget->isAncestorOf(focused)) ||
         (_measure_widget && _measure_widget->isAncestorOf(focused)))) {
      QWidget *target = focused;
      if (focused->focusProxy()) {
        target = focused->focusProxy();
      } else if (qobject_cast<QAbstractSpinBox *>(focused) ||
                 qobject_cast<QComboBox *>(focused)) {
        QLineEdit *le = focused->findChild<QLineEdit *>();
        if (le) {
          target = le;
        }
      }

      QString text = ke->text();
      uint key = ke->key();

      // Fix for WinNativeWidget's raw VK codes
      if (key == 0x08)
        key = Qt::Key_Backspace;
      else if (key == 0x0D)
        key = Qt::Key_Return;
      else if (key == 0x25)
        key = Qt::Key_Left;
      else if (key == 0x26)
        key = Qt::Key_Up;
      else if (key == 0x27)
        key = Qt::Key_Right;
      else if (key == 0x28)
        key = Qt::Key_Down;
      else if (key == 0x2E)
        key = Qt::Key_Delete;
      else if (key == 0x24)
        key = Qt::Key_Home;
      else if (key == 0x23)
        key = Qt::Key_End;
      else if (key >= 0x60 && key <= 0x69) // VK_NUMPAD0 to VK_NUMPAD9
        key = Qt::Key_0 + (key - 0x60);
      else if (key == 0x6A) // VK_MULTIPLY
        key = Qt::Key_Asterisk;
      else if (key == 0x6B) // VK_ADD
        key = Qt::Key_Plus;
      else if (key == 0x6D) // VK_SUBTRACT
        key = Qt::Key_Minus;
      else if (key == 0x6E) // VK_DECIMAL
        key = Qt::Key_Period;
      else if (key == 0x6F) // VK_DIVIDE
        key = Qt::Key_Slash;

      if (text.isEmpty() && target->inherits("QLineEdit")) {
        if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
          char c = (char)key;
          bool shift = (ke->modifiers() & Qt::ShiftModifier);
          if (c >= 'A' && c <= 'Z' && !shift) {
            c += 32;
          } else if (c >= 'a' && c <= 'z' && shift) {
            c -= 32;
          }
          text = QString(QChar(c));
        }
      }

      QKeyEvent newEvent(ke->type(), key, ke->modifiers(), text,
                         ke->isAutoRepeat(), ke->count());

      pxv_info("  Forwarding event to focused widget: %s (target: %s, text: "
               "%s, mapped_key: %d)",
               focused->metaObject()->className(),
               target->metaObject()->className(), text.toStdString().c_str(),
               key);
      in_filter = true;
      qApp->sendEvent(target, &newEvent);
      in_filter = false;
      return true;
    }

    const auto &sigs = current_view()->get_own_signals();

    int modifier = ke->modifiers();

    // Ctrl+Z — undo the most recent glitch filter application (Task 9).
    // Handled here before the generic shortcut resolver because the
    // configurable shortcut system does not define an Undo action; the
    // generic path below would otherwise consume Ctrl+Z (returns true for
    // unrecognized Ctrl combos) and swallow the keystroke.
    if ((modifier & Qt::ControlModifier) && ke->key() == Qt::Key_Z) {
      pv::view::View *view = current_view();
      if (view && view->can_undo_filter()) {
        view->undo_filter();
        return true;
      }
    }

    int action = resolveShortcutAction(ke->key(), (int)modifier);
    if (action == 0) {
      if (modifier & Qt::ControlModifier || modifier & Qt::AltModifier) {
        return true;
      }
      return false;
    }

    switch (action) {
    case SHORTCUT_RUN_STOP:
      _sampling_bar->run_or_stop();
      break;
    case SHORTCUT_INSTANT:
      _sampling_bar->run_or_stop_instant();
      break;
    case SHORTCUT_TRIGGER:
      _side_bar->getItem(SIDEBAR_TRIGGER)->button->click();
      break;
    case SHORTCUT_DECODE:
      _side_bar->getItem(SIDEBAR_DECODE)->button->click();
      break;
    case SHORTCUT_MEASURE:
      _side_bar->getItem(SIDEBAR_MEASURE)->button->click();
      break;
    case SHORTCUT_SEARCH:
      _side_bar->getItem(SIDEBAR_SEARCH)->button->click();
      break;
    case SHORTCUT_OPTIONS:
      _side_bar->getItem(SIDEBAR_OPTIONS)->button->click();
      break;
    case SHORTCUT_DEVICE_SELECT:
      _sampling_bar->device_selected();
      break;
    case SHORTCUT_PAGE_UP:
      current_view()->set_scale_offset(current_view()->scale(),
                                       current_view()->offset() -
                                           current_view()->get_view_width());
      break;
    case SHORTCUT_PAGE_DOWN:
      current_view()->set_scale_offset(current_view()->scale(),
                                       current_view()->offset() +
                                           current_view()->get_view_width());
      break;
    case SHORTCUT_ZOOM_IN:
      current_view()->zoom(1);
      break;
    case SHORTCUT_ZOOM_OUT:
      current_view()->zoom(-1);
      break;
    case SHORTCUT_DSO_CH0:
      for (auto s : sigs) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
          view::DsoSignal *dsoSig = (view::DsoSignal *)s;
          if (dsoSig->get_index() == 0)
            dsoSig->set_vDialActive(!dsoSig->get_vDialActive());
          else
            dsoSig->set_vDialActive(false);
        }
      }
      current_view()->setFocus();
      update();
      break;
    case SHORTCUT_DSO_CH1:
      for (auto s : sigs) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
          view::DsoSignal *dsoSig = (view::DsoSignal *)s;
          if (dsoSig->get_index() == 1)
            dsoSig->set_vDialActive(!dsoSig->get_vDialActive());
          else
            dsoSig->set_vDialActive(false);
        }
      }
      current_view()->setFocus();
      update();
      break;
    case SHORTCUT_DSO_VUP:
      for (auto s : sigs) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
          view::DsoSignal *dsoSig = (view::DsoSignal *)s;
          if (dsoSig->get_vDialActive()) {
            dsoSig->go_vDialNext(true);
            update();
            break;
          }
        }
      }
      break;
    case SHORTCUT_DSO_VDOWN:
      for (auto s : sigs) {
        if (s->signal_type() == SR_CHANNEL_DSO) {
          view::DsoSignal *dsoSig = (view::DsoSignal *)s;
          if (dsoSig->get_vDialActive()) {
            dsoSig->go_vDialPre(true);
            update();
            break;
          }
        }
      }
      break;
    case SHORTCUT_FILE_OPEN:
      _file_bar->_action_open->trigger();
      break;
    case SHORTCUT_FILE_SAVE:
      _file_bar->_action_save->trigger();
      break;
    case SHORTCUT_FILE_EXPORT:
      _file_bar->_action_export->trigger();
      break;
    case SHORTCUT_FILE_IMPORT:
      _file_bar->_action_import->trigger();
      break;
    case SHORTCUT_FILE_LOAD:
      _file_bar->_action_load->trigger();
      break;
    case SHORTCUT_FILE_STORE:
      _file_bar->_action_store->trigger();
      break;
    case SHORTCUT_SCREENSHOT:
      _file_bar->_action_capture->trigger();
      break;
    case SHORTCUT_FFT:
      _trig_bar->_action_fft->trigger();
      break;
    case SHORTCUT_MATH:
      _trig_bar->_action_math->trigger();
      break;
    case SHORTCUT_LISSAJOUS:
      _trig_bar->_action_lissajous->trigger();
      break;
    case SHORTCUT_SETTINGS:
      _trig_bar->_action_dispalyOptions->trigger();
      break;
    case SHORTCUT_LOG:
      _side_bar->getItem(SIDEBAR_LOG)->button->click();
      break;
    case SHORTCUT_FUNCTION:
      _side_bar->getItem(SIDEBAR_FUNCTION)->button->click();
      break;
    case SHORTCUT_THEME_TOGGLE: {
      AppConfig &app = AppConfig::Instance();
      if (app.IsDarkStyle())
        switchTheme(THEME_STYLE_LIGHT);
      else
        switchTheme(THEME_STYLE_DARK);
      break;
    }
    case SHORTCUT_NEW_TAB:
      on_new_tab_requested();
      break;
    case SHORTCUT_CLOSE_TAB:
      if (_tab_widget && _tab_widget->count() > 0)
        remove_tab(_tab_widget->currentIndex());
      break;
    case SHORTCUT_ZOOM_FIT:
      if (current_view()) {
        current_view()->auto_set_max_scale();
        current_view()->set_scale_offset(current_view()->scale(), 0);
      }
      break;
    default:
      return false;
    }
    return true;
  }
  return false;
}

void MainWindow::switchLanguage(int language) {
  if (language == 0)
    return;

  AppConfig &app = AppConfig::Instance();

  if (app.frameOptions.language != language && language > 0) {
    app.frameOptions.language = language;
    app.SaveFrame();
    LangResource::Instance()->Load(language);
  }

  if (language == LAN_CN) {
    (void)_qtTrans.load(":/qt_" + QString::number(language));
    qApp->installTranslator(&_qtTrans);
    (void)_myTrans.load(":/my_" + QString::number(language));
    qApp->installTranslator(&_myTrans);
  } else if (language == LAN_EN) {
    qApp->removeTranslator(&_qtTrans);
    qApp->removeTranslator(&_myTrans);
  }

  retranslateUi();

  UiManager::Instance()->Update(UI_UPDATE_ACTION_LANG);
  _session->update_lang_text();
}

void MainWindow::switchTheme(QString style) {
  AppConfig &app = AppConfig::Instance();

  if (app.frameOptions.style != style) {
    app.frameOptions.style = style;
    app.SaveFrame();
  }

  QString qssRes = ":/theme.qss";
  QFile qss(qssRes);
  if (!qss.open(QFile::ReadOnly | QFile::Text)) {
    return;
  }
  QString qssContent = qss.readAll();
  qss.close();

  QHash<QString, QString> tokens;

  // Load base tokens from JSON schema instance
  QString jsonRes = ":/" + style + ".json";
  QFile jsonFile(jsonRes);
  if (jsonFile.open(QFile::ReadOnly | QFile::Text)) {
    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonFile.readAll());
    QJsonObject rootObj = jsonDoc.object();
    QJsonObject tokensObj = rootObj.value("tokens").toObject();
    for (const QString &key : tokensObj.keys()) {
      tokens[key] = tokensObj.value(key).toString();
    }
    jsonFile.close();
  } else {
    // Fallback: parse from QSS if JSON is missing
    QRegularExpression tokenRe(
        "@([\\w-]+):\\s*([^\\r\\n]+?)\\s*(?:\\*/|\\r|\\n)");
    QRegularExpressionMatchIterator it = tokenRe.globalMatch(qssContent);
    while (it.hasNext()) {
      QRegularExpressionMatch match = it.next();
      QString tokenName = "@" + match.captured(1);
      QString tokenValue = match.captured(2).trimmed();
      tokens[tokenName] = tokenValue;
    }
  }

  for (int i = 0; i < app.styleOptions.items.size(); i++) {
    tokens[app.styleOptions.items[i].tokenName] =
        app.styleOptions.items[i].value;
  }

  QList<QString> keys = tokens.keys();
  std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
    return a.length() > b.length();
  });

  for (const QString &key : keys) {
    qssContent.replace(key, tokens[key]);
  }

  // Process SVG files that contain token placeholders (e.g. @accent)
  QString tempDir =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
      "/pxview_themed_svgs";
  QDir().mkpath(tempDir);

  QRegularExpression svgRe("image:\\s*url\\((:[^)]+\\.svg)\\)");
  QRegularExpressionMatchIterator svgIt = svgRe.globalMatch(qssContent);
  QSet<QString> processedSvgs;
  while (svgIt.hasNext()) {
    QRegularExpressionMatch match = svgIt.next();
    QString svgResPath = match.captured(1);

    if (processedSvgs.contains(svgResPath))
      continue;
    processedSvgs.insert(svgResPath);

    QFile svgFile(svgResPath);
    if (!svgFile.open(QFile::ReadOnly | QFile::Text))
      continue;
    QString svgContent = svgFile.readAll();
    svgFile.close();

    bool hasPlaceholders = false;
    for (const QString &key : keys) {
      if (svgContent.contains(key)) {
        hasPlaceholders = true;
        break;
      }
    }
    if (!hasPlaceholders)
      continue;

    for (const QString &key : keys) {
      svgContent.replace(key, tokens[key]);
    }

    QString fileName = svgResPath;
    fileName.replace(":/", "");
    fileName.replace("/", "_");
    QString tempPath = tempDir + "/" + fileName;
    QFile tempFile(tempPath);
    if (tempFile.open(QFile::WriteOnly | QFile::Text)) {
      tempFile.write(svgContent.toUtf8());
      tempFile.close();
    }

    qssContent.replace(svgResPath, tempPath);
  }

  app.SetThemeTokens(tokens);

  qApp->setStyleSheet(qssContent);

  UiManager::Instance()->Update(UI_UPDATE_ACTION_THEME);
  UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);

  data_updated();
  Ribbon_retranslateUi();
}

void MainWindow::data_updated() {
  _event.data_updated(); // safe call
}

void MainWindow::on_data_updated() {
  _measure_widget->reCalc();
  current_view()->data_updated();
}

void MainWindow::on_open_doc() { openDoc(); }

void MainWindow::openDoc() {
  QDir dir(GetAppDataDir());
  AppConfig &app = AppConfig::Instance();
  int lan = app.frameOptions.language;
  QDesktopServices::openUrl(QUrl("file:///" + dir.absolutePath() + "/ug" +
                                 QString::number(lan) + ".pdf"));
}

void MainWindow::update_capture() { _event.update_capture_sig(); }

void MainWindow::on_update_capture() { current_view()->update_hori_res(); }

void MainWindow::cur_snap_samplerate_changed() {
  _event.cur_snap_samplerate_changed(); // safe call
}

void MainWindow::on_cur_snap_samplerate_changed() {
  _measure_widget->reCalc();
  update_sample_period();
}

/*------------------on event end-------*/

void MainWindow::signals_changed() {
  _event.signals_changed(); // safe call
}

void MainWindow::on_signals_changed() {
  // Rebuild View signals from current SignalModels
  // (SignalFactory::update_signals with AllReplaced preserves UI state), then
  // refresh layout. This ensures LogicSignals pick up new SignalModel pointers
  // and Qt signal/slot connections are re-established after
  // init_signals()/reload() recreates models.
  current_view()->on_signals_changed();
}

void MainWindow::receive_trigger(quint64 trigger_pos) {
  _event.receive_trigger(trigger_pos); // save call
}

void MainWindow::on_receive_trigger(quint64 trigger_pos) {
  current_view()->receive_trigger(trigger_pos);
}

void MainWindow::frame_ended() {
  _event.frame_ended(); // save call
}

void MainWindow::on_frame_ended() {
  pxv_info("MainWindow::on_frame_ended() [UI-only: Core handles copy+decode+guard]");
  _acq_count++;
  _side_bar->setItemRunning(SIDEBAR_RUNSTOP, false);
  _side_bar->setItemRunning(SIDEBAR_INSTANT, false);

  // CRITICAL FIX (fork 迁移遗漏): 采集结束时更新所有 UI 组件的 enabled
  // 状态。is_working() 此时已为 false（action_stop_capture 或 SR_DF_END 路径
  // 设置），update_capture_ui_status() 会据此启用 toolbar/sidebar 按钮、
  // protocol dock 和 device options dock。
  //
  // 之前的问题：single 模式手动停止时，EndCollectWork 不被广播（只在 repeat
  // 模式广播，见 capturemanager.cpp:496-498），而 on_event(EndCollectWorkPrev)
  // 在 GUI 模式下是空操作。所以 UI 状态永远不会被更新，按钮保持禁用状态。
  //
  // 使用统一的 update_capture_ui_status() 而非单独调用各个 update 方法，
  // 确保所有采集状态相关的 UI 组件同步更新，避免遗漏。
  update_capture_ui_status();

  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document()) {
    // CRITICAL FIX: copy_data_to_document + start_all_decode_tasks are now
    // handled exclusively by Core layer:
    //   LOGIC mode: SigSession::on_event(RevEndPacket) → bg copy thread →
    //               CopyToDocDone handler (or ELSE branch: direct decode +
    //               guard release for stream mode).
    //   non-LOGIC mode: DataFeedParser SR_DF_END else branch.
    // MainWindow previously did a DUPLICATE synchronous copy here, which raced
    // with the background copy thread and never released the CaptureOwnerGuard,
    // causing wait_capture_complete to time out forever.
    ctx->document()->save_signal_config(
        _session->get_signal_models(), build_channel_layout(current_view()));
  }
  current_view()->receive_end();
}

void MainWindow::frame_began() {
  _event.frame_began(); // save call
}

void MainWindow::on_frame_began() {
  if (_session->is_instant()) {
    _side_bar->setItemRunning(SIDEBAR_INSTANT, true);
  } else {
    _side_bar->setItemRunning(SIDEBAR_RUNSTOP, true);
  }
  pv::TabContext *ctx = current_context();
  if (ctx) {
    ctx->make_live();
    if (ctx->document()) {
      ctx->document()->clear();
      // Task 11.3 (R6 对称): is_working 时跳过 set_active_document，
      // 避免覆盖 capture owner——后台采集进行中切换 active 会造成数据归属错乱。
      // END_COLLECT_WORK 时会显式恢复当前 tab 的 active_document 归属。
      if (!_session->is_working()) {
        _session->set_active_document(ctx->document());
      }
    }
    current_view()->set_signal_data_from_source(_session);
  }
  current_view()->frame_began();
}

void MainWindow::show_region(uint64_t start, uint64_t end, bool keep) {
  _event.show_region_sig((quint64)start, (quint64)end, keep);
}

void MainWindow::on_show_region(quint64 start, quint64 end, bool keep) {
  current_view()->show_region((uint64_t)start, (uint64_t)end, keep);
}

void MainWindow::show_wait_trigger() { _event.show_wait_trigger_sig(); }

void MainWindow::on_show_wait_trigger() { current_view()->show_wait_trigger(); }

void MainWindow::repeat_hold(int percent) { _event.repeat_hold_sig(percent); }

void MainWindow::on_repeat_hold(int percent) {
  (void)percent;
  current_view()->repeat_show();
}

void MainWindow::decode_done() {
  _event.decode_done(); // safe call
}

void MainWindow::on_decode_done() { _protocol_widget->update_model(); }

void MainWindow::receive_data_len(quint64 len) {
  _event.receive_data_len(len); // safe call
}

void MainWindow::on_receive_data_len(quint64 len) {
  current_view()->set_receive_len(len);
}

void MainWindow::receive_header() {}

void MainWindow::check_usb_device_speed() {
  // USB device speed check
  if (_device_agent->is_hardware()) {
    // SR_CONF_USB_SPEED/USB30_SUPPORT fork keys were deleted from pxlogic.c.
    // The link speed is now read directly from libusb via the typed wrapper
    // DeviceAgent::get_usb_speed() (calls sr_dev_inst_usb_speed_get).
    int usb_speed = _device_agent->get_usb_speed();
    if (usb_speed == LIBUSB_SPEED_UNKNOWN) {
      // Non-USB or speed undeterminable — nothing to check.
      return;
    }

    // is_usb30() returns true only for SUPER/SUPER_PLUS. For UNKNOWN we
    // conservatively treat as USB 2.0 (no warning shown).
    bool usb30_support = _device_agent->is_usb30();
    pxv_info("The device's USB module version: %d.0", usb30_support ? 3 : 2);

    int cable_ver = 1;
    if (usb_speed == LIBUSB_SPEED_HIGH)
      cable_ver = 2;
    else if (usb_speed == LIBUSB_SPEED_SUPER)
      cable_ver = 3;

    pxv_info("The cable's USB port version: %d.0", cable_ver);

    if (usb30_support && usb_speed == LIBUSB_SPEED_HIGH) {
      QString str_err(
          L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHECK_USB_SPEED_ERROR),
              "Plug the device into a USB 2.0 port will seriously affect its "
              "performance.\nPlease replug it into a USB 3.0 port."));
      delay_prop_msg(str_err);
    }
  }
}

void MainWindow::reset_all_view() {
  _sampling_bar->reload();
  current_view()->status_clear();
  current_view()->reload();
  current_view()->set_device();
  _trigger_widget->update_view();
  _trigger_widget->device_updated();
  _trig_bar->reload();
  _dso_trigger_widget->update_view();
  _measure_widget->reload();
  // DeviceOptionsDock refresh is handled by the caller:
  //   - DeviceModeChanged  → on_mode_changed() (lightweight, preserves scaffolding)
  //   - CurrentDeviceChanged → update_view() (full rebuild, called explicitly at line ~3160)
  // if (_sliding_drawer->isOpen())
  //   _sliding_drawer->close();
  // _side_bar->clearAllChecked();

  if (_device_agent->get_work_mode() == ANALOG)
    current_view()->get_viewstatus()->setVisible(false);
  else
    current_view()->get_viewstatus()->setVisible(true);
}

bool MainWindow::confirm_to_store_data() {
  bool ret = false;
  _is_save_confirm_msg = true;

  if (_session->have_hardware_data() && _session->is_first_store_confirm()) {
    // Only popup one time.
    ret = MsgBox::Confirm(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_SAVE_CAPDATE), "Save captured data?"));

    if (!ret && _is_auto_switch_device) {
      pxv_info("The data save confirm end, auto switch to the new device.");
      _is_auto_switch_device = false;

      if (_session->is_working())
        _session->stop_capture();

      _session->set_default_device();
    }
  }

  _is_save_confirm_msg = false;
  return ret;
}

void MainWindow::check_config_file_version() {
  auto device_agent = _session->get_device();
  if (device_agent->is_file() && device_agent->is_new_device()) {
    if (device_agent->get_work_mode() == LOGIC) {
      int version = -1;
      if (device_agent->get_config_int16(SR_CONF_FILE_VERSION, version)) {
        if (version == 1) {
          QString strMsg(
              L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHECK_SESSION_FILE_VERSION_ERROR),
                  "Current loading file has an old format. \nThis will lead to "
                  "a slow loading speed. \nPlease resave it after loaded."));
          MsgBox::Show(strMsg);
        }
      }
    }
  }
}

void MainWindow::load_device_config() {
  _title_ext_string = "";
  int mode = _device_agent->get_work_mode();
  QString file;

  if (_device_agent->is_hardware() && !_device_agent->is_demo()) {
    QString ses_name = gen_config_file_path(true);

    bool bExist = false;

    QFile sf(ses_name);
    if (!sf.exists()) {
      pxv_info("Try to load the low version profile.");
      ses_name = gen_config_file_path(false);
    } else {
      bExist = true;
    }

    if (!bExist) {
      QFile sf2(ses_name);
      if (!sf2.exists()) {
        pxv_info("Try to load the default profile.");
        ses_name = _file_bar->genDefaultSessionFile();
      }
    }

    file = ses_name;
  } else if (_device_agent->is_demo()) {
    QDir dir(GetFirmwareDir());
    if (dir.exists()) {
      QString ses_name = dir.absolutePath() + "/" +
                         _device_agent->driver_name() + QString::number(mode) +
                         ".pxc";

      QFile sf(ses_name);
      if (sf.exists()) {
        file = ses_name;
      }
    }
  }

  if (file != "") {
    bool ret = load_config_from_file(file);
    if (ret && _device_agent->is_hardware()) {
      _title_ext_string = file;
    }
  }
}

QJsonDocument MainWindow::get_config_json_from_data_file(QString file,
                                                         bool &bSucesss) {
  QJsonDocument sessionDoc;
  QJsonParseError error;
  bSucesss = false;

  if (file == "") {
    pxv_err("File name is empty.");
    assert(false);
  }

  auto f_name = pv::path::ConvertPath(file);
  ZipReader rd(f_name.c_str());
  auto *data = rd.GetInnterFileData("session");

  if (data != nullptr) {
    QByteArray raw_bytes = QByteArray::fromRawData(data->data(), data->size());
    QString jsonStr(raw_bytes.data());
    QByteArray qbs = jsonStr.toUtf8();
    sessionDoc = QJsonDocument::fromJson(qbs, &error);

    if (error.error != QJsonParseError::NoError) {
      QString estr = error.errorString();
      pxv_err("File::get_session(), parse json error:\"%s\"!",
              estr.toUtf8().data());
    } else {
      bSucesss = true;
    }

    rd.ReleaseInnerFileData(data);
  }

  return sessionDoc;
}

QJsonArray MainWindow::get_decoder_json_from_data_file(QString file,
                                                       bool &bSucesss) {
  QJsonArray dec_array;
  QJsonParseError error;

  bSucesss = false;

  if (file == "") {
    pxv_err("File name is empty.");
    assert(false);
  }

  /* read "decoders" */
  auto f_name = path::ConvertPath(file);
  ZipReader rd(f_name.c_str());
  auto *data = rd.GetInnterFileData("decoders");

  if (data != nullptr) {
    QByteArray raw_bytes = QByteArray::fromRawData(data->data(), data->size());
    QString jsonStr(raw_bytes.data());
    QByteArray qbs = jsonStr.toUtf8();
    QJsonDocument sessionDoc = QJsonDocument::fromJson(qbs, &error);

    if (error.error != QJsonParseError::NoError) {
      QString estr = error.errorString();
      pxv_err(
          "MainWindow::get_decoder_json_from_file(), parse json error:\"%s\"!",
          estr.toUtf8().data());
    } else {
      bSucesss = true;
    }

    dec_array = sessionDoc.array();
    rd.ReleaseInnerFileData(data);
  }

  return dec_array;
}

void MainWindow::update_capture_ui_status() {
  update_toolbar_view_status();
  _protocol_widget->update_view_status();
  _device_options_widget->update_widgets_status();
}

void MainWindow::update_toolbar_view_status() {
  _sampling_bar->update_view_status();
  _file_bar->update_view_status();
  _trig_bar->update_view_status();

  bool bEnable = _session->is_working() == false;
  int mode = _device_agent->get_work_mode();

  _side_bar->setItemEnabled(SIDEBAR_TRIGGER, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_DECODE, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_MEASURE, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_SEARCH, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_FUNCTION, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_OPTIONS, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_MCP, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_LOG, bEnable);
  _side_bar->setItemEnabled(SIDEBAR_RUNSTOP, true);
  _side_bar->setItemEnabled(SIDEBAR_INSTANT, true);

  if (_session->is_working() && mode == DSO) {
    if (_session->is_instant() == false) {
      _side_bar->setItemEnabled(SIDEBAR_TRIGGER, true);
      _side_bar->setItemEnabled(SIDEBAR_MEASURE, true);
      _side_bar->setItemEnabled(SIDEBAR_FUNCTION, true);
      _side_bar->setItemEnabled(SIDEBAR_OPTIONS, true);
    }
  }

  if (mode == LOGIC) {
    _side_bar->setItemVisible(SIDEBAR_TRIGGER, true);
    _side_bar->setItemVisible(SIDEBAR_DECODE, true);
    _side_bar->setItemVisible(SIDEBAR_MEASURE, true);
    _side_bar->setItemVisible(SIDEBAR_SEARCH, true);
    _side_bar->setItemVisible(SIDEBAR_FUNCTION, false);
    _side_bar->setItemVisible(SIDEBAR_OPTIONS, true);
    _side_bar->setItemVisible(SIDEBAR_MCP, true);
    _side_bar->setItemVisible(SIDEBAR_LOG, true);
    _side_bar->setItemVisible(SIDEBAR_RUNSTOP, true);
    _side_bar->setItemVisible(SIDEBAR_INSTANT, true);
  } else if (mode == ANALOG) {
    _side_bar->setItemVisible(SIDEBAR_TRIGGER, false);
    _side_bar->setItemVisible(SIDEBAR_DECODE, false);
    _side_bar->setItemVisible(SIDEBAR_MEASURE, true);
    _side_bar->setItemVisible(SIDEBAR_SEARCH, false);
    _side_bar->setItemVisible(SIDEBAR_FUNCTION, false);
    _side_bar->setItemVisible(SIDEBAR_OPTIONS, true);
    _side_bar->setItemVisible(SIDEBAR_MCP, true);
    _side_bar->setItemVisible(SIDEBAR_LOG, true);
    _side_bar->setItemVisible(SIDEBAR_RUNSTOP, true);
    _side_bar->setItemVisible(SIDEBAR_INSTANT, false);
  } else if (mode == DSO) {
    _side_bar->setItemVisible(SIDEBAR_TRIGGER, true);
    _side_bar->setItemVisible(SIDEBAR_DECODE, false);
    _side_bar->setItemVisible(SIDEBAR_MEASURE, true);
    _side_bar->setItemVisible(SIDEBAR_SEARCH, false);
    _side_bar->setItemVisible(SIDEBAR_FUNCTION, true);
    _side_bar->setItemVisible(SIDEBAR_OPTIONS, true);
    _side_bar->setItemVisible(SIDEBAR_MCP, true);
    _side_bar->setItemVisible(SIDEBAR_LOG, true);
    _side_bar->setItemVisible(SIDEBAR_RUNSTOP, true);
    _side_bar->setItemVisible(SIDEBAR_INSTANT, true);
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
      should_close = !_side_bar->isItemVisible(SIDEBAR_TRIGGER);
    else if (cp == _drawer_page_protocol)
      should_close = !_side_bar->isItemVisible(SIDEBAR_DECODE);
    else if (cp == _drawer_page_search)
      should_close = !_side_bar->isItemVisible(SIDEBAR_SEARCH);
    else if (cp == _drawer_page_function)
      should_close = !_side_bar->isItemVisible(SIDEBAR_FUNCTION);
    if (should_close) {
      _sliding_drawer->close();
      _side_bar->clearAllChecked();
      _drawer_current_page = -1;
    }
  }
}

// ---------------------------------------------------------------------------
// IEventListener::on_event overrides (Task 12 — fully typed event dispatch).
//
// Each override corresponds to one of the 41 event structs in events.h and
// contains its handler body directly (no int dispatch, no switch). The former
// per-responsibility (int,int) helpers and the legacy IMessageListener /
// DSV_MSG_* / broadcast_msg / trigger_message infrastructure have been
// removed. broadcast<T>() / broadcast_sync<T>() / broadcast_async<T>() are
// the only dispatch paths: broadcast<T>() is synchronous and is invoked from
// within the async-dispatched handler, so these overrides already run on
// qApp's thread (main thread) — no GUI-thread marshal is needed.
//
// Empty-body overrides:
//   * CaptureOwnerChanged — uses ev.new_owner directly (no int param race).
//   * CopyToDocDone / DecodeDone / SignalsChanged / DataUpdated /
//     DeviceConfigUpdated — these events have no GUI work to do in
//     MainWindow.
// ---------------------------------------------------------------------------

// --- Capture state group ---
void MainWindow::on_event(const pv::interface::CaptureStateChanged &) {
  update_capture_ui_status();
}
void MainWindow::on_event(const pv::interface::StartCollectWork &) {
  update_capture_ui_status();
  // CRITICAL FIX (fork 迁移遗漏): 旧版在 frame_began() 时设置 sidebar 按钮为
  // running 状态,但 frame_began() 只在收到第一个 logic 数据包时才被调用。
  // 等待触发时(无数据) frame_began() 不会被调用,sidebar 按钮保持 "Start",
  // 用户无法直观看到"正在采集中"的状态。在 StartCollectWork 事件中立即设置
  // sidebar 按钮为 running(Stop),让用户在采集开始的瞬间就看到状态变化。
  // setItemRunning 是幂等的,后续 frame_began() 会再次设置(无副作用)。
  if (_session->is_instant()) {
    _side_bar->setItemRunning(SIDEBAR_INSTANT, true);
  } else {
    _side_bar->setItemRunning(SIDEBAR_RUNSTOP, true);
  }
  current_view()->on_state_changed(false);
}
void MainWindow::on_event(const pv::interface::CollectStart &) {
  // 状态栏提示"采集中"
  statusBar()->showMessage(tr("采集中..."), 3000);
}
void MainWindow::on_event(const pv::interface::CollectEnd &) {
  prgRate(0);
  current_view()->repeat_unshow();
  current_view()->on_state_changed(true);
}
void MainWindow::on_event(const pv::interface::EndCollectWork &) {
  update_capture_ui_status();

  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document() && ctx->document()->has_pending_config()) {
    ctx->document()->apply_pending_config();
    // Task 2.6 (R2): apply_pending_config 触发 reload 重建 SignalModel，
    // 从 _signal_config 回写 trig_type 到新建的 SignalModel（参考
    // tabcontext.cpp:86-95）。
    for (const auto &ch : ctx->document()->get_signal_config().channels) {
      auto m = _session->get_signal_by_index(ch.index);
      if (m)
        m->set_trig_type(ch.trig_type);
    }
    _device_options_widget->update_view();
  }
  // R6: activate 在 working 时跳过了 set_active_document，工作结束后
  // 显式恢复当前 tab 的 active_document 归属。
  if (ctx) {
    _session->set_active_document(ctx->document());
  }
}
void MainWindow::on_event(const pv::interface::TrigNextCollect &) {
  // 状态栏提示"等待下一次采集"
  statusBar()->showMessage(tr("等待下一次采集..."), 3000);
}

// --- Device management group ---
void MainWindow::on_event(const pv::interface::DeviceListUpdated &) {
  _sampling_bar->update_device_list();
}
void MainWindow::on_event(const pv::interface::CurrentDeviceChanged &) {
  reset_all_view();
  load_device_config();
  update_title_bar_text();
  _sampling_bar->update_device_list();

  // After load_device_config() restored device settings (including
  // operation_mode / stream mode), reload the SamplingBar and DeviceOptions
  // panel so the stream mode button, sample count list, loop mode toggle,
  // and all device option controls reflect the persisted configuration.
  // Without this, the UI shows the auto-detected defaults (Buffer mode)
  // instead of the restored Stream mode.
  _sampling_bar->reload();
  _device_options_widget->update_view();

  _logo_bar->dsl_connected(_session->get_device()->is_hardware());
  update_toolbar_view_status();
  _session->device_event_object()->device_updated();

  // Save signal config for current tab and rebuild signals
  {
    pv::TabContext *ctx = current_context();
    if (ctx && ctx->document()) {
      ctx->document()->save_signal_config(
          _session->get_signal_models(),
          build_channel_layout(current_view()));
      current_view()->rebuild_signals();
      pxv_info("CurrentDeviceChanged: saved config and rebuilt "
               "signals for current tab");
    }
  }

  if (_device_agent->is_hardware()) {
    _session->on_load_config_end();
  }

  if (_device_agent->get_work_mode() == LOGIC &&
      _device_agent->is_file() == false)
    current_view()->auto_set_max_scale();

  if (_device_agent->is_file()) {
    check_config_file_version();

    bool bDoneDecoder = false;
    bool bLoadSuccess = false;
    QJsonDocument doc =
        get_config_json_from_data_file(_device_agent->path(), bLoadSuccess);

    if (bLoadSuccess) {
      load_config_from_json(doc, bDoneDecoder);
    }

    if (!bDoneDecoder && _device_agent->get_work_mode() == LOGIC) {
      QJsonArray deArray = get_decoder_json_from_data_file(
          _device_agent->path(), bLoadSuccess);

      if (bLoadSuccess) {
        StoreSession ss(_session);
        ss.load_decoders(_protocol_widget, deArray);
      }
    }

    current_view()->update_all_trace_postion();
    QTimer::singleShot(100, this,
                       [this]() { _session->start_capture(true); });
  } else if (_device_agent->is_demo()) {
    if (_device_agent->get_work_mode() == LOGIC) {
      _pattern_mode = _device_agent->get_demo_operation_mode();
      _protocol_widget->del_all_protocol();
      current_view()->auto_set_max_scale();

      if (_pattern_mode != "random") {
        load_demo_decoder_config(_pattern_mode);
      }
    }
  }

  calc_min_height();

  if (_device_agent->is_hardware() && _device_agent->is_new_device()) {
    check_usb_device_speed();
  }
}
void MainWindow::on_event(const pv::interface::UsbDeviceArrived &) {
  if (_msg != nullptr) {
    _msg->close();
    _msg = nullptr;
  }

  _sampling_bar->update_device_list();

  // If the current device is working, do not remind to switch new device.
  if (_session->get_device()->is_hardware() && _session->is_working()) {
    return;
  }

  // If a saving task is running, not need to remind to switch device,
  // when the task end, the new device will be selected.
  if (_session->get_device()->is_demo() == false && !_is_save_confirm_msg) {
    QString msgText = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_TO_SWITCH_DEVICE),
                          "To switch the new device?");

    if (MsgBox::Confirm(msgText, "", &_msg, nullptr) == false) {
      _msg = nullptr;
      return;
    }
    _msg = nullptr;
  }

  // The store confirm is not processed.
  if (_is_save_confirm_msg) {
    pxv_info("New device attached:Waitting for the confirm box be closed.");
    _is_auto_switch_device = true;
    return;
  }

  if (_session->is_saving()) {
    pxv_info("New device attached:Waitting for store the data. and will "
             "switch to new device.");
    _is_auto_switch_device = true;
    return;
  }

  int mode = _device_agent->get_work_mode();

  if (mode != DSO && confirm_to_store_data()) {
    _is_auto_switch_device = true;

    if (_session->is_working())
      _session->stop_capture();

    on_save();
  } else {
    if (_session->is_working())
      _session->stop_capture();

    _session->set_default_device();
  }
}
void MainWindow::on_event(const pv::interface::DeviceDetached &) {
  if (_msg != nullptr) {
    _msg->close();
    _msg = nullptr;
  }

  // Save current config, and switch to the last device.
  _session->device_event_object()->device_updated();
  save_config();
  // Calibration dialog removed; nothing to hide.

  if (_session->is_saving()) {
    pxv_info("Device detached:Waitting for store the data. and will switch "
             "to new device.");
    _is_auto_switch_device = true;
    return;
  }

  if (confirm_to_store_data()) {
    _is_auto_switch_device = true;
    on_save();
  } else {
    _session->set_default_device();
  }
}

void MainWindow::on_event(const pv::interface::DeviceOpenFailed &evt) {
  // set_device() failed to open the new device via sr_dev_open. The old device
  // was already released, so the UI is now blank. Show a user-facing message
  // with the driver name and error reason so the user knows the device failed
  // to open (e.g. USB interface claimed by another driver, firmware version
  // mismatch, libusb permission issue) instead of staring at an empty window.
  QString driver = QString::fromStdString(evt.driver_name);
  QString err = QString::fromStdString(evt.error_message);
  QString title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DEVICE_OPEN_FAILED),
                       "Failed to open device");
  QString text;
  if (err.isEmpty()) {
    text = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DEVICE_OPEN_FAILED_REASON),
               "The device could not be opened. Check USB connection, "
               "driver, and that no other program is using it.");
  } else {
    text = err;
  }
  if (!driver.isEmpty()) {
    text = QString("[%1] %2").arg(driver, text);
  }
  pxv_err("DeviceOpenFailed: driver=%s reason=%s",
          driver.toUtf8().constData(), err.toUtf8().constData());
  MsgBox::Show(title, text, this);
}

// --- Device options group ---
void MainWindow::on_event(const pv::interface::DeviceOptionsUpdated &) {
  _trigger_widget->device_updated();
  _device_options_widget->device_updated();
  _measure_widget->reload();
  // Calibration dialog check removed (SR_CONF_CALI key deleted).

  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document()) {
    ctx->document()->save_signal_config(
        _session->get_signal_models(), build_channel_layout(current_view()));
  }

  current_view()->rebuild_signals();
  current_view()->signals_changed(nullptr);
}
void MainWindow::on_event(const pv::interface::DsoViewOptionChanged &) {
  // DSO header interaction (vDial/factor/acCoupling). The DsoSignal setters
  // already synced driver + Core model + View state; we only need to refresh
  // dock panels and persist config. reload()/rebuild_signals() are explicitly
  // avoided here because they drop View-only state (_stop_scale resets to 1
  // in path-B full rebuild → waveform no longer scales with vdiv).
  _trigger_widget->device_updated();
  _device_options_widget->device_updated();
  _measure_widget->reload();

  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document()) {
    ctx->document()->save_signal_config(
        _session->get_signal_models(), build_channel_layout(current_view()));
  }
}
void MainWindow::on_event(const pv::interface::SampleRateChanged &) {
  _trigger_widget->device_updated();
  current_view()->timebase_changed();
}
void MainWindow::on_event(const pv::interface::SampleCountUpdated &) {
  _sampling_bar->update_sample_count_selector();
}
void MainWindow::on_event(const pv::interface::DeviceModeChanged &) {
  // switch_work_mode() broadcasts DeviceModeChanged via broadcast_async,
  // which is queued on qApp via Qt::QueuedConnection by EventBus, so this
  // handler runs AFTER the View has finished its signals_changed rebuild
  // (which rebinds view::Signal::_model to the new SignalModels via
  // compute_change_event pointer-identity check). No manual
  // rebuild_signals() is needed here.
  current_view()->mode_changed();
  reset_all_view();
  load_device_config();
  update_title_bar_text();
  // Lightweight refresh of DeviceOptionsDock: only rebuild the dynamic panel
  // (channel area) and Mode section, preserving scaffolding (separators,
  // minWid, stretch, sampling widget). Avoids the full nuke-and-rebuild of
  // update_view() which caused UI jumping on mode switch.
  _device_options_widget->on_mode_changed();
  // Calibration dialog removed; nothing to hide.

  update_toolbar_view_status();
  _sampling_bar->update_sample_rate_list();
  _sampling_bar->reload();

  // Save signal config for current tab and rebuild signals
  {
    pv::TabContext *ctx = current_context();
    if (ctx && ctx->document()) {
      ctx->document()->save_signal_config(
          _session->get_signal_models(),
          build_channel_layout(current_view()));
      current_view()->rebuild_signals();
      pxv_info("DeviceModeChanged: saved config and rebuilt "
               "signals for current tab");
    }
  }

  if (_device_agent->is_hardware())
    _session->on_load_config_end();

  if (_device_agent->get_work_mode() == LOGIC)
    current_view()->auto_set_max_scale();

  if (_device_agent->is_demo()) {
    _pattern_mode = _device_agent->get_demo_operation_mode();
    _protocol_widget->del_all_protocol();

    if (_device_agent->get_work_mode() == LOGIC) {
      if (_pattern_mode != "random") {
        _device_agent->update();
        load_demo_decoder_config(_pattern_mode);
      }
    }
  }

  calc_min_height();
}
void MainWindow::on_event(const pv::interface::CollectModeChanged &) {
  if (_device_agent->is_demo()) {
    _pattern_mode = _device_agent->get_demo_operation_mode();
  }
  _trigger_widget->device_updated();
  current_view()->update();
}
void MainWindow::on_event(const pv::interface::EndDeviceOptions &) {
  if (_device_agent->is_demo() && _device_agent->get_work_mode() == LOGIC) {
    QString pattern_mode = _device_agent->get_demo_operation_mode();

    if (pattern_mode != _pattern_mode) {
      _pattern_mode = pattern_mode;

      _device_agent->update();
      _session->clear_view_data();
      _session->init_signals();
      update_toolbar_view_status();
      _sampling_bar->update_sample_rate_list();
      _protocol_widget->del_all_protocol();

      if (_pattern_mode != "random") {
        _session->set_collect_mode(COLLECT_SINGLE);
        load_demo_decoder_config(_pattern_mode);

        _session->start_capture(false); // Auto load data.
      }
    }
  }
  calc_min_height();
}
void MainWindow::on_event(const pv::interface::DemoModeChanged &) {
  if (_device_agent->is_demo() && _device_agent->get_work_mode() == LOGIC) {
    QString pattern_mode = _device_agent->get_demo_operation_mode();

    if (pattern_mode != _pattern_mode) {
      _pattern_mode = pattern_mode;

      _device_agent->update();
      _session->clear_view_data();
      _session->init_signals();
      update_toolbar_view_status();
      _sampling_bar->update_sample_rate_list();
      _protocol_widget->del_all_protocol();

      if (_pattern_mode != "random") {
        _session->set_collect_mode(COLLECT_SINGLE);
        load_demo_decoder_config(_pattern_mode);
      }
    }
  }
  calc_min_height();
}

// --- UI options group ---
void MainWindow::on_event(const pv::interface::AppOptionsChanged &) {
  update_title_bar_text();
}
void MainWindow::on_event(const pv::interface::FontOptionsChanged &) {
  UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);
}
void MainWindow::on_event(const pv::interface::ShortcutChanged &) {
}
void MainWindow::on_event(const pv::interface::StyleChanged &) {
  UiManager::Instance()->Update(UI_UPDATE_ACTION_THEME);
  for (QWidget *w : qApp->topLevelWidgets()) {
    w->update();
  }
}

// --- Data group ---
void MainWindow::on_event(const pv::interface::DataPoolChanged &) {
  current_view()->check_measure();
}
void MainWindow::on_event(const pv::interface::CopyInProgressChanged &) {
  // 显示后台 copy 指示器；完成后由其它消息刷新
  if (_disk_cache_status_label)
    _disk_cache_status_label->setText(tr("后台数据拷贝中..."));
}
void MainWindow::on_event(const pv::interface::ActiveDocumentChanged &) {
  // 活动文档已切换，更新标题栏与 dock 状态
  update_title_bar_text();
}
void MainWindow::on_event(const pv::interface::SaveComplete &) {
  _session->clear_store_confirm_flag();

  if (_is_auto_switch_device) {
    _is_auto_switch_device = false;
    _session->set_default_device();
  } else {
    ds_device_handle devh = _sampling_bar->get_next_device_handle();
    if (devh != NULL_HANDLE) {
      pxv_info("Auto switch to the selected device.");
      _session->set_device(devh);
    }
  }
}
void MainWindow::on_event(const pv::interface::ClearDecodeData &) {
  if (_device_agent->get_work_mode() == LOGIC)
    _protocol_widget->reset_view();
}

// --- Filter / invert group ---
void MainWindow::on_event(const pv::interface::GlitchFilterStarted &) {
  // 复用磁盘缓存状态标签显示毛刺滤波处理中指示
  if (_disk_cache_status_label)
    _disk_cache_status_label->setText(tr("毛刺滤波处理中..."));
}
void MainWindow::on_event(const pv::interface::GlitchFilterProgress &e) {
  // FilterProcessor emits broadcast_async<GlitchFilterProgress> carrying
  // the 0-100 progress percent. The typed event is dispatched to all
  // IEventListener consumers on the main thread.
  int p = e.progress;
  if (p < 0)
    p = 0;
  if (p > 100)
    p = 100;
  statusBar()->showMessage(
      tr("毛刺滤波进行中... %1%").arg(p), 2000);
}
void MainWindow::on_event(const pv::interface::GlitchFilterCompleted &) {
  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document()) {
    _session->copy_data_to_document(ctx->document());
  }
  // Restart decoders after data change
  _session->restart_decoders();

  // 若 GlitchFilterPopup 已打开,刷新其直方图与默认值(底层
  // LogicSnapshot 数据已变化,直方图应反映滤波后的脉冲分布)。
  if (auto *v = current_view()) {
    v->on_glitch_filter_completed();
  }
}
void MainWindow::on_event(const pv::interface::GlitchFilterCleared &) {
  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document()) {
    _session->copy_data_to_document(ctx->document());
  }
  // Restart decoders after data change
  _session->restart_decoders();

  // 若 GlitchFilterPopup 已打开,刷新其直方图与默认值(底层
  // LogicSnapshot 数据已变化,直方图应反映滤波后的脉冲分布)。
  if (auto *v = current_view()) {
    v->on_glitch_filter_cleared();
  }
}
void MainWindow::on_event(const pv::interface::SignalInvertStarted &) {
  if (_disk_cache_status_label)
    _disk_cache_status_label->setText(tr("信号反相处理中..."));
}
void MainWindow::on_event(const pv::interface::SignalInvertCompleted &) {
  pv::TabContext *ctx2 = current_context();
  if (ctx2 && ctx2->document()) {
    _session->copy_data_to_document(ctx2->document());
  }
  // Restart decoders after data change
  _session->restart_decoders();
}
void MainWindow::on_event(const pv::interface::SignalInvertCleared &) {
  pv::TabContext *ctx2 = current_context();
  if (ctx2 && ctx2->document()) {
    _session->copy_data_to_document(ctx2->document());
  }
  // Restart decoders after data change
  _session->restart_decoders();
}

// --- Trigger group ---
void MainWindow::on_event(const pv::interface::SimpleTriggerChanged &) {
  if (_trigger_widget) {
    _trigger_widget->select_simple_trigger();
  }
}
void MainWindow::on_event(const pv::interface::TriggerConfigChanged &) {
  // Task 8.8: TriggerConfig 变化，刷新 TriggerDock UI。
  if (_trigger_widget)
    _trigger_widget->update_view();
}

// --- Empty-body / pre-broadcast overrides ---
void MainWindow::on_event(const pv::interface::CaptureOwnerChanged &) {
  // Capture owner 改变。原本在此处调用了 activate() 导致采集结束瞬间
  // 误触 reload()，从而把后台刚刚启动的离线解码任务强制杀掉。
  // 现在将其移除，仅在必要时（如 Tab 切换）才去调 activate()。
}
void MainWindow::on_event(const pv::interface::CopyToDocDone &) {
  // After background copy_data_to_document completes, rebind signal data
  // from session to document so waveforms use the document's own data copy.
  pv::TabContext *ctx = current_context();
  if (ctx && ctx->document() && ctx->document()->has_data()) {
    current_view()->set_data_document(ctx->document());
  }
}
void MainWindow::on_event(const pv::interface::DecodeDone &) {
  // 离线解码完成（或所有解码任务结束）时，主动通知 View 刷新界面，
  // 将刚刚生成的 Annotation 渲染出来，并更新右侧协议列表。
  on_data_updated();
  if (current_view()) {
    current_view()->update();
    current_view()->viewport_update();
  }
  on_decode_done();
}
void MainWindow::on_event(const pv::interface::SignalsChanged &) {}
void MainWindow::on_event(const pv::interface::DataUpdated &) {
  // modernize-core-layer-radical Task 13: DataUpdated is now emitted by
  // DataFeedParser::feed_in_* via broadcast_async. Route to the existing
  // on_data_updated() handler (measure reCalc + view data_updated).
  on_data_updated();
}
void MainWindow::on_event(const pv::interface::DeviceConfigUpdated &) {}

void MainWindow::on_event(const pv::interface::StoreConfPrev &) {
  // modernize-core-layer-radical Task 10: StoreConfPrev pre-broadcast hook.
  // Commit sampling-bar settings before the config store mutation lands,
  // but only for hardware devices without captured data.
  if (_device_agent && _device_agent->is_hardware() &&
      _session && !_session->have_hardware_data()) {
    _sampling_bar->commit_settings();
  }
}

void MainWindow::on_event(const pv::interface::CurrentDeviceChangePrev &) {
  // modernize-core-layer-radical Task 11: CurrentDeviceChangePrev pre-broadcast
  // hook. Close any modal message, hide calibration, delete all protocols,
  // reload the view BEFORE SigSession releases the old device.
  if (_msg != nullptr) {
    _msg->close();
    _msg = nullptr;
  }
  // Calibration dialog removed; nothing to hide.

  _protocol_widget->del_all_protocol();
  current_view()->reload();
}

void MainWindow::on_event(const pv::interface::StartCollectWorkPrev &) {
  // modernize-core-layer-radical Task 11: StartCollectWorkPrev pre-broadcast
  // hook. Commit trigger settings + capture_init + on_state_changed(false)
  // BEFORE CaptureManager::exec_capture() starts the device.
  if (_device_agent->get_work_mode() == LOGIC)
    _trigger_widget->try_commit_trigger();
  else if (_device_agent->get_work_mode() == DSO)
    _dso_trigger_widget->check_setting();

  current_view()->capture_init();
  current_view()->on_state_changed(false);
}

void MainWindow::on_event(const pv::interface::EndCollectWorkPrev &) {
  // modernize-core-layer-radical Task 11: EndCollectWorkPrev is a no-op in
  // GUI mode; SessionService is the sole consumer. Empty override satisfies
  // the IEventListener virtual dispatch.
}

// ---------------------------------------------------------------------------
// IServiceEventListener — route View operation broadcasts from SessionService
// (MCP/WS API) to the active View. In Headless mode there is no MainWindow,
// so these events are simply not consumed.
// ---------------------------------------------------------------------------
void MainWindow::on_service_event(const pv::api::ServiceEventData &data) {
  pv::view::View *view = current_view();
  if (!view)
    return;

  const auto &params = data.params;

  switch (data.event) {
  case pv::api::ServiceEvent::ViewShowRegion: {
    auto it_start = params.find("start");
    auto it_end = params.find("end");
    if (it_start != params.end() && it_end != params.end()) {
      uint64_t start = std::stoull(it_start->second);
      uint64_t end = std::stoull(it_end->second);
      view->show_region(start, end, true);
    }
    break;
  }
  case pv::api::ServiceEvent::ViewZoomFit: {
    // TODO: View has no zoom_fit() method yet; approximate with zoom out.
    // A proper fit-to-screen implementation should be added to View.
    view->zoom(-1.0);
    break;
  }
  case pv::api::ServiceEvent::ViewZoomIn: {
    view->zoom(1.0);
    break;
  }
  case pv::api::ServiceEvent::ViewZoomOut: {
    view->zoom(-1.0);
    break;
  }
  case pv::api::ServiceEvent::ViewCursorAdded: {
    auto it = params.find("sample_pos");
    if (it != params.end()) {
      uint64_t sample_pos = std::stoull(it->second);
      view->add_cursor(sample_pos);
    }
    break;
  }
  case pv::api::ServiceEvent::ViewCursorRemoved: {
    // Cursor removal by index is handled by View internally;
    // no direct public API to remove by index from outside.
    // TODO: Add View::remove_cursor(int index) if needed.
    break;
  }
  case pv::api::ServiceEvent::ViewCursorsCleared: {
    view->clear_cursors();
    break;
  }
  case pv::api::ServiceEvent::DecoderAdded:
  case pv::api::ServiceEvent::DecoderRemoved:
  case pv::api::ServiceEvent::SignalsChanged: {
    // Core data changed via MCP/API (decoder added/removed or signals
    // changed). Trigger lazy sync so View creates/removes the
    // corresponding DecodeTrace by Core Stack identity comparison.
    // signals_changed(nullptr) internally calls mark_derived_traces_dirty()
    // then get_traces() -> get_own_decode_traces() -> sync_derived_traces(),
    // which performs the Stack-pointer-identity-based reconciliation.
    // The explicit mark_derived_traces_dirty() is kept for clarity and
    // defensive purposes (idempotent).
    view->mark_derived_traces_dirty();
    view->signals_changed(nullptr);
    break;
  }
  default:
    // Not a View event; ignore.
    break;
  }
}

void MainWindow::calc_min_height() {
  if (_frame != nullptr) {
    if (_device_agent->get_work_mode() == LOGIC) {
      int ch_num = _session->get_ch_num(-1);
      int win_height = Base_Height + Per_Chan_Height * ch_num;

      if (win_height < Min_Height)
        _frame->setMinimumHeight(win_height);
      else
        _frame->setMinimumHeight(Min_Height);
    } else {
      _frame->setMinimumHeight(Min_Height);
    }
  }
}

void MainWindow::delay_prop_msg(QString strMsg) {
  _strMsg = strMsg;
  if (_strMsg != "") {
    _delay_prop_msg_timer.Start(500);
  }
}

void MainWindow::on_delay_prop_msg() {
  _delay_prop_msg_timer.Stop();

  if (_strMsg != "") {
    MsgBox::Show("", _strMsg, this, &_msg);
    _msg = nullptr;
  }
}

void MainWindow::update_title_bar_text() {
  // Set the title
  QString title = QApplication::applicationName() + " v" +
                  QApplication::applicationVersion();
  AppConfig &app = AppConfig::Instance();

  if (_title_ext_string != "" && app.appOptions.displayProfileInBar) {
    title += " [" + _title_ext_string + "]";
  }

  if (_lst_title_string != title) {
    _lst_title_string = title;

    setWindowTitle(
        QApplication::translate("MainWindow", title.toLocal8Bit().data(), 0));
    _title_bar->setTitle(this->windowTitle());
  }
}

void MainWindow::load_demo_decoder_config(QString optname) {
  QString file = GetAppDataDir() + "/demo/logic/" + optname + ".demo";
  bool bLoadSurccess = false;

  QJsonArray deArray = get_decoder_json_from_data_file(file, bLoadSurccess);

  if (bLoadSurccess) {
    StoreSession ss(_session);
    ss.load_decoders(_protocol_widget, deArray);
  }

  current_view()->update_all_trace_postion();
}

QWidget *MainWindow::GetBodyView() { return current_view(); }

pv::view::View *MainWindow::current_view() {
  if (_current_tab_index >= 0 && _current_tab_index < _tab_contexts.size()) {
    return _tab_contexts[_current_tab_index]->view();
  }
  return nullptr;
}

pv::TabContext *MainWindow::current_context() {
  if (_current_tab_index >= 0 && _current_tab_index < _tab_contexts.size()) {
    return _tab_contexts[_current_tab_index];
  }
  return nullptr;
}

void MainWindow::add_tab(pv::TabContext *ctx) {
  pv::view::View *view = ctx->view();
  _tab_contexts.append(ctx);
  _tab_widget->addTab(view, ctx->title());
  _tab_widget->setCurrentIndex(_tab_widget->count() - 1);
  update_tab_style(_tab_widget->count() - 1);
}

void MainWindow::remove_tab(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  if (_tab_contexts.size() <= 1)
    return;

  pv::TabContext *ctx = _tab_contexts[index];
  if (ctx->is_live() && _session->is_working()) {
    _session->stop_capture();
  }

  if (_session->get_active_document() == ctx->document()) {
    _session->set_active_document(nullptr);
  }

  _tab_contexts.removeAt(index);
  disconnect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, this,
             &MainWindow::on_tab_changed);
  _tab_widget->removeTab(index);
  // Task 4.3: capture owner cleanup is now RAII-managed by CaptureOwnerGuard.
  // clear_capture_owner_document() resets the guard, whose destructor joins the
  // copy thread + clears owner + broadcasts. No need for manual join_copy_thread.
  _session->clear_capture_owner_document(ctx->document());

  // A2 fix: stop decoder threads working on this document's stacks before the
  // document is destroyed. Without this, a running decode thread would access
  // freed DecoderStack memory. We stop each stack individually rather than
  // calling clear_all_documents_decoders() (which would stop ALL tabs' decoders).
  auto doc = ctx->document();
  if (doc) {
    for (auto &stack : doc->get_decoder_stacks()) {
      if (stack && stack->IsRunning()) {
        stack->stop_decode_work();
      }
    }
  }

  // phase 2: unregister_document() removed — document ownership is now held by
  // DocumentRegistry. The document is released (marked deletion) inside
  // TabContext::~TabContext (called by destroy_context below) via
  // registry->release_document(doc_index). No explicit release here.

  // A2 fix: detach View→Document pointer BEFORE deleteLater(). deleteLater is
  // async — the View may receive paint events before actual deletion. Without
  // this detach, those paint events would dereference the soon-to-be-destroyed
  // document pointer (use-after-free).
  ctx->view()->set_data_document(nullptr);

  ctx->view()->deleteLater();
  SessionManager::instance()->destroy_context(ctx);

  if (_current_tab_index >= _tab_contexts.size()) {
    _current_tab_index = _tab_contexts.size() - 1;
  } else if (index < _current_tab_index) {
    _current_tab_index--;
  }

  _tab_contexts[_current_tab_index]->activate();
  _tab_widget->setCurrentIndex(_current_tab_index);
  update_tab_style(_current_tab_index);

  pv::TabContext *new_ctx = _tab_contexts[_current_tab_index];
  _sampling_bar->bind_context(new_ctx);
  _measure_widget->bind_context(new_ctx);
  _search_widget->bind_context(new_ctx);
  _protocol_widget->bind_context(new_ctx);
  _device_options_widget->bind_context(new_ctx);
  _log_widget->bind_context(new_ctx);
  _trigger_widget->bind_context(new_ctx);
  _dso_trigger_widget->bind_context(new_ctx);

  pv::view::View *view = current_view();
  if (view) {
    _sampling_bar->set_context(_session, view);
    _sampling_bar->set_readonly(false);
    _sampling_bar->set_view(view);
    _measure_widget->set_view(view);
    _search_widget->set_view(view);
    _protocol_widget->set_view(view);
    view->installEventFilter(this);
  }

  connect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, this,
          &MainWindow::on_tab_changed);
}

void MainWindow::update_tab_style(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  pv::TabContext *ctx = _tab_contexts[index];
  _tab_widget->setTabText(index, ctx->title());
}

void MainWindow::on_tab_changed(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  int old_index = _current_tab_index;
  pxv_info("MainWindow::on_tab_changed(%d) old=%d", index, old_index);

  if (old_index >= 0 && old_index < _tab_contexts.size() &&
      old_index != index) {
    _tab_contexts[old_index]->deactivate();
    update_tab_style(old_index);
  }

  _current_tab_index = index;
  _tab_contexts[index]->activate();
  update_tab_style(index);

  pv::view::View *view = current_view();
  update_sample_period();
  if (view) {
    if (old_index >= 0 && old_index < _tab_contexts.size() &&
        old_index != index) {
      _sampling_bar->unbind_context();
      _measure_widget->unbind_context();
      _search_widget->unbind_context();
      _protocol_widget->unbind_context();
      _device_options_widget->unbind_context();
      _log_widget->unbind_context();
      _trigger_widget->unbind_context();
      _dso_trigger_widget->unbind_context();
    }

    pv::TabContext *new_ctx = _tab_contexts[index];
    _sampling_bar->bind_context(new_ctx);
    _measure_widget->bind_context(new_ctx);
    _search_widget->bind_context(new_ctx);
    _protocol_widget->bind_context(new_ctx);
    _device_options_widget->bind_context(new_ctx);
    _log_widget->bind_context(new_ctx);
    _trigger_widget->bind_context(new_ctx);
    _dso_trigger_widget->bind_context(new_ctx);

    view->installEventFilter(this);
  }

  update_title_bar_text();
  SessionManager::instance()->set_active_context(_tab_contexts[index]);
}

void MainWindow::on_tab_moved(int from, int to) {
  if (from < 0 || from >= _tab_contexts.size() || to < 0 ||
      to >= _tab_contexts.size())
    return;
  if (from == to)
    return;

  pv::TabContext *ctx = _tab_contexts[from];
  _tab_contexts.removeAt(from);
  _tab_contexts.insert(to, ctx);

  if (_current_tab_index == from) {
    _current_tab_index = to;
  } else if (from < _current_tab_index && to >= _current_tab_index) {
    _current_tab_index--;
  } else if (from > _current_tab_index && to <= _current_tab_index) {
    _current_tab_index++;
  }
}

void MainWindow::on_tab_detach(int index, QWidget *widget,
                               const QString &title) {
  (void)index;
  (void)title;

  pv::TabContext *ctx = nullptr;
  for (auto c : _tab_contexts) {
    if (c->view() == widget) {
      ctx = c;
      break;
    }
  }

  if (ctx) {
    if (ctx->is_live()) {
      ctx->deactivate();
    }
    _tab_contexts.removeOne(ctx);
    if (_current_tab_index >= _tab_contexts.size()) {
      _current_tab_index = _tab_contexts.size() - 1;
    }
    if (!_tab_contexts.isEmpty()) {
      _tab_contexts[_current_tab_index]->activate();
      update_tab_style(_current_tab_index);
    }
    SessionManager::instance()->detach_context(ctx);
    ctx->view()->setProperty("detached_ctx",
                             QVariant::fromValue((quintptr)ctx));
  }
}

void MainWindow::on_tab_attached(QWidget *widget, const QString &title) {
  (void)title;
  QVariant prop = widget->property("detached_ctx");
  if (!prop.isValid() || prop.isNull())
    return;

  pv::TabContext *ctx =
      reinterpret_cast<pv::TabContext *>(prop.value<quintptr>());
  if (!ctx)
    return;

  _tab_contexts.append(ctx);
  SessionManager::instance()->attach_context(ctx);
  widget->setProperty("detached_ctx", QVariant());
}

void MainWindow::on_new_tab_requested() {
  pv::view::View *new_view = new pv::view::View(_session, _sampling_bar, this);
  // phase 2: document owned by DocumentRegistry.
  size_t new_doc_idx = _session->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_session));
  pv::data::SessionDocument *new_doc =
      _session->document_registry()->get_document_by_index(new_doc_idx);

  if (_device_agent && _device_agent->have_instance()) {
    new_doc->save_signal_config(_session->get_signal_models(), {});
    pxv_info("MainWindow::on_new_tab_requested() saved signal config, mode=%d "
             "ch_count=%d",
             new_doc->get_signal_config().work_mode,
             (int)new_doc->get_signal_config().channels.size());
  }

  pv::TabContext *new_ctx =
      SessionManager::instance()->create_context(new_view, _session, new_doc,
                                                 new_doc_idx,
                                                 _session->document_registry());
  new_ctx->set_title(
      QString::fromUtf8(L_S(STR_PAGE_MSG, S_ID(IDS_TAB_TITLE), "Tab %1"))
          .arg(_tab_contexts.size() + 1));
  add_tab(new_ctx);
}

void MainWindow::update_disk_cache_status() {
  update_sample_period();
  if (!_device_agent || !_device_agent->have_instance()) {
    if (_disk_cache_status_label)
      _disk_cache_status_label->hide();
    _trig_time_label->hide();
    return;
  }

  QDateTime trig_time = _session->get_trig_time();
  if (_session->is_triged() && trig_time.isValid()) {
    _trig_time_label->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_TIME), "Trigger Time: ") +
        trig_time.toString("yyyy-MM-dd hh:mm:ss"));
    _trig_time_label->show();
  } else {
    _trig_time_label->hide();
  }

  bool cache_enabled = false;
  // DISK_CACHE_ENABLE is a PXLogic fork key — only DSL/PXLogic devices
  // implement it. demo/file/compat devices would otherwise log "Option not
  // available" every 500ms via _disk_cache_status_timer.
  if (_device_agent->is_dsl_device())
    _device_agent->get_config_bool(SR_CONF_DISK_CACHE_ENABLE, cache_enabled);

  if (!cache_enabled) {
    _disk_cache_status_label->hide();
    return;
  }

  QString cache_path;
  _device_agent->get_config_string(SR_CONF_DISK_CACHE_PATH, cache_path);
  if (cache_path.isEmpty()) {
    cache_path = QDir::tempPath() + "/PXView_cache";
  }
  QString text = QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_ON),
                             "Disk Cache: ON")) +
                 " | " +
                 QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_PATH_LABEL),
                             "Path: ")) +
                 cache_path;

  double wspeed = _session->get_disk_write_speed_mbps();
  size_t qdepth = _session->get_disk_write_queue_depth();

  data::LogicSnapshot *logic = _session->get_logic_snapshot();
  uint64_t pf = 0;
  uint64_t ws = 0;
  uint64_t qb = 0;

  if (logic) {
    pf = logic->get_page_fault_count();
    ws = logic->get_working_set_bytes();
    qb = logic->get_async_queue_bytes();
  }

  if (!_session->is_working()) {
    wspeed = 0.0;
  }

  text +=
      " | " +
      QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_WRITE), "Write: ")) +
      QString("%1 MB/s").arg(wspeed, 0, 'f', 1);

  if (logic) {
    text +=
        " | " +
        QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_QUEUE), "Queue: ")) +
        QString("%1 MB (%2 blks)")
            .arg(qb / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(qdepth);
    text += " | " +
            QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_RAM), "RAM: ")) +
            QString("%1 MB").arg(ws / (1024.0 * 1024.0), 0, 'f', 1);
    text += " | " +
            QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_PF), "PF/s: ")) +
            QString("%1").arg(pf);

    if (logic->is_disk_cache_active()) {
      uint64_t total_blocks = logic->get_disk_total_blocks_written();
      double disk_gb = total_blocks * 2105376 / (1024.0 * 1024.0 * 1024.0);
      text +=
          " | " +
          QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_DISK), "Disk: ")) +
          QString("%1 GB").arg(disk_gb, 0, 'f', 2);
    }
  }

  if (_session->is_disk_write_disk_full()) {
    text += " | " + QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_FULL),
                                "DISK FULL"));
    _disk_cache_status_label->setStyleSheet("color: red; font-weight: bold;");
  } else if (qdepth > 256) {
    _disk_cache_status_label->setStyleSheet("color: red; font-weight: bold;");
  } else if (qdepth > 64) {
    _disk_cache_status_label->setStyleSheet(
        "color: yellow; font-weight: bold;");
  } else {
    _disk_cache_status_label->setStyleSheet("");
  }

  _disk_cache_status_label->setText(text);
  _disk_cache_status_label->show();
}

void MainWindow::update_fps() {
  int ui_fps = 0;
  pv::view::View *cur_view = current_view();
  if (cur_view && cur_view->get_time_view()) {
    ui_fps = cur_view->get_time_view()->get_fps();
  }

  int dock_fps = 0;
  if (_sliding_drawer) {
    dock_fps = _sliding_drawer->get_fps();
  }

  _acq_count = 0;

  if (_fps_label) {
    QString fps_text =
        QString("UI: %1ms | Dock: %2ms").arg(ui_fps).arg(dock_fps);
    _fps_label->setText(fps_text);
    _fps_label->show();
  }
}

void MainWindow::update_sample_period() {
  if (!_sample_period_label)
    return;

  pv::TabContext *ctx = current_context();
  if (!ctx || !ctx->document()) {
    _sample_period_label->setText(
        (AppConfig::Instance().frameOptions.language == LAN_CN)
            ? "采样周期: --"
            : "Sample Period: --");
    return;
  }

  uint64_t samplerate = ctx->document()->get_samplerate();
  if (samplerate == 0) {
    _sample_period_label->setText(
        (AppConfig::Instance().frameOptions.language == LAN_CN)
            ? "采样周期: --"
            : "Sample Period: --");
    return;
  }

  double period = 1.0 / samplerate;
  QString unit = "s";
  double val = period;
  if (period < 1.0) {
    if (period >= 1e-3) {
      val = period * 1e3;
      unit = "ms";
    } else if (period >= 1e-6) {
      val = period * 1e6;
      unit = "us";
    } else if (period >= 1e-9) {
      val = period * 1e9;
      unit = "ns";
    } else if (period >= 1e-12) {
      val = period * 1e12;
      unit = "ps";
    } else {
      val = period * 1e15;
      unit = "fs";
    }
  }

  QString val_str = QString::number(val, 'f', 4);
  if (val_str.contains('.')) {
    while (val_str.endsWith('0')) {
      val_str.chop(1);
    }
    if (val_str.endsWith('.')) {
      val_str.chop(1);
    }
  }

  QString prefix = (AppConfig::Instance().frameOptions.language == LAN_CN)
                       ? "采样周期: "
                       : "Sample Period: ";
  _sample_period_label->setText(prefix + val_str + " " + unit);
}

} // namespace pv
