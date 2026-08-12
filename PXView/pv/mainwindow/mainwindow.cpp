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

#include "pv/widgets/searchpatterninput.h"
#include "pv/widgets/sidebar.h"
#include "pv/widgets/smoothscrollarea.h"
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

#include "pv/base/log.h"
#include "pv/mainwindow/mainwindow.h"
#include "pv/mainwindow/config_io.h"
#include "pv/mainwindow/dock_manager.h"
#include "pv/mainwindow/event_dispatcher.h"
#include "pv/mainwindow/file_ops.h"
#include "pv/mainwindow/signal_connector.h"
#include "pv/mainwindow/tab_manager.h"
#include "pv/mainwindow/theme_manager.h"
#include "pv/mainwindow/status_bar.h"
#include "pv/mainwindow/shortcut_manager.h"

#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/logicsnapshot.h"

#include "pv/dialogs/about.h"
#include "pv/dialogs/deviceoptions.h"
#include "pv/dialogs/regionoptions.h"
#include "pv/dialogs/storeprogress.h"

#include "pv/toolbars/filebar.h"
#include "pv/toolbars/logobar.h"
#include "pv/toolbars/samplingbar.h"
#include "pv/toolbars/titlebar.h"
#include "pv/toolbars/trigbar.h"

#include "pv/dock/deviceoptionsdock.h"
#include "pv/dock/logdock.h"
#include "pv/dock/mcpcontroldock.h"
#include "pv/dock/functiondock.h"
#include "pv/dock/measuredock.h"
#include "pv/dock/protocoldock.h"
#include "pv/dock/searchdock.h"
#include "pv/dock/dsotriggerdock.h"
#include "pv/dock/triggerdock.h"


#include "pv/data/stack/decoderstack.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/core/documentregistry.h"
#include "pv/interface/icontextaware.h"
#include "pv/session/sessionmanager.h"
#include "pv/session/tabcontext.h"
#include "pv/ui/draggabletabwidget.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/signal.h"
#include "pv/view/trace/trace.h"
#include "pv/view/view.h"
#include "pv/view/component/viewstatus.h"
#include "pv/view/viewport/viewport.h"

/* __STDC_FORMAT_MACROS is required for PRIu64 and friends (in C++). */
#include "pv/base/ZipMaker.h"
#include "pv/api/app_service.h"
#include "pv/mainwindow/appcontrol.h"
#include "pv/config/appconfig.h"
#include "pv/config/shortcutdefs.h"
#include "pv/session/deviceagent.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/mainwindow/mainframe.h"
#include "pv/session/sigsession.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/ui/uimanager.h"
#include "pv/utility/encoding.h"
#include "pv/utility/path.h"
#include <glib.h>
#include <inttypes.h>
#include <list>
#include <stdarg.h>
#include <cstdint>
#include <cstdlib>
#include <thread>

#ifdef ENABLE_DEBUG_HELPER
#include "pv/ui/widgetinspector.h"
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
make_channel_layout(pv::view::View *view) {
  std::map<int, pv::data::ChannelLayoutState> layout;
  if (view) {
    for (auto &sig : view->get_own_signals()) {
      pv::data::ChannelLayoutState s;
      s.view_index = sig->get_view_index();
      s.v_offset = sig->get_v_offset();
      s.own_height = sig->get_own_height();
      layout[sig->get_index()] = s;
    }
  }
  return layout;
}
} // namespace

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
  _device_agent = _session->get_device();
// Phase 2: initialise the config I/O delegate.
_config_io = std::make_unique<MainWindowConfigIO>(this);
_event_dispatcher = std::make_unique<SessionEventDispatcher>(this, _session->get_event_bus());
_tab_manager = std::make_unique<TabManager>(this);
_dock_manager = std::make_unique<DockManager>(this);
_theme_manager = std::make_unique<MainWindowThemeManager>(this);
_status_bar = std::make_unique<MainWindowStatusBar>(this);
_shortcut_manager = std::make_unique<MainWindowShortcutManager>(this);
_signal_connector = std::make_unique<MainWindowSignalConnector>(this);
_file_ops = std::make_unique<MainWindowFileOps>(this);

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
  // Subscriptions auto-unsubscribe via RAII in SessionEventDispatcher.
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

  _tab_manager->create_tab_widget(this, _vertical_layout);
  _tab_manager->init_initial_tab();

  // Phase 2: Ribbon setup extracted to ThemeManager delegate.
  _theme_manager->setupRibbonCategories();
  setIconSize(QSize(16, 16));

  // Phase 2: dock creation, sliding drawer, sidebar, and connections
  // are all handled by the DockManager delegate.
  pv::view::View *initial_view = _tab_manager->current_view();
  _dock_manager->create_docks(initial_view);
  _dock_manager->setup_drawer(_central_widget, _vertical_layout);
  _dock_manager->setup_side_bar();
  _dock_manager->setup_connections();

  // event filter (non-dock widgets)
  initial_view->installEventFilter(this);
  _sampling_bar->installEventFilter(this);
  _trig_bar->installEventFilter(this);
  _file_bar->installEventFilter(this);
  _logo_bar->installEventFilter(this);
  _dock_manager->install_event_filters(this);

  // defaut language
  AppConfig &app = AppConfig::Instance();
  switchLanguage(app.frameOptions.language);
  switchTheme(app.frameOptions.style);

  _sampling_bar->set_view(initial_view);

  // Phase 2: signal/slot wiring extracted to MainWindowSignalConnector.
  _signal_connector->setup_connections();

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
  _status_bar->init(_disk_cache_status_label, _trig_time_label,
                    _sample_period_label, _fps_label);
  if (!_tab_manager->contexts().isEmpty()) {
    _tab_manager->contexts()[0]->activate();
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
  _dock_manager->retranslateUi();
  _theme_manager->retranslateRibbon();
}

void MainWindow::on_load_file(QString file_name) { _file_ops->on_load_file(file_name); }

void MainWindow::on_import_file(QString file_name) { _file_ops->on_import_file(file_name); }


void MainWindow::on_session_error() { _event_dispatcher->handle_session_error(); }

void MainWindow::save_config() { _config_io->save_config(); }

QString MainWindow::gen_config_file_path(bool isNewFormat) { return _config_io->gen_config_file_path(isNewFormat); }

bool MainWindow::able_to_close() {
  // Only commit UI settings to device when the device has no prior capture
  // data. If the device has data, the settings were already committed during
  // capture setup. Calling commit_settings() unconditionally would overwrite
  // device values (e.g., sample limit loaded from .pxc) with UI dropdown
  // values, which may not have the exact same option (e.g., 200M vs 1G).
  if (_device_agent->is_hardware() && _session->have_hardware_data() == false) {
    _sampling_bar->commit_settings();
  }

  _tab_manager->close_detached_windows();

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

void MainWindow::on_side_bar_dock_clicked(int index) { _dock_manager->on_side_bar_dock_clicked(index); }

void MainWindow::on_side_bar_action_clicked(int index) { _dock_manager->on_side_bar_action_clicked(index); }

void MainWindow::on_screenShot() { _file_ops->on_screenShot(); }

void MainWindow::on_save() { _file_ops->on_save(); }

void MainWindow::on_export() { _file_ops->on_export(); }

bool MainWindow::on_load_session(QString name) {
  return load_config_from_file(name);
}

bool MainWindow::load_config_from_file(QString file) { return _config_io->load_config_from_file(file); }

bool MainWindow::gen_config_json(QJsonObject &sessionVar) { return _config_io->gen_config_json(sessionVar); }

bool MainWindow::load_config_from_json(QJsonDocument &doc, bool &haveDecoder) { return _config_io->load_config_from_json(doc, haveDecoder); }

bool MainWindow::on_store_session(QString name) {
  return save_config_to_file(name);
}

bool MainWindow::save_config_to_file(QString name) { return _config_io->save_config_to_file(name); }

bool MainWindow::genSessionData(std::string &str) { return _config_io->genSessionData(str); }

::DockOptions *MainWindow::getDockOptions() { return _dock_manager->getDockOptions(); }
void MainWindow::restore_dock() { _dock_manager->restore_dock(); }

int MainWindow::resolveShortcutAction(int key, int modifiers) {
  return _shortcut_manager->resolveShortcutAction(key, modifiers);
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::KeyPress)
    return _shortcut_manager->handleKeyPress(object, event);
  return false;
}

void MainWindow::switchLanguage(int language) { _theme_manager->switchLanguage(language); }

void MainWindow::switchTheme(QString style) {
  _theme_manager->switchTheme(style);
}


void MainWindow::on_data_updated() {
  _dock_manager->measure_widget()->reCalc();
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


void MainWindow::on_update_capture() { current_view()->update_hori_res(); }


void MainWindow::on_cur_snap_samplerate_changed() {
  _dock_manager->measure_widget()->reCalc();
  update_sample_period();
}

/*------------------on event end-------*/

void MainWindow::on_signals_changed() {
  // Rebuild View signals from current SignalModels
  // (SignalFactory::update_signals with AllReplaced preserves UI state), then
  // refresh layout. This ensures LogicSignals pick up new SignalModel pointers
  // and Qt signal/slot connections are re-established after
  // init_signals()/reload() recreates models.
  current_view()->on_signals_changed();
}

void MainWindow::on_receive_trigger(quint64 trigger_pos) {
  current_view()->receive_trigger(trigger_pos);
}

void MainWindow::on_frame_ended() {
  pxv_info("MainWindow::on_frame_ended() [UI-only: Core handles copy+decode+guard]");
  _acq_count++;
  _dock_manager->side_bar()->setItemRunning(SIDEBAR_RUNSTOP, false);
  _dock_manager->side_bar()->setItemRunning(SIDEBAR_INSTANT, false);

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
        _session->get_signal_models(), make_channel_layout(current_view()));
  }
  current_view()->receive_end();
}

void MainWindow::on_frame_began() {
  if (_session->is_instant()) {
    _dock_manager->side_bar()->setItemRunning(SIDEBAR_INSTANT, true);
  } else {
    _dock_manager->side_bar()->setItemRunning(SIDEBAR_RUNSTOP, true);
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

void MainWindow::on_show_region(quint64 start, quint64 end, bool keep) {
  current_view()->show_region((uint64_t)start, (uint64_t)end, keep);
}

void MainWindow::on_show_wait_trigger() { current_view()->show_wait_trigger(); }

void MainWindow::on_repeat_hold(int percent) {
  (void)percent;
  current_view()->repeat_show();
}

void MainWindow::on_decode_done() { _dock_manager->protocol_widget()->update_model(); }

void MainWindow::on_receive_data_len(quint64 len) {
  current_view()->set_receive_len(len);
}

void MainWindow::check_usb_device_speed() { _event_dispatcher->check_usb_device_speed(); }

void MainWindow::reset_all_view() {
  _sampling_bar->reload();
  current_view()->status_clear();
  current_view()->reload();
  current_view()->set_device();
  _dock_manager->trigger_widget()->update_view();
  _dock_manager->trigger_widget()->device_updated();
  _trig_bar->reload();
  _dock_manager->dso_trigger_widget()->update_view();
  _dock_manager->measure_widget()->reload();
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

void MainWindow::check_config_file_version() { _config_io->check_config_file_version(); }

void MainWindow::load_device_config() { _config_io->load_device_config(); }

QJsonDocument MainWindow::get_config_json_from_data_file(QString file,
                                                         bool &bSucesss) { return _config_io->get_config_json_from_data_file(file, bSucesss); }

QJsonArray MainWindow::get_decoder_json_from_data_file(QString file,
                                                       bool &bSucesss) { return _config_io->get_decoder_json_from_data_file(file, bSucesss); }

void MainWindow::update_capture_ui_status() {
  _dock_manager->update_toolbar_view_status();
  _dock_manager->protocol_widget()->update_view_status();
  _dock_manager->device_options_widget()->update_widgets_status();
}

// Phase 2: update_toolbar_view_status() extracted to DockManager.
// This thin forwarder keeps existing callers (SessionEventDispatcher,
// on_frame_ended, etc.) working without changes.
void MainWindow::update_toolbar_view_status() {
  _dock_manager->update_toolbar_view_status();
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

// Phase 2: Public wrapper for SessionEventDispatcher
std::map<int, pv::data::ChannelLayoutState>
MainWindow::build_channel_layout(pv::view::View *view) {
  return make_channel_layout(view);
}

// ---------------------------------------------------------------------------
// IEventListener forwarding removed — SessionEventDispatcher now registers
// directly with EventBus via subscribe<T>(). No forwarding needed.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// IServiceEventListener — forwarded to SessionEventDispatcher (Phase 2).
// The actual View-operation routing logic lives in the dispatcher.
// ---------------------------------------------------------------------------
void MainWindow::on_service_event(const pv::api::ServiceEventData &data) {
  _event_dispatcher->on_service_event(data);
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

void MainWindow::load_demo_decoder_config(QString optname) { _config_io->load_demo_decoder_config(optname); }

QWidget *MainWindow::GetBodyView() { return current_view(); }

pv::view::View *MainWindow::current_view() { return _tab_manager->current_view(); }
pv::TabContext *MainWindow::current_context() { return _tab_manager->current_context(); }
void MainWindow::add_tab(pv::TabContext *ctx) { _tab_manager->add_tab(ctx); }
void MainWindow::remove_tab(int index) { _tab_manager->remove_tab(index); }
void MainWindow::update_tab_style(int index) { _tab_manager->update_tab_style(index); }
void MainWindow::on_tab_changed(int index) { _tab_manager->on_tab_changed(index); }
void MainWindow::on_tab_moved(int from, int to) { _tab_manager->on_tab_moved(from, to); }
void MainWindow::on_tab_detach(int index, QWidget *widget, const QString &title) { _tab_manager->on_tab_detach(index, widget, title); }
void MainWindow::on_tab_attached(QWidget *widget, const QString &title) { _tab_manager->on_tab_attached(widget, title); }
void MainWindow::on_new_tab_requested() { _tab_manager->on_new_tab_requested(); }
void MainWindow::update_disk_cache_status() {
  _status_bar->update_disk_cache_status();
}

void MainWindow::update_fps() {
  _status_bar->update_fps();
}

void MainWindow::update_sample_period() {
  _status_bar->update_sample_period();
}

} // namespace pv
