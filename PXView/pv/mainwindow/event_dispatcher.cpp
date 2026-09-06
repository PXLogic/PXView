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
#include "pv/mainwindow/tab_manager.h"
#include "pv/session/deviceagent.h"
#include "pv/session/sigsession.h"
#include "pv/session/tabcontext.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/ui/uimanager.h"
#include "pv/ui/msgbox.h"
#include "pv/core/langresource.h"
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
  // Throttle timer for SignalsChanged: singleShot, 50ms delay.
  // When multiple SignalsChanged events arrive in rapid succession,
  // only the first triggers the timer; subsequent events are coalesced
  // (timer is already running so they're ignored). The actual
  // on_signals_changed logic runs once when the timer fires.
  _signals_changed_timer.setSingleShot(true);
  _signals_changed_timer.setInterval(50);
  QObject::connect(&_signals_changed_timer, &QTimer::timeout, [this]() {
    if (_window.isNull()) return;
    _window->on_signals_changed();
  });
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
  _subscriptions.push_back(_bus->subscribe<pv::interface::FileDeviceClosed>(
      [this](const pv::interface::FileDeviceClosed &e) { on_file_device_closed(e); }));
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
  _subscriptions.push_back(_bus->subscribe<pv::interface::DecoderAnalogTriggerFound>(
      [this](const pv::interface::DecoderAnalogTriggerFound &e) { on_decoder_analog_trigger_found(e); }));
  _subscriptions.push_back(_bus->subscribe<pv::interface::DecoderAnalogTriggerDisplayHold>(
      [this](const pv::interface::DecoderAnalogTriggerDisplayHold &e) { on_decoder_analog_trigger_display_hold(e); }));
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
  // Symmetric with on_start_collect_work(): that handler latches the sidebar
  // Start/Instant button into its "running" (Stop) state, so this one must
  // release it. Relying solely on CollectEnd -> MainWindow::on_frame_ended()
  // left a hole: any capture that ends without SR_DF_END never emits
  // CollectEnd, and the button stayed stuck on "Stop" forever. Releasing here
  // as well is idempotent and makes the latch impossible to leak.
  _window->dock_manager()->side_bar()->setItemRunning(_window->SIDEBAR_RUNSTOP, false);
  _window->dock_manager()->side_bar()->setItemRunning(_window->SIDEBAR_INSTANT, false);
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
void SessionEventDispatcher::on_current_device_changed(const pv::interface::CurrentDeviceChanged &ev) {
  PV_WIN_GUARD();
  // Session-Centric 阶段1（止血）：TabSwitch 是"tab 恢复自己的设备"，
  // TabContext::activate() 已在同步路径完成数据绑定/布局恢复；
  // reset_all_view() 的全局副作用（采样栏/触发栏/dock 全量刷新）属于
  // 设备身份变更语义，TabSwitch 一律跳过——切换从"摧毁重建"变"恢复"。
  if (ev.reason != pv::interface::DeviceChangeReason::TabSwitch)
    _window->reset_all_view();
  // 架构重构 Phase 1/2：设备 profile（demo0.pxc / 硬件 per-device profile）
  // 只在「设备身份变更」的场景作为初始化来源加载 —— FirstInit（首启）与
  // UserSelection（用户/接口选择了另一设备，标签页旧 config 已过期，加载
  // profile 后经本 handler 尾部 save_signal_config 写入标签页文档）。
  // TabSwitch 时标签页 config 已由 TabContext::activate() 的
  // apply_signal_config() 恢复，是权威来源，绝不能被 profile 覆盖 ——
  // 这正是"切回 demo 后通道数和 header 名被还原"的历史根因。
  // 显式 reason 语义取代了曾经的 has_signal_config() 启发式与
  // broadcast_async 事件时序巧合依赖。
  if (ev.reason != pv::interface::DeviceChangeReason::TabSwitch) {
    // Phase 4：profile 加载内部会 set CHANNEL_MODE 等配置，触发排队的
    // DeviceModeChanged → on_device_mode_changed → 再次 load_device_config
    // （历史日志中一次设备选择双重加载 + 双份全量刷新）。置位抑制标志，
    // 经排队调用清除 —— 排队清除必然晚于 profile 加载期间排队的全部
    // DeviceModeChanged 事件（同一事件队列 FIFO）。
    _window->loading_device_profile = true;
    _window->load_device_config();
    QMetaObject::invokeMethod(
        _window,
        [w = _window]() { w->loading_device_profile = false; },
        Qt::QueuedConnection);
  }
  _window->update_title_bar_text();
  _window->sampling_bar()->update_device_list();
  _window->sampling_bar()->reload();
  _window->dock_manager()->device_options_widget()->update_view();
  _window->logo_bar()->dsl_connected(_window->session()->get_device()->is_hardware());
  _window->update_toolbar_view_status();
  _window->session()->device_event_object()->device_updated();

  // Rebind model (device-keyed data pool): leaving a file-device slot in the
  // same tab rebinds the tab to a fresh document for the newly-active device.
  // The file slot stays pinned in the TabContext (snapshots + decoder stacks +
  // harvested intent survive); subsequent captures on the new device then
  // write to the fresh document instead of clobbering the pool slot. This
  // MUST run before the save_signal_config block below, so the new device's
  // freshly-rebuilt models are saved into the new document — not into the
  // pinned file slot (which would corrupt its harvested intent).
  {
    pv::TabContext *ctx = _window->current_context();
    if (ctx && ctx->document() && ctx->document()->is_file_device_slot() &&
        ctx->document()->device_handle() != ev.handle) {
      _window->rebind_current_tab_to_fresh_document();
    }
  }

  {
    pv::TabContext *ctx = _window->current_context();
    if (ctx && ctx->document()) {
      ctx->document()->save_signal_config(
          _window->session()->get_signal_models(),
          _window->build_channel_layout(safe_current_view()));
      if (auto *v = safe_current_view()) v->rebuild_signals();
    }
  }

  if (_window->device_agent()->is_hardware()) {
    _window->session()->on_load_config_end();
  }

  if (_window->device_agent()->get_work_mode() == LOGIC &&
      _window->device_agent()->is_file() == false) {
    // 修复：切回已采集数据的旧 tab 时保留其视图位置（缩放/滚动），
    // 不要重置为"适配全部数据"。只有当前 tab 文档无数据（首次加载/新建）
    // 时才 auto_set_max_scale 自动适配。
    pv::TabContext *cur = _window->current_context();
    const bool tab_has_data = cur && cur->document() && cur->document()->has_data();
    if (!tab_has_data)
      if (auto *v = safe_current_view()) v->auto_set_max_scale();
  }

  if (_window->device_agent()->is_file()) {
    _window->check_config_file_version();

    // Rebind model (device-keyed data pool) — cache-first ruling:
    // If the current tab's document is this file device's pool slot and
    // already holds data (+ decoder stacks), serve the switch entirely from
    // the pool: skip the file JSON config/decoder reload AND the replay
    // capture. The file is never re-read on switch-back ("不回退到文件").
    // A cache miss (cold switch / data genuinely absent) falls through to
    // the legacy load path below.
    pv::TabContext *cur_ctx = _window->current_context();
    const bool cache_hit =
        cur_ctx && cur_ctx->document() &&
        cur_ctx->document()->is_file_device_slot() &&
        cur_ctx->document()->device_handle() == ev.handle &&
        cur_ctx->document()->has_data();

    if (!cache_hit) {
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
        auto *dock = _window->dock_manager()->protocol_widget();
        ss.load_decoders(
            [dock](const QString &id, bool stacked_ok,
                   std::list<pv::data::decode::Decoder *> &subs) {
                return dock->add_protocol_by_id(id, stacked_ok, subs);
            },
            deArray);
      }
    }

    if (auto *v = safe_current_view()) v->update_all_trace_postion();
    if (!_window->device_agent()->is_input_module()) {
      QTimer::singleShot(100, _window,
                         [this]() { _window->session()->start_capture(true); });
    }
    // 数据模型重构步骤3：Cold switch adoption 已删除——这里曾把"当前 tab"
    // 的文档改绑为该文件设备的池槽并改写 tab 身份，是"demo tab 被静默变成
    // 文件设备"的直接来源。文件设备的身份归属由建签流程（file_ops）显式
    // 记录；设备下拉选择文件设备统一走 MainWindow::route_to_file_device_data
    // 的"激活属主 tab"路径，不再有改绑。
    } else {
      // Cache hit: data + decoder stacks came from the pool slot — nothing to
      // reload, just re-fit the restored traces.
      if (auto *v = safe_current_view()) v->update_all_trace_postion();
    }
  } else if (_window->device_agent()->is_demo()) {
    if (_window->device_agent()->get_work_mode() == LOGIC) {
      _window->pattern_mode() = _window->device_agent()->get_demo_operation_mode();
      _window->dock_manager()->protocol_widget()->del_all_protocol();
      // 修复：切回已采集数据的旧 tab 时保留其视图位置，不要重置为
      // "适配全部数据"（与上面 LOGIC 分支同理）。
      pv::TabContext *cur = _window->current_context();
      const bool tab_has_data = cur && cur->document() && cur->document()->has_data();
      if (!tab_has_data)
        if (auto *v = safe_current_view()) v->auto_set_max_scale();

      if (_window->pattern_mode() != "random") {
        _window->load_demo_decoder_config(_window->pattern_mode());
      }
    }
  }

  // Rebind model: reconcile View traces with the bound document's decoder
  // stacks, then rebuild protocol dock rows from the traces. For file-device
  // pool-slot flows del_all_protocol was skipped in the prev handler (stacks
  // preserved) and a cache hit runs no JSON reload — this is the only
  // row/trace refresh on those paths. Non-protected flows are unaffected.
  if (auto *v = safe_current_view()) {
    v->mark_derived_traces_dirty();
    v->sync_derived_traces();
  }
  _window->dock_manager()->protocol_widget()->rebuild_protocol_layers();

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
// Rebind model v3: unified device-identity invalidation point (pool rule:
// 槽存活 = 设备存活). close_file() broadcasts this AFTER the sdi has been
// freed; every holder of the dead identity drops it here. Idempotent with
// TabManager::remove_tab's inline detach (which runs synchronously first):
// by the time this handler runs, tabs already detached by remove_tab see a
// fresh document and do nothing.
void SessionEventDispatcher::on_file_device_closed(
    const pv::interface::FileDeviceClosed &ev) {
  PV_WIN_GUARD();
  const ds_device_handle h = (ds_device_handle)ev.handle;
  if (h == NULL_HANDLE)
    return;
  pxv_info("FileDeviceClosed: invalidating identity of handle %llu",
           (unsigned long long)h);
  for (pv::TabContext *ctx : _window->tab_manager()->contexts()) {
    const bool current_binding_is_dead_slot =
        ctx->document() && ctx->document()->is_file_device_slot() &&
        ctx->document()->device_handle() == h;
    if (current_binding_is_dead_slot) {
      // The tab still binds the dead slot (a close path that did not detach
      // inline, e.g. an API/headless-initiated close_file). Rebind it to a
      // fresh document; only the foreground tab may claim the active doc.
      _window->rebind_tab_to_fresh_document(
          ctx, ctx == _window->current_context());
    }
    ctx->invalidate_device(h);
  }
}

void SessionEventDispatcher::on_device_open_failed(const pv::interface::DeviceOpenFailed &evt) {  QString driver = QString::fromStdString(evt.driver_name);
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
void SessionEventDispatcher::on_device_mode_changed(const pv::interface::DeviceModeChanged &ev) {
  PV_WIN_GUARD();
  if (auto *v = safe_current_view()) v->mode_changed();
  // 阶段1（止血）：TabSwitch 期间的工作模式变化来自标签页意图应用，
  // 与 on_current_device_changed 同理跳过 reset_all_view 的全局副作用。
  if (ev.reason != pv::interface::DeviceChangeReason::TabSwitch)
    _window->reset_all_view();
  // 架构演进：DeviceModeChanged 现自携带 reason（switch_work_mode 广播时
  // 透传 SigSession 记录的最近一次设备切换原因），不再依赖 MainWindow 的
  // last_device_change_reason 状态转发。裁决规则与 on_current_device_changed
  // 一致：TabSwitch 期间的工作模式变化来自标签页意图应用，绝不加载
  // profile；loading_device_profile 期间跳过以免双重加载。
  if (ev.reason != pv::interface::DeviceChangeReason::TabSwitch &&
      !_window->loading_device_profile)
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
  // Throttle: if the timer is already pending, this event is coalesced.
  // This prevents N full signal-layout passes from running back-to-back
  // when N SignalsChanged events are queued (e.g. when MCP adds multiple
  // decoders in rapid succession). The timer fires once after 50ms of
  // quiet, processing the latest state.
  if (!_signals_changed_timer.isActive())
    _signals_changed_timer.start();
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
  // Rebind model (device-keyed data pool): del_all_protocol() funnels through
  // View into data_source()->clear_all_decoder(), which wipes the ACTIVE
  // document's decoder stacks. When a file-device pool slot is involved we
  // must NOT wipe it — its snapshots + decoder stacks are pinned and restored
  // on switch-back. Shapes covered:
  //  - tab switch where the outgoing/active doc is a file slot, or the
  //    incoming tab's doc is one (cache-first restore).
  // NOTE: NO intent harvest here. This handler runs AFTER set_device() has
  // fully completed (async broadcast), so "current device == slot device"
  // would wrongly fire on switch-IN and stash_signal_models_to() would MOVE
  // the live session models out mid-activation (blank view + corrupted slot
  // config). The leave-side harvest runs synchronously at the actual leave
  // point (SamplingBar::on_device_selected) while the outgoing device is
  // still current.
  pv::TabContext *ctx = _window->current_context();
  data::SessionDocument *active_doc = _window->session()->get_active_document();
  const bool protect_file_slot =
      (ctx && ctx->document() && ctx->document()->is_file_device_slot()) ||
      (active_doc && active_doc->is_file_device_slot());
  if (!protect_file_slot)
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
    // changed). Mark derived traces dirty immediately (cheap — just
    // sets a flag), but defer the expensive signals_changed(nullptr)
    // layout pass to the throttle timer so that rapid bursts (e.g.
    // 16 decoders added in succession) don't cause 16 full layout
    // passes back-to-back.
    view->mark_derived_traces_dirty();
    if (!_signals_changed_timer.isActive())
      _signals_changed_timer.start();
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

void SessionEventDispatcher::on_decoder_analog_trigger_found(
    const pv::interface::DecoderAnalogTriggerFound &e) {
  PV_WIN_GUARD();
  // Generation token check: if the session's current generation has advanced
  // past the event's generation, this event is stale (from a previous repeat
  // frame) and should be discarded to avoid displaying an outdated trigger.
  if (_window->session() &&
      e.generation != _window->session()->repeat_analog_trigger_ui_generation_for_ui())
    return;
  if (auto *v = safe_current_view())
    v->set_decoder_analog_trigger_position(e.sample_position,
                                          e.display_position_percent);
}

void SessionEventDispatcher::on_decoder_analog_trigger_display_hold(
    const pv::interface::DecoderAnalogTriggerDisplayHold &e) {
  PV_WIN_GUARD();
  // Generation token check: discard stale hold/release events from a
  // previous repeat frame.
  if (_window->session() &&
      e.generation != _window->session()->repeat_analog_trigger_ui_generation_for_ui())
    return;
  if (auto *v = safe_current_view())
    v->set_decoder_analog_trigger_display_hold(e.hold);
}
