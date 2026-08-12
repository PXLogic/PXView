// mainwindow_event_dispatcher.cpp
// SessionEventDispatcher — handles all session events for MainWindow.
// Events registered via EventBus::subscribe<T>() (no IEventListener).

#include "pv/mainwindow/event_dispatcher.h"

#include <QApplication>
#include <QObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStatusBar>
#include <QTimer>

#include "pv/base/log.h"
#include "pv/mainwindow/mainwindow.h"
#include "pv/mainwindow/dock_manager.h"
#include "pv/session/deviceagent.h"
#include "pv/session/sigsession.h"
#include "pv/session/tabcontext.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/ui/uimanager.h"
#include "pv/ui/msgbox.h"
#include "pv/ui/langresource.h"
#include "pv/ui/string_ids.h"
#include "pv/view/view.h"
#include "pv/dock/protocoldock.h"
#include "pv/dock/triggerdock.h"
#include "pv/dock/dsotriggerdock.h"
#include "pv/dock/measuredock.h"
#include "pv/dock/deviceoptionsdock.h"
#include "pv/dock/searchdock.h"
#include "pv/dialogs/dsmessagebox.h"
#include "pv/session/storesession.h"
#include "pv/toolbars/samplingbar.h"
#include "pv/toolbars/logobar.h"
#include "pv/toolbars/titlebar.h"
#include "pv/widgets/sidebar.h"
#include "pv/base/pxvdef.h"
#include "pv/api/types.h"

// build_channel_layout is accessed via MainWindow::build_channel_layout()

using namespace pv;

// ===========================================================================
// Helper macros — MainWindow's private members are accessed via _window->
// ===========================================================================

// P1 guard: SessionEventDispatcher is an IEventListener registered with the
// global EventBus. Events are broadcast via Qt::QueuedConnection onto qApp's
// event loop, so a queued event may still fire AFTER MainWindow has been
// destroyed. _window is a QPointer, so it becomes null on teardown — bail out
// of every handler the moment the owning window is gone.
#define PV_WIN_GUARD() \
  do {                 \
    if (_window.isNull()) return; \
  } while (0)

namespace pv {

SessionEventDispatcher::SessionEventDispatcher(MainWindow *window, core::EventBus *bus)
    : _window(window), _bus(bus) {
  _subscriptions.push_back(_bus->subscribe<pv::interface::CaptureStateChanged>(
      [this](const pv::interface::CaptureStateChanged &e) { on_capture_state_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CaptureOwnerChanged>(
      [this](const pv::interface::CaptureOwnerChanged &e) { on_capture_owner_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::TriggerConfigChanged>(
      [this](const pv::interface::TriggerConfigChanged &e) { on_trigger_config_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SampleCountUpdated>(
      [this](const pv::interface::SampleCountUpdated &e) { on_sample_count_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DeviceOptionsUpdated>(
      [this](const pv::interface::DeviceOptionsUpdated &e) { on_device_options_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DsoViewOptionChanged>(
      [this](const pv::interface::DsoViewOptionChanged &e) { on_dso_view_option_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::ActiveDocumentChanged>(
      [this](const pv::interface::ActiveDocumentChanged &e) { on_active_document_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CopyToDocDone>(
      [this](const pv::interface::CopyToDocDone &e) { on_copy_to_doc_done(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DecodeDone>(
      [this](const pv::interface::DecodeDone &e) { on_decode_done(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SignalsChanged>(
      [this](const pv::interface::SignalsChanged &e) { on_signals_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DataUpdated>(
      [this](const pv::interface::DataUpdated &e) { on_data_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DeviceModeChanged>(
      [this](const pv::interface::DeviceModeChanged &e) { on_device_mode_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CollectModeChanged>(
      [this](const pv::interface::CollectModeChanged &e) { on_collect_mode_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DeviceListUpdated>(
      [this](const pv::interface::DeviceListUpdated &e) { on_device_list_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CurrentDeviceChanged>(
      [this](const pv::interface::CurrentDeviceChanged &e) { on_current_device_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DeviceOpenFailed>(
      [this](const pv::interface::DeviceOpenFailed &e) { on_device_open_failed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::UsbDeviceArrived>(
      [this](const pv::interface::UsbDeviceArrived &e) { on_usb_device_arrived(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DeviceDetached>(
      [this](const pv::interface::DeviceDetached &e) { on_device_detached(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SampleRateChanged>(
      [this](const pv::interface::SampleRateChanged &e) { on_sample_rate_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SaveComplete>(
      [this](const pv::interface::SaveComplete &e) { on_save_complete(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::StartCollectWork>(
      [this](const pv::interface::StartCollectWork &e) { on_start_collect_work(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CollectStart>(
      [this](const pv::interface::CollectStart &e) { on_collect_start(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CollectEnd>(
      [this](const pv::interface::CollectEnd &e) { on_collect_end(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::EndCollectWork>(
      [this](const pv::interface::EndCollectWork &e) { on_end_collect_work(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::EndDeviceOptions>(
      [this](const pv::interface::EndDeviceOptions &e) { on_end_device_options(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DeviceConfigUpdated>(
      [this](const pv::interface::DeviceConfigUpdated &e) { on_device_config_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DemoModeChanged>(
      [this](const pv::interface::DemoModeChanged &e) { on_demo_mode_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DataPoolChanged>(
      [this](const pv::interface::DataPoolChanged &e) { on_data_pool_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SimpleTriggerChanged>(
      [this](const pv::interface::SimpleTriggerChanged &e) { on_simple_trigger_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::GlitchFilterStarted>(
      [this](const pv::interface::GlitchFilterStarted &e) { on_glitch_filter_started(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::GlitchFilterProgress>(
      [this](const pv::interface::GlitchFilterProgress &e) { on_glitch_filter_progress(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::GlitchFilterCompleted>(
      [this](const pv::interface::GlitchFilterCompleted &e) { on_glitch_filter_completed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::GlitchFilterCleared>(
      [this](const pv::interface::GlitchFilterCleared &e) { on_glitch_filter_cleared(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SignalInvertStarted>(
      [this](const pv::interface::SignalInvertStarted &e) { on_signal_invert_started(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SignalInvertCompleted>(
      [this](const pv::interface::SignalInvertCompleted &e) { on_signal_invert_completed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SignalInvertCleared>(
      [this](const pv::interface::SignalInvertCleared &e) { on_signal_invert_cleared(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CopyInProgressChanged>(
      [this](const pv::interface::CopyInProgressChanged &e) { on_copy_in_progress_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::TrigNextCollect>(
      [this](const pv::interface::TrigNextCollect &e) { on_trig_next_collect(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::ClearDecodeData>(
      [this](const pv::interface::ClearDecodeData &e) { on_clear_decode_data(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::AppOptionsChanged>(
      [this](const pv::interface::AppOptionsChanged &e) { on_app_options_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::FontOptionsChanged>(
      [this](const pv::interface::FontOptionsChanged &e) { on_font_options_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::ShortcutChanged>(
      [this](const pv::interface::ShortcutChanged &e) { on_shortcut_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::StyleChanged>(
      [this](const pv::interface::StyleChanged &e) { on_style_changed(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::StoreConfPrev>(
      [this](const pv::interface::StoreConfPrev &e) { on_store_conf_prev(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CurrentDeviceChangePrev>(
      [this](const pv::interface::CurrentDeviceChangePrev &e) { on_current_device_change_prev(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::StartCollectWorkPrev>(
      [this](const pv::interface::StartCollectWorkPrev &e) { on_start_collect_work_prev(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::EndCollectWorkPrev>(
      [this](const pv::interface::EndCollectWorkPrev &e) { on_end_collect_work_prev(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DataLenUpdated>(
      [this](const pv::interface::DataLenUpdated &e) { on_data_len_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::HeaderReceived>(
      [this](const pv::interface::HeaderReceived &e) { on_header_received(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::CaptureUpdated>(
      [this](const pv::interface::CaptureUpdated &e) { on_capture_updated(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::ShowRegion>(
      [this](const pv::interface::ShowRegion &e) { on_show_region(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::RepeatHold>(
      [this](const pv::interface::RepeatHold &e) { on_repeat_hold(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::TriggerReceived>(
      [this](const pv::interface::TriggerReceived &e) { on_trigger_received(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::ShowWaitTrigger>(
      [this](const pv::interface::ShowWaitTrigger &e) { on_show_wait_trigger(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SessionError>(
      [this](const pv::interface::SessionError &e) { on_session_error(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SaveRequested>(
      [this](const pv::interface::SaveRequested &e) { on_save_requested(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DelayedPropMsg>(
      [this](const pv::interface::DelayedPropMsg &e) { on_delayed_prop_msg(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::SampleLimitsChanged>(
      [this](const pv::interface::SampleLimitsChanged &e) { on_sample_limits_changed(e); }));
}
// P0/P1: Safely resolve the current view. Returns nullptr if MainWindow is
// gone (QPointer) or no tab is active. Callers MUST null-check the result
// before dereferencing.
view::View *SessionEventDispatcher::safe_current_view() const {
  if (_window.isNull())
    return nullptr;
  return _window->current_view();
}
} // namespace pv

// --- Capture state group ---
void SessionEventDispatcher::on_capture_state_changed(const pv::interface::CaptureStateChanged &) {
  _window->update_capture_ui_status();
}
void SessionEventDispatcher::on_start_collect_work(const pv::interface::StartCollectWork &) {
  PV_WIN_GUARD();
  _window->update_capture_ui_status();
  if (_window->session()->is_instant()) {
    _window->dock_manager()->side_bar()->setItemRunning(_window->SIDEBAR_INSTANT, true);
  } else {
      _window->dock_manager()->side_bar()->setItemRunning(_window->SIDEBAR_RUNSTOP, true);
  }
  if (auto *v = safe_current_view()) v->on_state_changed(false);
}
void SessionEventDispatcher::on_collect_start(const pv::interface::CollectStart &) {
  _window->statusBar()->showMessage(MainWindow::tr("采集中..."), 3000);
  _window->on_frame_began();
}
void SessionEventDispatcher::on_collect_end(const pv::interface::CollectEnd &) {
  PV_WIN_GUARD();
  _window->prgRate(0);
  if (auto *v = safe_current_view()) {
    v->repeat_unshow();
    v->on_state_changed(true);
  }
  _window->on_frame_ended();
}
void SessionEventDispatcher::on_end_collect_work(const pv::interface::EndCollectWork &) {
  _window->update_capture_ui_status();

  pv::TabContext *ctx = _window->current_context();
  if (ctx && ctx->document() && ctx->document()->has_pending_config()) {
    ctx->document()->apply_pending_config();
    for (const auto &ch : ctx->document()->get_signal_config().channels) {
      auto m = _window->session()->get_signal_by_index(ch.index);
      if (m)
        m->set_trig_type(ch.trig_type);
    }
    _window->dock_manager()->device_options_widget()->update_view();
  }
  if (ctx) {
    _window->session()->set_active_document(ctx->document());
  }
}
void SessionEventDispatcher::on_trig_next_collect(const pv::interface::TrigNextCollect &) {
  _window->statusBar()->showMessage(MainWindow::tr("等待下一次采集..."), 3000);
}

// --- Device management group ---
void SessionEventDispatcher::on_device_list_updated(const pv::interface::DeviceListUpdated &) {
  _window->sampling_bar()->update_device_list();
}
void SessionEventDispatcher::on_current_device_changed(const pv::interface::CurrentDeviceChanged &) {
  PV_WIN_GUARD();
  _window->reset_all_view();
  _window->load_device_config();
  _window->update_title_bar_text();
  _window->sampling_bar()->update_device_list();
  _window->sampling_bar()->reload();
  _window->dock_manager()->device_options_widget()->update_view();
  _window->logo_bar()->dsl_connected(_window->session()->get_device()->is_hardware());
  _window->update_toolbar_view_status();
  _window->session()->device_event_object()->device_updated();

  {
    pv::TabContext *ctx = _window->current_context();
    if (ctx && ctx->document()) {
      ctx->document()->save_signal_config(
          _window->session()->get_signal_models(),
          _window->build_channel_layout(safe_current_view()));
      if (auto *v = safe_current_view()) v->rebuild_signals();
      pxv_info("CurrentDeviceChanged: saved config and rebuilt signals for current tab");
    }
  }

  if (_window->device_agent()->is_hardware()) {
    _window->session()->on_load_config_end();
  }

  if (_window->device_agent()->get_work_mode() == LOGIC &&
      _window->device_agent()->is_file() == false)
    if (auto *v = safe_current_view()) v->auto_set_max_scale();

  if (_window->device_agent()->is_file()) {
    _window->check_config_file_version();

    bool bDoneDecoder = false;
    bool bLoadSuccess = false;
    QJsonDocument doc =
        _window->get_config_json_from_data_file(_window->device_agent()->path(), bLoadSuccess);

    if (bLoadSuccess) {
      _window->load_config_from_json(doc, bDoneDecoder);
    }

    if (!bDoneDecoder && _window->device_agent()->get_work_mode() == LOGIC) {
      QJsonArray deArray = _window->get_decoder_json_from_data_file(
          _window->device_agent()->path(), bLoadSuccess);
      if (bLoadSuccess) {
        StoreSession ss(_window->session());
        ss.load_decoders(_window->dock_manager()->protocol_widget(), deArray);
      }
    }

    if (auto *v = safe_current_view()) v->update_all_trace_postion();
    if (!_window->device_agent()->is_input_module()) {
      QTimer::singleShot(100, _window,
                         [this]() { _window->session()->start_capture(true); });
    }
  } else if (_window->device_agent()->is_demo()) {
    if (_window->device_agent()->get_work_mode() == LOGIC) {
      _window->pattern_mode() = _window->device_agent()->get_demo_operation_mode();
      _window->dock_manager()->protocol_widget()->del_all_protocol();
      if (auto *v = safe_current_view()) v->auto_set_max_scale();

      if (_window->pattern_mode() != "random") {
        _window->load_demo_decoder_config(_window->pattern_mode());
      }
    }
  }

  _window->calc_min_height();

  if (_window->device_agent()->is_hardware() && _window->device_agent()->is_new_device()) {
    _window->check_usb_device_speed();
  }
}
void SessionEventDispatcher::on_usb_device_arrived(const pv::interface::UsbDeviceArrived &) {
  if (_window->msg() != nullptr) {
    _window->msg()->close();
    _window->msg() = nullptr;
  }

  _window->sampling_bar()->update_device_list();

  if (_window->session()->get_device()->is_hardware() && _window->session()->is_working()) {
    return;
  }

  if (_window->session()->get_device()->is_demo() == false && !_window->is_save_confirm_msg()) {
    QString msgText = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_TO_SWITCH_DEVICE),
                          "To switch the new device?");
    if (MsgBox::Confirm(msgText, "", &_window->msg(), nullptr) == false) {
      _window->msg() = nullptr;
      return;
    }
    _window->msg() = nullptr;
  }

  if (_window->is_save_confirm_msg()) {
    pxv_info("New device attached:Waitting for the confirm box be closed.");
    _window->is_auto_switch_device() = true;
    return;
  }

  if (_window->session()->is_saving()) {
    pxv_info("New device attached:Waitting for store the data. and will switch to new device.");
    _window->is_auto_switch_device() = true;
    return;
  }

  int mode = _window->device_agent()->get_work_mode();

  if (mode != DSO && _window->confirm_to_store_data()) {
    _window->is_auto_switch_device() = true;
    if (_window->session()->is_working())
      _window->session()->stop_capture();
    _window->on_save();
  } else {
    if (_window->session()->is_working())
      _window->session()->stop_capture();
    _window->session()->set_default_device();
  }
}
void SessionEventDispatcher::on_device_detached(const pv::interface::DeviceDetached &) {
  if (_window->msg() != nullptr) {
    _window->msg()->close();
    _window->msg() = nullptr;
  }

  _window->session()->device_event_object()->device_updated();
  _window->save_config();

  if (_window->session()->is_saving()) {
    pxv_info("Device detached:Waitting for store the data. and will switch to new device.");
    _window->is_auto_switch_device() = true;
    return;
  }

  if (_window->confirm_to_store_data()) {
    _window->is_auto_switch_device() = true;
    _window->on_save();
  } else {
    _window->session()->set_default_device();
  }
}
void SessionEventDispatcher::on_device_open_failed(const pv::interface::DeviceOpenFailed &evt) {
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
  MsgBox::Show(title, text, _window);
}

// --- Device options group ---
void SessionEventDispatcher::on_device_options_updated(const pv::interface::DeviceOptionsUpdated &) {
  _window->dock_manager()->trigger_widget()->device_updated();
  _window->dock_manager()->device_options_widget()->device_updated();
  _window->dock_manager()->measure_widget()->reload();

  pv::TabContext *ctx = _window->current_context();
  if (ctx && ctx->document()) {
    ctx->document()->save_signal_config(
        _window->session()->get_signal_models(), _window->build_channel_layout(safe_current_view()));
  }

  if (auto *v = safe_current_view()) {
    v->rebuild_signals();
    v->signals_changed(nullptr);
  }
}
void SessionEventDispatcher::on_dso_view_option_changed(const pv::interface::DsoViewOptionChanged &) {
  _window->dock_manager()->trigger_widget()->device_updated();
  _window->dock_manager()->device_options_widget()->device_updated();
  _window->dock_manager()->measure_widget()->reload();

  pv::TabContext *ctx = _window->current_context();
  if (ctx && ctx->document()) {
    ctx->document()->save_signal_config(
        _window->session()->get_signal_models(), _window->build_channel_layout(safe_current_view()));
  }
}
void SessionEventDispatcher::on_sample_rate_changed(const pv::interface::SampleRateChanged &) {
  _window->dock_manager()->trigger_widget()->device_updated();
  if (auto *v = safe_current_view()) v->timebase_changed();
  _window->on_cur_snap_samplerate_changed();
}
void SessionEventDispatcher::on_sample_count_updated(const pv::interface::SampleCountUpdated &) {
  _window->sampling_bar()->update_sample_count_selector();
}
void SessionEventDispatcher::on_device_mode_changed(const pv::interface::DeviceModeChanged &) {
  PV_WIN_GUARD();
  if (auto *v = safe_current_view()) v->mode_changed();
  _window->reset_all_view();
  _window->load_device_config();
  _window->update_title_bar_text();
  _window->dock_manager()->device_options_widget()->on_mode_changed();
  _window->update_toolbar_view_status();
  _window->sampling_bar()->update_sample_rate_list();
  _window->sampling_bar()->reload();

  {
    pv::TabContext *ctx = _window->current_context();
    if (ctx && ctx->document()) {
      ctx->document()->save_signal_config(
          _window->session()->get_signal_models(),
          _window->build_channel_layout(safe_current_view()));
      if (auto *v = safe_current_view()) v->rebuild_signals();
      pxv_info("DeviceModeChanged: saved config and rebuilt signals for current tab");
    }
  }

  if (_window->device_agent()->is_hardware())
    _window->session()->on_load_config_end();

  if (_window->device_agent()->get_work_mode() == LOGIC)
    if (auto *v = safe_current_view()) v->auto_set_max_scale();

  if (_window->device_agent()->is_demo()) {
    _window->pattern_mode() = _window->device_agent()->get_demo_operation_mode();
    _window->dock_manager()->protocol_widget()->del_all_protocol();

    if (_window->device_agent()->get_work_mode() == LOGIC) {
      if (_window->pattern_mode() != "random") {
        _window->device_agent()->update();
        _window->load_demo_decoder_config(_window->pattern_mode());
      }
    }
  }

  _window->calc_min_height();
}
void SessionEventDispatcher::on_collect_mode_changed(const pv::interface::CollectModeChanged &) {
  PV_WIN_GUARD();
  if (_window->device_agent()->is_demo()) {
    _window->pattern_mode() = _window->device_agent()->get_demo_operation_mode();
  }
  _window->dock_manager()->trigger_widget()->device_updated();
  if (auto *v = safe_current_view()) v->update();
}
void SessionEventDispatcher::on_end_device_options(const pv::interface::EndDeviceOptions &) {
  if (_window->device_agent()->is_demo() && _window->device_agent()->get_work_mode() == LOGIC) {
    QString pattern_mode = _window->device_agent()->get_demo_operation_mode();

    if (pattern_mode != _window->pattern_mode()) {
      _window->pattern_mode() = pattern_mode;

      _window->device_agent()->update();
      _window->session()->clear_view_data();
      _window->session()->init_signals();
      _window->update_toolbar_view_status();
      _window->sampling_bar()->update_sample_rate_list();
      _window->dock_manager()->protocol_widget()->del_all_protocol();

      if (_window->pattern_mode() != "random") {
        _window->session()->set_collect_mode(COLLECT_SINGLE);
        _window->load_demo_decoder_config(_window->pattern_mode());
        _window->session()->start_capture(false);
      }
    }
  }
  _window->calc_min_height();
}
void SessionEventDispatcher::on_demo_mode_changed(const pv::interface::DemoModeChanged &) {
  if (_window->device_agent()->is_demo() && _window->device_agent()->get_work_mode() == LOGIC) {
    QString pattern_mode = _window->device_agent()->get_demo_operation_mode();

    if (pattern_mode != _window->pattern_mode()) {
      _window->pattern_mode() = pattern_mode;

      _window->device_agent()->update();
      _window->session()->clear_view_data();
      _window->session()->init_signals();
      _window->update_toolbar_view_status();
      _window->sampling_bar()->update_sample_rate_list();
      _window->dock_manager()->protocol_widget()->del_all_protocol();

      if (_window->pattern_mode() != "random") {
        _window->session()->set_collect_mode(COLLECT_SINGLE);
        _window->load_demo_decoder_config(_window->pattern_mode());
      }
    }
  }
  _window->calc_min_height();
}

// --- UI options group ---
void SessionEventDispatcher::on_app_options_changed(const pv::interface::AppOptionsChanged &) {
  _window->update_title_bar_text();
}
void SessionEventDispatcher::on_font_options_changed(const pv::interface::FontOptionsChanged &) {
  UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);
}
void SessionEventDispatcher::on_shortcut_changed(const pv::interface::ShortcutChanged &) {
}
void SessionEventDispatcher::on_style_changed(const pv::interface::StyleChanged &) {
  UiManager::Instance()->Update(UI_UPDATE_ACTION_THEME);
  for (QWidget *w : qApp->topLevelWidgets()) {
    w->update();
  }
}

// --- Data group ---
void SessionEventDispatcher::on_data_pool_changed(const pv::interface::DataPoolChanged &) {
  PV_WIN_GUARD();
  if (auto *v = safe_current_view()) v->check_measure();
}
void SessionEventDispatcher::on_copy_in_progress_changed(const pv::interface::CopyInProgressChanged &) {
  if (_window->disk_cache_status_label())
    _window->disk_cache_status_label()->setText(MainWindow::tr("后台数据拷贝中..."));
}
void SessionEventDispatcher::on_active_document_changed(const pv::interface::ActiveDocumentChanged &) {
  _window->update_title_bar_text();
}
void SessionEventDispatcher::on_save_complete(const pv::interface::SaveComplete &) {
  _window->session()->clear_store_confirm_flag();

  if (_window->is_auto_switch_device()) {
    _window->is_auto_switch_device() = false;
    _window->session()->set_default_device();
  } else {
    ds_device_handle devh = _window->sampling_bar()->get_next_device_handle();
    if (devh != NULL_HANDLE) {
      pxv_info("Auto switch to the selected device.");
      _window->session()->set_device(devh);
    }
  }
}
void SessionEventDispatcher::on_clear_decode_data(const pv::interface::ClearDecodeData &) {
  if (_window->device_agent()->get_work_mode() == LOGIC)
    _window->dock_manager()->protocol_widget()->reset_view();
}

// --- Filter / invert group ---
void SessionEventDispatcher::on_glitch_filter_started(const pv::interface::GlitchFilterStarted &) {
  if (_window->disk_cache_status_label())
    _window->disk_cache_status_label()->setText(MainWindow::tr("毛刺滤波处理中..."));
}
void SessionEventDispatcher::on_glitch_filter_progress(const pv::interface::GlitchFilterProgress &e) {
  int p = e.progress;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  _window->statusBar()->showMessage(
      MainWindow::tr("毛刺滤波进行中... %1%").arg(p), 2000);
}
void SessionEventDispatcher::on_glitch_filter_completed(const pv::interface::GlitchFilterCompleted &) {
  pv::TabContext *ctx = _window->current_context();
  if (ctx && ctx->document()) {
    _window->session()->copy_data_to_document(ctx->document());
  }
  _window->session()->restart_decoders();
  if (auto *v = safe_current_view()) {
    v->on_glitch_filter_completed();
  }
}
void SessionEventDispatcher::on_glitch_filter_cleared(const pv::interface::GlitchFilterCleared &) {
  pv::TabContext *ctx = _window->current_context();
  if (ctx && ctx->document()) {
    _window->session()->copy_data_to_document(ctx->document());
  }
  _window->session()->restart_decoders();
  if (auto *v = safe_current_view()) {
    v->on_glitch_filter_cleared();
  }
}
void SessionEventDispatcher::on_signal_invert_started(const pv::interface::SignalInvertStarted &) {
  if (_window->disk_cache_status_label())
    _window->disk_cache_status_label()->setText(MainWindow::tr("信号反相处理中..."));
}
void SessionEventDispatcher::on_signal_invert_completed(const pv::interface::SignalInvertCompleted &) {
  pv::TabContext *ctx2 = _window->current_context();
  if (ctx2 && ctx2->document()) {
    _window->session()->copy_data_to_document(ctx2->document());
  }
  _window->session()->restart_decoders();
}
void SessionEventDispatcher::on_signal_invert_cleared(const pv::interface::SignalInvertCleared &) {
  pv::TabContext *ctx2 = _window->current_context();
  if (ctx2 && ctx2->document()) {
    _window->session()->copy_data_to_document(ctx2->document());
  }
  _window->session()->restart_decoders();
}

// --- Trigger group ---
void SessionEventDispatcher::on_simple_trigger_changed(const pv::interface::SimpleTriggerChanged &) {
  if (_window->dock_manager()->trigger_widget()) {
    _window->dock_manager()->trigger_widget()->select_simple_trigger();
  }
}
void SessionEventDispatcher::on_trigger_config_changed(const pv::interface::TriggerConfigChanged &) {
  if (_window->dock_manager()->trigger_widget())
    _window->dock_manager()->trigger_widget()->update_view();
}

// --- Empty-body / pre-broadcast overrides ---
void SessionEventDispatcher::on_capture_owner_changed(const pv::interface::CaptureOwnerChanged &) {
}
void SessionEventDispatcher::on_copy_to_doc_done(const pv::interface::CopyToDocDone &) {
  PV_WIN_GUARD();
  pv::TabContext *ctx = _window->current_context();
  if (ctx && ctx->document() && ctx->document()->has_data()) {
    if (auto *v = safe_current_view()) v->set_data_document(ctx->document());
  }
}
void SessionEventDispatcher::on_decode_done(const pv::interface::DecodeDone &) {
  PV_WIN_GUARD();
  _window->on_data_updated();
  if (auto *v = safe_current_view()) {
    v->update();
    v->viewport_update();
  }
  _window->on_decode_done();
}
void SessionEventDispatcher::on_signals_changed(const pv::interface::SignalsChanged &) {
  _window->on_signals_changed();
}
void SessionEventDispatcher::on_data_updated(const pv::interface::DataUpdated &) {
  _window->on_data_updated();
}
void SessionEventDispatcher::on_device_config_updated(const pv::interface::DeviceConfigUpdated &) {}

void SessionEventDispatcher::on_store_conf_prev(const pv::interface::StoreConfPrev &) {
  if (_window->device_agent() && _window->device_agent()->is_hardware() &&
      _window->session() && !_window->session()->have_hardware_data()) {
    _window->sampling_bar()->commit_settings();
  }
}

void SessionEventDispatcher::on_current_device_change_prev(const pv::interface::CurrentDeviceChangePrev &) {
  if (_window->msg() != nullptr) {
    _window->msg()->close();
    _window->msg() = nullptr;
  }
  _window->dock_manager()->protocol_widget()->del_all_protocol();
  if (auto *v = safe_current_view()) v->reload();
}

void SessionEventDispatcher::on_start_collect_work_prev(const pv::interface::StartCollectWorkPrev &) {
  if (_window->device_agent()->get_work_mode() == LOGIC)
    _window->dock_manager()->trigger_widget()->try_commit_trigger();
  else if (_window->device_agent()->get_work_mode() == DSO)
    _window->dock_manager()->dso_trigger_widget()->check_setting();

  if (auto *v = safe_current_view()) {
    v->capture_init();
    v->on_state_changed(false);
  }
}

void SessionEventDispatcher::on_end_collect_work_prev(const pv::interface::EndCollectWorkPrev &) {
}

// ---------------------------------------------------------------------------
// IServiceEventListener — route View operation broadcasts from SessionService
// (MCP/WS API) to the active View. In Headless mode there is no MainWindow,
// so these events are simply not consumed.
// Extracted from MainWindow::on_service_event during Phase 2 modernization.
// ---------------------------------------------------------------------------
void SessionEventDispatcher::on_service_event(const pv::api::ServiceEventData &data) {
  pv::view::View *view = safe_current_view();
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
    view->mark_derived_traces_dirty();
    view->signals_changed(nullptr);
    break;
  }
  default:
    // Not a View event; ignore.
    break;
  }
}

// ===========================================================================
// Phase 2: additional delegated logic (moved from MainWindow)
// ===========================================================================

void SessionEventDispatcher::handle_session_error() {
  if (!_window)
    return;

  QString title;
  QString details;

  switch (_window->session()->get_error()) {
  case SigSession::Hw_err:
    pxv_info("SessionEventDispatcher::handle_session_error(),Hw_err, stop capture");
    _window->session()->stop_capture();
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_HARDWARE_ERROR),
                "Hardware Operation Failed");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_HARDWARE_ERROR_DET),
                  "Please replug device to refresh hardware configuration!");
    break;
  case SigSession::Malloc_err:
    pxv_info("SessionEventDispatcher::handle_session_error(),Malloc_err, stop capture");
    _window->session()->stop_capture();
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MALLOC_ERROR), "Malloc Error");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_MALLOC_ERROR_DET),
                  "Memory is not enough for this sample!\nPlease reduce the "
                  "sample depth!");
    break;
  case SigSession::Pkt_data_err:
    title = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PACKET_ERROR), "Packet Error");
    details = L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PACKET_ERROR_DET),
                  "the content of received packet are not expected!");
    _window->session()->refresh(0);
    break;
  case SigSession::Data_overflow:
    pxv_info("SessionEventDispatcher::handle_session_error(),Data_overflow, stop capture");
    _window->session()->stop_capture();
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

  pv::dialogs::DSMessageBox msg(_window, title);
  msg.mBox()->setText(details);
  msg.mBox()->setStandardButtons(QMessageBox::Ok);
  msg.mBox()->setIcon(QMessageBox::Warning);
  QObject::connect(_window->session()->device_event_object(), &DeviceEventObject::device_updated,
          &msg, &QDialog::accept);
  _window->msg() = &msg;
  msg.exec();
  _window->msg() = nullptr;

  _window->session()->clear_error();
}

void SessionEventDispatcher::check_usb_device_speed() {
  if (!_window)
    return;

  // USB device speed check
  if (_window->device_agent()->is_hardware()) {
    int usb_speed = _window->device_agent()->get_usb_speed();
    if (usb_speed == PXV_USB_SPEED_UNKNOWN) {
      return;
    }

    bool usb30_support = _window->device_agent()->is_usb30();
    pxv_info("The device's USB module version: %d.0", usb30_support ? 3 : 2);

    int cable_ver = 1;
    if (usb_speed == PXV_USB_SPEED_HIGH)
      cable_ver = 2;
    else if (usb_speed == PXV_USB_SPEED_SUPER)
      cable_ver = 3;

    pxv_info("The cable's USB port version: %d.0", cable_ver);

    if (usb30_support && usb_speed == PXV_USB_SPEED_HIGH) {
      QString str_err(
          L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHECK_USB_SPEED_ERROR),
              "Plug the device into a USB 2.0 port will seriously affect its "
              "performance.\nPlease replug it into a USB 3.0 port."));
      _window->delay_prop_msg(str_err);
    }
  }
}

// --- Spec v2 Task 7: Handlers for events migrated from ISessionCallback ---

void SessionEventDispatcher::on_data_len_updated(const pv::interface::DataLenUpdated &e) {
  PV_WIN_GUARD();
  _window->on_receive_data_len(e.length);
}

void SessionEventDispatcher::on_header_received(const pv::interface::HeaderReceived &) {
  // Was MainWindow::receive_header() — empty in original implementation.
}

void SessionEventDispatcher::on_capture_updated(const pv::interface::CaptureUpdated &) {
  PV_WIN_GUARD();
  _window->on_update_capture();
}

void SessionEventDispatcher::on_show_region(const pv::interface::ShowRegion &e) {
  PV_WIN_GUARD();
  _window->on_show_region((quint64)e.start, (quint64)e.end, e.keep);
}

void SessionEventDispatcher::on_repeat_hold(const pv::interface::RepeatHold &e) {
  PV_WIN_GUARD();
  _window->on_repeat_hold(e.percent);
}

void SessionEventDispatcher::on_trigger_received(const pv::interface::TriggerReceived &e) {
  PV_WIN_GUARD();
  _window->on_receive_trigger(e.trigger_pos);
}

void SessionEventDispatcher::on_show_wait_trigger(const pv::interface::ShowWaitTrigger &) {
  PV_WIN_GUARD();
  _window->on_show_wait_trigger();
}

void SessionEventDispatcher::on_session_error(const pv::interface::SessionError &) {
  PV_WIN_GUARD();
  _window->on_session_error();
}

void SessionEventDispatcher::on_save_requested(const pv::interface::SaveRequested &) {
  PV_WIN_GUARD();
  _window->save_config();
}

void SessionEventDispatcher::on_delayed_prop_msg(const pv::interface::DelayedPropMsg &e) {
  PV_WIN_GUARD();
  _window->delay_prop_msg(e.message);
}

void SessionEventDispatcher::on_sample_limits_changed(const pv::interface::SampleLimitsChanged &) {
  // Was ICaptureCallback::cur_samplelimits_changed() — empty default in original.
}
