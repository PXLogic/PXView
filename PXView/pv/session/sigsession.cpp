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

#include <libsigrokdecode.h>
#include <libusb.h>
#include <libsigrok/libsigrok.h>
#include <glib.h>

#include "pv/session/sigsession.h"

#include "pv/core/filterprocessor.h"
#include "pv/core/decodetaskmanager.h"
#include "pv/core/datafeedparser.h"
#include "pv/core/documentregistry.h"
#include "pv/core/capturemanager.h"
#include "pv/core/measurecalculator.h"  // Task C1.5: MeasureCalculator::compute
#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/cache/disk_cache_config.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/stack/mathstack.h"
#include "pv/data/document/sessionsnapshot.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/stack/spectrumstack.h"
#include "pv/interface/events.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

#include "pv/config/appconfig.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/utility/path.h"

// Upstream libsigrok 0.6.0 is now the sole libsigrok (fork + bridge removed).
// All ds_* fork APIs are replaced by sr_* upstream APIs.

namespace pv {

// --- Dispatch helpers (forward to SessionStateContext) ---
// These were migrated to SessionStateContext in modernize-core-layer-radical
// phase 1. SigSession retains the methods for backward compat with View/API
// callers, but just forwards to _state.

void SigSession::data_updated() { _state->data_updated(); }
void SigSession::set_receive_data_len(quint64 len) { _state->set_receive_data_len(len); }
void SigSession::receive_header() { _state->receive_header(); }
void SigSession::cur_snap_samplerate_changed() { _state->cur_snap_samplerate_changed(); }
void SigSession::frame_began() { _state->frame_began(); }
void SigSession::frame_ended() { _state->frame_ended(); }
void SigSession::update_capture() { _state->update_capture(); }
void SigSession::repeat_hold(int percent) { _state->repeat_hold(percent); }
void SigSession::receive_trigger(quint64 trigger_pos) { _state->receive_trigger(trigger_pos); }
void SigSession::show_wait_trigger() { _state->show_wait_trigger(); }
void SigSession::signals_changed() { _state->signals_changed(); }
void SigSession::session_error() { _state->session_error(); }
void SigSession::delay_prop_msg(QString strMsg) { _state->delay_prop_msg(strMsg); }

// _empty_decoder_stacks static member removed: SessionStateContext now hosts
// its own file-static _empty_decoder_stacks (see sessionstatecontext.cpp) and
// exposes it via get_decoder_stacks(). SigSession::get_decoder_stacks forwards.

SigSession::SigSession() {
  _decoder_pannel = nullptr;

  // SessionStateContext owns all shared mutable state (mutexes, signal models,
  // device agent, view/capture data, atomic flags, trigger config, etc.).
  // Its constructor initializes _sampling_mutex/_data_mutex (via make_unique),
  // _data_list (with 2 SessionData entries), _view_data/_capture_data (both
  // pointing to _data_list[0]), and all bool/atomic/numeric fields with their
  // default values.
  _state = std::make_unique<core::SessionStateContext>();

  // EventBus must be constructed before add_event_listener(this), since
  // add_event_listener forwards to _event_bus. All typed event dispatch goes
  // through broadcast<T>() / broadcast_sync<T>() / broadcast_async<T>().
  _event_bus = std::make_unique<core::EventBus>();
  _state->set_event_bus(_event_bus.get());
  // Register event handlers via subscribe<T>() (replaces IEventListener).
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::DeviceOptionsUpdated>(
          [this](const interface::DeviceOptionsUpdated &) { on_device_options_updated(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::TrigNextCollect>(
          [this](const interface::TrigNextCollect &) { on_trig_next_collect(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::RevEndPacket>(
          [this](const interface::RevEndPacket &) { on_rev_end_packet(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::CopyToDocDone>(
          [this](const interface::CopyToDocDone &) { on_copy_to_doc_done(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::DeviceSpeedNotMatch>(
          [this](const interface::DeviceSpeedNotMatch &) { on_device_speed_not_match(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::SessionStopped>(
          [this](const interface::SessionStopped &) { on_session_stopped_event(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::DecodeDone>(
          [this](const interface::DecodeDone &) { on_decode_done_event(); }));
  _event_subscriptions.push_back(
      _event_bus->subscribe<interface::EndCollectWorkPrev>(
          [this](const interface::EndCollectWorkPrev &) { on_end_collect_work_prev(); }));

  // Managers are constructed after _event_bus (they hold a raw pointer to it)
  // and after _state (they hold a raw pointer to it). FilterProcessor accesses
  // _state->view_data(), which is already initialized by SessionStateContext's
  // constructor.
  _filter_processor = std::make_unique<core::FilterProcessor>(_event_bus.get(),
                                                              _state.get(),
                                                              _state.get());
  _decode_task_manager = std::make_unique<core::DecodeTaskManager>(
      _event_bus.get(), _state.get(), _state.get());
  _data_feed_parser = std::make_unique<core::DataFeedParser>(_event_bus.get(),
                                                              _state.get(),
                                                              _state.get());
  _document_registry = std::make_unique<core::DocumentRegistry>(
      _event_bus.get(), _state.get(), _state.get());
  // CaptureManager owns the capture lifecycle + DsTimer instances + the
  // _is_instant / _clt_mode / _data_lock / _repeat_intvl / _dso_packet_count
  // / _disk_cache_config state. Constructed after _document_registry because
  // action_start_capture calls _document_registry->acquire_capture_owner().
  _capture_manager = std::make_unique<core::CaptureManager>(_event_bus.get(),
                                                             _state.get(),
                                                             _state.get());

  // Inject manager back-pointers into _state so cross-manager helpers
  // (decode_traces / attach_data_to_signal / sync_trigger_to_libsigrok /
  // clear_all_decode_task2 / etc.) can dispatch to the right manager.
  _state->set_capture_manager(_capture_manager.get());
  _state->set_decode_task_manager(_decode_task_manager.get());
  _state->set_data_feed_parser(_data_feed_parser.get());
  _state->set_document_registry(_document_registry.get());
  _state->set_filter_processor(_filter_processor.get());

  // DataFeedParser needs typed access to CaptureManager / DecodeTaskManager
  // for state queries. These are injected directly (not via ISessionCoordination,
  // which stays concrete-free) so the parser no longer pulls concrete manager
  // types through the coordination interface.
  _data_feed_parser->set_managers(_capture_manager.get(), _decode_task_manager.get());

  _state->device_agent().set_callback(this);
  // Wire the datafeed callback so DeviceAgent registers it with sr_session
  // when open_by_handle creates the session. The callback trampoline lives
  // on DataFeedParser (static method); user_data is the parser instance.
  _state->device_agent().set_datafeed_callback(
      &core::DataFeedParser::data_feed_callback_ex,
      _data_feed_parser.get());
}

SigSession::SigSession(SigSession &o) { (void)o; }

SigSession::~SigSession() {
  // Join any background file import thread before destroying state.
  // The import thread accesses the sdi and sr_session through DeviceAgent.
  wait_for_import_complete_();

  // A3 fix: ensure Close() has been called so background threads (decode/copy/
  // glitch_filter/signal_invert) are joined before we destroy _state.
  // Close() is idempotent (_bClose guard), so calling it here is safe even
  // if already called via uninit().
  Close();

  // Stop the reconnect watchdog timer (if active) before _event_bus is
  // torn down. QTimer has nullptr parent (SigSession is NOT a QObject), so
  // unique_ptr manages its lifetime. stop() ensures no in-flight timer
  // events race teardown; reset() deletes the QTimer.
  if (reconnect_timer_) {
    reconnect_timer_->stop();
    reconnect_timer_.reset();
  }

  // Subscriptions auto-unsubscribe via RAII (vector<Subscription> destructor).

  // _state destructor clears _data_list entries. Managers (unique_ptrs) are
  // destroyed before _state due to reverse declaration order in sigsession.h,
  // so manager back-pointers in _state are already dangling-but-unused by the
  // time _state is destroyed.
}

// libsigrok log callback: forward sr_err/sr_warn/sr_info/sr_dbg into PXView's
// xlog system so driver-internal failures (e.g. fx2lafw_dev_open libusb errors,
// firmware version mismatch, interface claim failures) are visible in PXView.log.
// Without this, sr_err output goes to stderr and is invisible in a GUI app,
// leaving only "sr_dev_open failed" with no root cause.
static int sigrok_log_callback(void *cb_data, int loglevel,
                               const char *format, va_list args)
{
  (void)cb_data;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), format, args);
  // Strip trailing newline added by sr_log_v_printf to keep xlog format clean.
  size_t n = strlen(buf);
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = 0;

  // 过滤 libsigrok hwdriver.c 中 "Option 'xxx' not available" 的 sr_err 噪音。
  // 上游 sr_config_get/sr_config_set 在 key 不被设备支持时返回 SR_ERR_ARG 并
  // 打印此 sr_err，属于正常情况（PXView 的 get_config/set_config 已静默处理
  // SR_ERR_ARG），但 libsigrok 内部的 sr_err 仍会输出到日志。降级为 debug。
  if (loglevel == SR_LOG_ERR && strstr(buf, "not available for this device instance")) {
    pxv_dbg("sr: %s", buf);
    return 0;
  }

  // 过滤 asix-omega-rtm-cli 驱动扫描时的外部进程执行失败噪音
  // （该驱动尝试执行 omegartmcli 外部进程，不存在时正常失败）
  if (loglevel == SR_LOG_ERR && strstr(buf, "Cannot execute RTM CLI process")) {
    pxv_dbg("sr: %s", buf);
    return 0;
  }

  switch (loglevel) {
    case SR_LOG_ERR:
      pxv_err("sr: %s", buf);
      break;
    case SR_LOG_WARN:
      pxv_warn("sr: %s", buf);
      break;
    case SR_LOG_INFO:
      pxv_info("sr: %s", buf);
      break;
    case SR_LOG_DBG:
    case SR_LOG_SPEW:
      pxv_dbg("sr: %s", buf);
      break;
    default:
      break;
  }
  return 0;
}

// libusb windows hotplug backend log callback — routes libusb hotplug debug
// messages (WM_DEVICECHANGE, device matching, usbi_hotplug_notification, etc.)
// into PXView's xlog so they're visible in PXView.log. Without this, the
// messages go to OutputDebugStringW/stderr which are invisible in MSYS.
// level: 0=info, 1=warn, 2=err. msg is fully formatted with trailing newline.
// Windows-only: the windows_hotplug_set_log_cb symbol exists only in the
// libusb event-abstraction-v4 fork's windows_hotplug.c. On Linux/macOS with
// system libusb, hotplug uses native backends (udev/IOKit) that log via the
// standard libusb_set_log_cb path — no separate hotplug log callback needed.
#ifdef _WIN32
extern "C" {
using windows_hotplug_log_cb_t = void (*)(int level, const char *msg);
void windows_hotplug_set_log_cb(windows_hotplug_log_cb_t cb);
}
extern "C" void pxv_hotplug_log_cb(int level, const char *msg)
{
  if (!msg)
    return;
  // Suppress info-level hotplug messages (initial scan, polling, etc.) to
  // keep PXView.log clean. Only forward warnings and errors.
  if (level == 0)
    return;
  // Strip trailing newline (xlog adds its own).
  char buf[600];
  size_t n = strlen(msg);
  if (n >= sizeof(buf))
    n = sizeof(buf) - 1;
  memcpy(buf, msg, n);
  buf[n] = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = 0;
  switch (level) {
    case 2: pxv_err("libusb-hotplug: %s", buf); break;
    case 1: pxv_warn("libusb-hotplug: %s", buf); break;
    default: break;
  }
}
#endif

// libusb global log callback — routes ALL libusb log messages (usbi_dbg,
// usbi_err, usbi_warn, usbi_info) into PXView's xlog. This covers the entire
// libusb core (transfer handling, device enumeration, etc.), not just the
// hotplug module. Without this, libusb debug output goes to
// OutputDebugStringW/stderr which are invisible in MSYS.
// Maps libusb_log_level to pxv_* severity.
extern "C" void pxv_libusb_log_cb(libusb_context *ctx,
                                   enum libusb_log_level level,
                                   const char *str)
{
  (void)ctx;
  if (!str)
    return;
  // Strip trailing newline (xlog adds its own).
  char buf[700];
  size_t n = strlen(str);
  if (n >= sizeof(buf))
    n = sizeof(buf) - 1;
  memcpy(buf, str, n);
  buf[n] = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    buf[--n] = 0;
  switch (level) {
    case LIBUSB_LOG_LEVEL_ERROR:
      pxv_err("libusb: %s", buf);
      break;
    case LIBUSB_LOG_LEVEL_WARNING:
      pxv_warn("libusb: %s", buf);
      break;
    case LIBUSB_LOG_LEVEL_INFO:
      pxv_info("libusb: %s", buf);
      break;
    default:
      pxv_info("libusb-dbg: %s", buf);
      break;
  }
}

bool SigSession::init() {
  // Upstream libsigrok 0.6.0 initialization (sole libsigrok after fork removal).
  // sr_init creates the sr_context which holds the libusb_context, driver list,
  // and resource hooks. The datafeed callback is registered per-session in
  // start_capture via sr_session_datafeed_callback_add.
  if (sr_init(&_sr_ctx) != SR_OK) {
    pxv_err("PXView run ERROR: libsigrok init failed.");
    return false;
  }

  // Forward libsigrok internal logs (sr_err/sr_warn/sr_info/sr_dbg) into
  // PXView's xlog so driver failures are observable in PXView.log.
  // 临时调到 SR_LOG_INFO 诊断 pxlogic 采集无数据问题:
  // 需要 usb_wr_reg/transfer/receive_transfer 的 sr_info 日志。
  // 修复后改回 SR_LOG_WARN 过滤 sr_dbg/sr_info 噪音。
  sr_log_callback_set(sigrok_log_callback, nullptr);
  sr_log_loglevel_set(SR_LOG_INFO);

  // Diagnostic: log every firmware search path libsigrok will consult, so
  // "Failed to locate 'fx2lafw-cypress-fx2.fw'" can be cross-checked against
  // this list. PulseView finds the same file in <appdir>/share/sigrok-firmware,
  // so the question is whether g_get_system_data_dirs() returns that path.
  GSList *fw_paths = sr_resourcepaths_get(SR_RESOURCE_FIRMWARE);
  pxv_info("libsigrok firmware search paths:");
  for (GSList *p = fw_paths; p; p = p->next) {
    pxv_info("  -> %s", p->data ? (const char *)p->data : "(nullptr)");
  }
  g_slist_free_full(fw_paths, g_free);

  pxv_info("libsigrok initialized (upstream 0.6.0, sole library)");

  // Register the libusb global log callback so ALL libusb log messages
  // (usbi_dbg/usbi_err/usbi_warn/usbi_info) are routed into PXView.log via
  // pxv_info/pxv_warn/pxv_err. This covers the entire libusb core (transfer
  // handling, device enumeration, hotplug dispatch), not just the hotplug
  // module. Without this, libusb debug output goes to OutputDebugStringW
  // which is invisible in MSYS. Must be set before sr_listen_hotplug so even
  // initial-scan logs are captured.
  // LIBUSB_LOG_LEVEL_NONE: disable libusb core logging (hotplug backend
  // still logs via windows_hotplug_set_log_cb). Change to
  // LIBUSB_LOG_LEVEL_INFO/WARNING for diagnostics.
  libusb_set_log_cb(nullptr, pxv_libusb_log_cb, LIBUSB_LOG_CB_GLOBAL);
  libusb_set_debug(nullptr, LIBUSB_LOG_LEVEL_NONE);

  // 首次扫描所有驱动，缓存到 DeviceAgent。后续 get_device_list 复用缓存，
  // 避免在设备 dev_open 后重复 sr_driver_scan 导致 LIBUSB_ERROR_ACCESS。
  refresh_device_list();

  // Install the libusb windows hotplug log callback so hotplug backend
  // messages (WM_DEVICECHANGE, device matching, notification dispatch) are
  // routed into PXView.log via pxv_info/pxv_warn/pxv_err. The callback is
  // set before sr_listen_hotplug so even initial-scan logs are captured.
  // Windows-only: on Linux/macOS, hotplug uses native udev/IOKit backends
  // that log via the standard libusb_set_log_cb path above.
#ifdef _WIN32
  windows_hotplug_set_log_cb(pxv_hotplug_log_cb);
#endif

  // Register USB hotplug listener (libsigrok sr_listen_hotplug).
  // The callback runs on a libsigrok internal GThread; hotplug_cb_ forwards
  // to the main thread via QMetaObject::invokeMethod(Qt::QueuedConnection)
  // so on_hotplug_event_() can safely touch Qt objects / the EventBus.
  // sr_listen_hotplug returns SR_OK on success; non-fatal if unsupported.
  if (_sr_ctx) {
    int r = sr_listen_hotplug(_sr_ctx, &SigSession::hotplug_cb_, this);
    if (r == SR_OK) {
      pxv_info("Hotplug listener registered");
    } else {
      pxv_warn("Hotplug not available on this platform; manual refresh required");
    }
  }

  return true;
}

void SigSession::uninit() {
  this->Close();

  // Stop hotplug listener before tearing down sr_context. sr_close_hotplug
  // is idempotent (sr_exit internally calls it as well), so calling it here
  // ensures the callback will not fire during sr_exit teardown.
  if (_sr_ctx) {
    sr_close_hotplug(_sr_ctx);
  }

  // DeviceAgent owns sr_session; it is destroyed in release()/destructor.
  // Just tear down the sr_context here.
  if (_sr_ctx) {
    sr_exit(_sr_ctx);
    _sr_ctx = nullptr;
  }
}

bool SigSession::set_default_device() {
  assert(!_state->is_saving());

  if (_state->is_working()) {
    pxv_info("SigSession::set_default_device()，The current device is working, "
             "now to stop it.");
    pxv_info("SigSession::set_default_device(), stop capture");
    stop_capture();
  }

  // Use the device list to pick the best device.
  int count = 0;
  int actived_index = -1;
  struct ds_device_base_info *array = get_device_list(count, actived_index);
  if (count < 1 || array == nullptr) {
    pxv_err("Error! Device list is empty, can't set default device.");
    if (array)
      free(array);
    return false;
  }

  // Try to find the last-used device by matching driver name + connection ID.
  // This is more stable than picking the last scanned device (USB scan order
  // is not guaranteed). Falls back to last scanned device if no match.
  const auto &devOpt = AppConfig::Instance().deviceOptions;

  // Determine fallback device: last scanned device that is NOT an input-module
  // device with empty channels. Input-module devices (VCD, CSV, binary) may
  // have been left in _file_sdi from a previous import that was released.
  // Their sdi may have NULL channels (e.g., VCD whose header was never parsed,
  // or the sdi was freed and recreated). Selecting such a device causes
  // init_signals() to see channel_count=0, leaving the UI in a broken state.
  ds_device_handle dev_handle = 0;
  for (int i = count - 1; i >= 0; i--) {
    ds_device_handle h = array[i].handle;
    struct sr_dev_inst *sdi = _state->device_agent().find_sdi_by_handle(h);
    if (!sdi)
      continue;
    struct sr_dev_driver *drv = sr_dev_inst_driver_get(sdi);
    if (!drv) {
      // Input-module device — check if it has channels.
      GSList *chans = sr_dev_inst_channels_get(sdi);
      if (!chans) {
        pxv_info("set_default_device: skipping input-module device "
                 "with no channels (handle=%llu)",
                 (unsigned long long)h);
        continue;
      }
    }
    dev_handle = h;
    break;
  }
  if (!dev_handle)
    dev_handle = (array + count - 1)->handle; // ultimate fallback

  if (!devOpt.lastDeviceDriver.isEmpty()) {
    bool found = false;
    for (int i = 0; i < count; i++) {
      ds_device_handle h = array[i].handle;
      struct sr_dev_inst *sdi = _state->device_agent().find_sdi_by_handle(h);
      if (!sdi)
        continue;
      struct sr_dev_driver *drv = sr_dev_inst_driver_get(sdi);
      if (!drv || !drv->name)
        continue;
      QString driver_name = QString::fromLocal8Bit(drv->name);
      if (driver_name != devOpt.lastDeviceDriver)
        continue;

      // Driver name matches. If we also have a connection ID, match it too
      // (distinguishes multiple devices of the same model). If no connId
      // stored (old config), first match by driver name is fine.
      if (!devOpt.lastDeviceConnId.isEmpty()) {
        const char *cid = sr_dev_inst_connid_get(sdi);
        if (cid && devOpt.lastDeviceConnId == QString::fromLocal8Bit(cid)) {
          dev_handle = h;
          found = true;
          pxv_info("set_default_device: matched last device by driver=%s connId=%s",
                   drv->name, cid);
          break;
        }
      } else {
        dev_handle = h;
        found = true;
        pxv_info("set_default_device: matched last device by driver=%s (no connId)",
                 drv->name);
        break;
      }
    }
    if (!found) {
      pxv_info("set_default_device: last device driver '%s' not found, "
               "using last scanned device",
               devOpt.lastDeviceDriver.toUtf8().constData());
    }
  }

  free(array);

  if (set_device(dev_handle)) {
    return true;
  }
  return false;
}

bool SigSession::set_device(ds_device_handle dev_handle) {
  assert(!_state->is_saving());
  assert(!_state->is_working());
  assert(_event_bus && _event_bus->has_subscribers());

  // If a background file import (import_file Steps 5-7) is still running,
  // join it before releasing the current device. The import thread accesses
  // the sdi (via sr_input_send) that DeviceAgent::release() would free.
  wait_for_import_complete_();

  // modernize-core-layer-radical Task 11: pre-broadcast synchronously so
  // MainWindow can close modal dialogs / hide calibration / delete protocols
  // / reload the view BEFORE the old device is released below.
  // Caller (set_device) is on the main thread (user-initiated action).
  // Plan B Phase 1: broadcast_sync → broadcast_async.
  _event_bus->broadcast_async<interface::CurrentDeviceChangePrev>({});
  // Release the old device.
  _state->device_agent().release();
  _state->set_device_status(ST_INIT);

  // Open the new device via DeviceAgent (handles sr_dev_open + channel setup).
  if (!_state->device_agent().open_by_handle(dev_handle, _sr_ctx)) {
    pxv_err("Switch device error!");
    // Broadcast DeviceOpenFailed so MainWindow can show a user-facing message
    // ("Failed to open device: <reason>") instead of leaving the UI blank.
    // The old device was already released above and the new one never opened,
    // so _dev_handle is nullptr — without this event, the UI silently stays empty
    // and "_dev_handle is nullptr" warnings flood the log.
    _event_bus->broadcast_async<interface::DeviceOpenFailed>({});
    return false;
  }

  _state->device_agent().update();
  set_collect_mode(COLLECT_SINGLE);

  if (_state->device_agent().is_file()) {
    std::string dev_name = pv::path::ToUnicodePath(_state->device_agent().name());
    pxv_info("Switch to file \"%s\" done.", dev_name.c_str());
  } else
    pxv_info("Switch to device \"%s\" done.",
             _state->device_agent().name().toUtf8().data());

  clear_all_documents_decoders();

  _state->view_data()->clear();
  _state->capture_data()->clear();
  _state->set_capture_data(_state->view_data());

  // 架构修复：从 AppConfig 恢复 auto_apply 默认值。
  // 这样即使没有打开 .pxl 文件（如新建采集），auto_apply 勾选状态
  // 也能跨会话保留。per-channel 阈值随 .pxl 文件保存/恢复。
  _state->view_data()->_glitch_filter_auto_apply =
      AppConfig::Instance().deviceOptions.glitchAutoApply;
  _state->view_data()->_show_glitch_filter_overlay =
      AppConfig::Instance().deviceOptions.glitchShowOverlay;

  init_signals();

  set_cur_snap_samplerate(_state->device_agent().get_sample_rate());
  set_cur_samplelimits(_state->device_agent().get_sample_limit());

  // The current device changed.
  _event_bus->broadcast_async<interface::CurrentDeviceChanged>({});

  return true;
}

bool SigSession::set_file(QString name) {
  assert(!_state->is_saving());
  assert(!_state->is_working());

  std::string file_name = pv::path::ToUnicodePath(name);
  pxv_info("Load file: \"%s\"", file_name.c_str());

  // 架构修复：使用 sr_session_load_file_device 替代 sr_input_scan_file。
  // sr_input_scan_file 遍历 input_module_list，没有任何模块支持 zip 格式，
  // 导致 .pxl 文件加载必然失败（静默返回 false → 空白 tab）。
  // sr_session_load_file_device 内部调用 sr_session_load，通过 session_file.c
  // 解析 metadata/header 元数据，通过 session_driver.c 回放数据块。
  // libsigrok 同时支持两种格式：
  //   - upstream sigrok: version/metadata + data-N (打包格式)
  //   - PXView v3: header + L-<ch>/<n> (按通道分块, session_driver 自动交织)
  struct sr_dev_inst *sdi = sr_session_load_file_device(_sr_ctx, file_name.c_str());
  if (!sdi) {
    pxv_err("Load file error: sr_session_load_file_device failed for \"%s\"",
            file_name.c_str());
    return false;
  }

  // Register the file-loaded device with DeviceAgent and get its handle.
  ds_device_handle dev_handle =
      _state->device_agent().set_file_device(sdi, name);
  if (dev_handle == NULL_HANDLE) {
    pxv_err("Load file error: set_file_device returned NULL_HANDLE");
    return false;
  }

  // 架构修复：直接选中文件设备，不调用 set_default_device()。
  // set_default_device() 会匹配 lastDeviceDriver（通常是 demo），
  // 导致文件设备被忽略，选中了 demo 设备而非 virtual-session。
  if (!set_device(dev_handle)) {
    pxv_err("Load file error: set_device failed for file device");
    return false;
  }

  // Restore the original capture timestamp from the .pxl header.
  // session_file.c parses "trigger time" and stores it via
  // SR_CONF_SESSION_TIME. Without this, the session time defaults
  // to the current time, making default filenames (e.g. "-yyMMdd-hhmmss")
  // incorrect for file-loaded data.
  {
    int64_t file_time_ms = 0;
    if (_state->device_agent().get_config_int64(SR_CONF_SESSION_TIME, file_time_ms)
        && file_time_ms > 0) {
      _state->set_session_time(QDateTime::fromMSecsSinceEpoch(file_time_ms));
      _state->set_trig_time(QDateTime::fromMSecsSinceEpoch(file_time_ms));
    }
  }

  // 文件设备选中后，触发采集来回放数据。
  // exec_capture() -> device_agent.start() -> sr_session_start/run()
  // -> dev_acquisition_start -> stream_session_data/stream_pxv_session_data
  // 数据通过 datafeed 回调进入 DataFeedParser::feed_in_logic -> LogicSnapshot。
  if (!start_capture(false, _state->document_registry()->get_active_document())) {
    pxv_err("Load file error: start_capture failed for file device");
    return false;
  }

  return true;
}

bool SigSession::import_file(QString name) {
  assert(!_state->is_saving());
  assert(!_state->is_working());

  std::string file_name = pv::path::ToUnicodePath(name);
  pxv_info("Import file: \"%s\"", file_name.c_str());

  // Step 1: Detect format using sr_input_scan_file (aligned with PulseView).
  // sr_input_scan_file reads the file header and finds the best matching
  // input module. It creates a temporary sr_input with the header data
  // buffered in input->buf, but receive() has NOT been called yet.
  const struct sr_input *tmp_input = nullptr;
  int ret = sr_input_scan_file(file_name.c_str(), &tmp_input);
  if (ret != SR_OK || !tmp_input) {
    pxv_err("Import file error: sr_input_scan_file failed for \"%s\"",
            file_name.c_str());
    return false;
  }

  // Extract the module ID, then free the temporary input instance.
  // We create a fresh input below so we can control exactly when data
  // is fed to the module (the temp input has up to 4MB of pre-read data
  // in its buffer which complicates the feed sequence).
  const struct sr_input_module *imod = sr_input_module_get(tmp_input);
  const char *mod_id = imod ? sr_input_id_get(imod) : nullptr;
  if (!mod_id) {
    pxv_err("Import file error: cannot determine input module ID");
    sr_input_free(tmp_input);
    return false;
  }
  std::string mod_id_str(mod_id);
  sr_input_free(tmp_input);

  // Step 2: Create a fresh input instance.
  // sr_input_new() calls the module's init() which creates the sdi.
  // For header-based formats (VCD, CSV, Saleae): sdi_ready is FALSE
  //   until enough header data is fed via sr_input_send().
  // For headerless formats (binary): sdi_ready is TRUE immediately.
  const struct sr_input_module *mod = sr_input_find(mod_id_str.c_str());
  if (!mod) {
    pxv_err("Import file error: sr_input_find failed for \"%s\"",
            mod_id_str.c_str());
    return false;
  }

  // For the binary input module, auto-detect channel count and sample rate
  // from the current device. Raw binary files have no header, so the module
  // defaults to 8 channels @ 0 Hz — which is almost never correct.
  // PulseView solves this by showing an options dialog before import; we
  // auto-detect from the current device context (if a device is open with
  // 16/32 channels, the binary file was likely exported from that device).
  GHashTable *input_opts = nullptr;
  if (mod_id_str == "binary" && _state->device_agent().have_instance()) {
    int cur_channels = 0;
    for (auto m : _state->signal_models()) {
      if (m->type() == SR_CHANNEL_LOGIC && m->enabled())
        cur_channels++;
    }
    if (cur_channels < 1)
      cur_channels = 8;  // fallback

    uint64_t cur_rate = 0;
    cur_rate = (uint64_t)_state->device_agent().get_sample_rate();
    if (cur_rate == 0)
      cur_rate = 1000000;  // fallback 1 MHz

    pxv_info("Import file: binary module — auto-detect channels=%d, "
             "samplerate=%llu from current device",
             cur_channels, (unsigned long long)cur_rate);

    input_opts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)g_variant_unref);
    g_hash_table_insert(input_opts, g_strdup("numchannels"),
                        g_variant_ref_sink(g_variant_new_int32(cur_channels)));
    g_hash_table_insert(input_opts, g_strdup("samplerate"),
                        g_variant_ref_sink(g_variant_new_uint64(cur_rate)));
  }

  const struct sr_input *input = sr_input_new(mod, input_opts);
  if (input_opts)
    g_hash_table_destroy(input_opts);
  if (!input) {
    pxv_err("Import file error: sr_input_new failed for \"%s\"",
            mod_id_str.c_str());
    return false;
  }

  // Step 3: Open file and feed data until the sdi becomes ready.
  // For header-based formats, the receive() function parses the header
  // (creates channels, sets sdi_ready=TRUE) WITHOUT sending any
  // datafeed packets — so this is safe to do before the session is
  // set up. After the header is parsed, remaining sample data stays
  // in input->buf unprocessed.
  // For headerless formats (binary), sdi_ready is TRUE immediately,
  // so the loop exits without feeding any data.
  // Use QFile for cross-platform Unicode path handling (g_fopen needs
  // glib/gstdio.h which is not reliably available on all platforms).
  // Allocated on the heap (shared_ptr) so the background thread can safely
  // outlive this stack frame — the lambda captures the shared_ptr by value.
  auto file = std::make_shared<QFile>(name);
  if (!file->open(QIODevice::ReadOnly)) {
    pxv_err("Import file error: cannot open file \"%s\"",
            file_name.c_str());
    sr_input_free(input);
    return false;
  }

  GString *chunk = g_string_sized_new(65536);
  struct sr_dev_inst *sdi = sr_input_dev_inst_get(input);

  while (!sdi) {
    qint64 n = file->read(chunk->str, 65536);
    if (n <= 0) {
      // EOF — check if sdi became ready on the last chunk
      sdi = sr_input_dev_inst_get(input);
      break;
    }
    chunk->len = n;
    sr_input_send(input, chunk);
    sdi = sr_input_dev_inst_get(input);
  }

  if (!sdi) {
    pxv_err("Import file error: could not determine device instance "
            "from input module \"%s\"", mod_id_str.c_str());
    g_string_free(chunk, TRUE);
    file->close();
    sr_input_free(input);
    return false;
  }

  pxv_info("Import file: input module \"%s\" ready, sdi=%p",
           mod_id_str.c_str(), (void *)sdi);

  // Step 4: Register the input sdi with DeviceAgent and set up the
  // session (create sr_session, add device, register datafeed callback,
  // init_signals, broadcast CurrentDeviceChanged).
  // open_by_handle() handles nullptr-driver sdi by skipping sr_dev_open.
  ds_device_handle dev_handle =
      _state->device_agent().set_file_device(sdi, name);
  if (dev_handle == NULL_HANDLE) {
    pxv_err("Import file error: set_file_device returned NULL_HANDLE");
    g_string_free(chunk, TRUE);
    file->close();
    // sdi was NOT registered (set_file_device failed), so sr_input_free
    // is safe here — it frees the sdi along with the input.
    sr_input_free(input);
    return false;
  }

  if (!set_device(dev_handle)) {
    pxv_err("Import file error: set_device failed for input device");
    g_string_free(chunk, TRUE);
    file->close();
    // The sdi was registered with DeviceAgent via set_file_device(),
    // so ownership has been transferred. Use sr_input_release_sdi() to
    // avoid freeing the sdi, then remove_device() to clean it up from
    // _file_sdi and free it properly via sr_dev_inst_free().
    sr_input_release_sdi(input);
    _state->device_agent().remove_device(dev_handle);
    return false;
  }

  // ========================================================================
  // Steps 5-7: Feed the remaining file data and finalise the import.
  //
  // ARCHITECTURE CHANGE: These steps now run on a background std::async
  // thread instead of the GUI main thread. The sr_input_send() loop reads
  // the entire file (potentially tens of MB for VCD/CSV/binary) and calls
  // the datafeed callback (DataFeedParser::data_feed_in) which appends
  // samples to the snapshot. Running this on the main thread blocked the
  // GUI for the entire duration; moving it to a background thread keeps
  // the UI responsive.
  //
  // Thread safety: DataFeedParser is already designed for non-main-thread
  // access — during normal hardware captures, the datafeed callback fires
  // on libsigrok's data-feed thread. The same locking (_state->data_mutex()
  // in DataFeedParser::data_feed_in) protects concurrent access from the
  // view's paint thread. The EventBus::broadcast_async calls made inside
  // DataFeedParser (DataUpdated, RevEndPacket) are safe from any thread.
  //
  // Lifecycle: _import_in_progress is set atomically before launching and
  // cleared by the background thread on exit. wait_for_import_complete_()
  // is called in set_device(), close_file(), and ~SigSession() to join the
  // thread before the device/sdi it references is released.
  // ========================================================================
  _import_in_progress.store(true, std::memory_order_release);

  // Capture the file name for error reporting and logging.
  std::string import_file_name = file_name;

  _import_future = std::async(std::launch::async,
      [this, input, chunk, file, import_file_name]() mutable {
        // Step 5: Process any remaining data in the input buffer.
        // After header parsing, sample data from the last chunk stays in
        // input->buf. Feeding an empty GString triggers receive() which
        // processes this data (now that the session is set up, datafeed
        // packets are properly routed to DataFeedParser).
        // For headerless formats (binary), input->buf is empty, so this
        // is a no-op except for sending the DF header packet.
        {
          GString *empty = g_string_new("");
          sr_input_send(input, empty);
          g_string_free(empty, TRUE);
        }

        // Step 6: Feed the rest of the file in chunks.
        // sr_input_send() calls the module's receive() which processes the
        // data and calls sr_session_send() — this directly invokes the
        // datafeed callback (DataFeedParser::data_feed_in) which appends
        // samples to the snapshot. No sr_session_run() is needed.
        while (true) {
          qint64 n = file->read(chunk->str, 65536);
          if (n <= 0)
            break;
          chunk->len = n;
          sr_input_send(input, chunk);
        }

        // Step 7: Signal end-of-data and release the input.
        // sr_input_end() flushes any buffered samples and sends SR_DF_END,
        // which triggers DataFeedParser's SR_DF_END handler: calls
        // capture_ended() on all snapshot types, sets device status to
        // ST_STOPPED, and broadcasts RevEndPacket (for LOGIC mode) which
        // swaps the capture/view buffer and kicks off decoders.
        //
        // Use sr_input_release_sdi() instead of sr_input_free() because the
        // sdi has been registered with DeviceAgent (via set_file_device +
        // open_by_handle). sr_input_free() would call sr_dev_inst_free() on
        // the sdi, causing a use-after-free when the async CurrentDeviceChanged
        // event later triggers reset_all_view() → DevMode::set_device() →
        // get_device_mode_list(), which accesses _di (the same sdi pointer).
        // sr_input_release_sdi() detaches the sdi from the input so it is NOT
        // freed; DeviceAgent takes ownership and will free it via
        // sr_dev_inst_free() in release() when the device is closed.
        sr_input_end(input);
        sr_input_release_sdi(input);
        g_string_free(chunk, TRUE);
        file->close();

        pxv_info("Import file complete: \"%s\"", import_file_name.c_str());

        _import_in_progress.store(false, std::memory_order_release);
      });

  // Return immediately — the background thread feeds data and posts
  // DataUpdated events to the main thread as samples arrive, so the
  // view will progressively show the imported waveform. The
  // CurrentDeviceChanged event (already broadcast by set_device in
  // Step 4) will fire on the main thread and handle UI setup (signal
  // rebuild, config loading). For input-module devices it does NOT
  // call start_capture() (see event_dispatcher.cpp:306).
  pxv_info("Import file: background thread started for \"%s\"",
           file_name.c_str());

  return true;
}

void SigSession::wait_for_import_complete_() {
  // Called on the main thread before releasing the current device
  // (set_device, close_file, destructor). If a background import is still
  // running, this blocks until it finishes. The import thread's
  // sr_input_send loop typically completes within seconds for most files;
  // the wait is bounded by file I/O speed, not network/USB latency.
  if (_import_in_progress.load(std::memory_order_acquire)) {
    pxv_info("Waiting for background file import to complete before "
             "switching/releasing device...");
  }
  if (_import_future.valid()) {
    _import_future.wait();
  }
}

void SigSession::close_file(unsigned long long dev_handle) {
  if (!dev_handle) {
    pxv_warn("%s", "SigSession::close_file: dev_handle is nullptr");
    return;
  }

  // Join any background file import before removing the device.
  wait_for_import_complete_();

  if (dev_handle == _state->device_agent().handle() && _state->is_working()) {
    pxv_err("The virtual device is running, can't remove it.");
    return;
  }
  bool isCurrent = dev_handle == _state->device_agent().handle();

  // Remove the device from DeviceAgent's tracked list.
  _state->device_agent().remove_device(dev_handle);

  if (isCurrent)
    set_default_device();
}

bool SigSession::have_hardware_data() {
  if (_state->device_agent().have_instance() && _state->device_agent().is_hardware()) {
    Snapshot *data = get_signal_snapshot();
    return data->have_data();
  }
  return false;
}

struct ds_device_base_info *SigSession::get_device_list(int &out_count,
                                                        int &actived_index) {
  out_count = 0;
  actived_index = -1;

  if (!_sr_ctx) {
    return nullptr;
  }

  // 复用 DeviceAgent 已缓存的扫描结果，避免重复 sr_driver_scan。
  // 重复扫描会导致已 dev_open 的 USB 设备被 fx2lafw scan 再次 libusb_open，
  // 触发 LIBUSB_ERROR_ACCESS（Windows 上 interface 已被 claim）。
  // 需要重新扫描硬件（如热插拔）时调用 refresh_device_list()。
  std::vector<struct sr_dev_inst *> all_sdi = _state->device_agent().scanned_sdi();

  // Also include any file-loaded devices tracked by DeviceAgent.
  auto &file_devs = _state->device_agent().file_devices();
  for (auto sdi : file_devs) {
    if (sdi)
      all_sdi.push_back(sdi);
  }

  if (all_sdi.empty()) {
    return nullptr;
  }

  // Allocate (count + 1) entries; last entry is a sentinel with handle=0.
  int count = (int)all_sdi.size();
  struct ds_device_base_info *array = (struct ds_device_base_info *)
      calloc(count + 1, sizeof(struct ds_device_base_info));
  if (!array) {
    return nullptr;
  }

  // Fill entries. Handle = index+1 (0 is reserved for NULL_HANDLE sentinel).
  for (int i = 0; i < count; i++) {
    struct ds_device_base_info *entry = &array[i];
    entry->handle = static_cast<ds_device_handle>(i + 1);

    // Build display name from vendor/model/conn fields.
    const char *vendor = sr_dev_inst_vendor_get(all_sdi[i]);
    const char *model = sr_dev_inst_model_get(all_sdi[i]);
    const char *conn = sr_dev_inst_connid_get(all_sdi[i]);

    char name_buf[150] = {0};
    if (vendor && model) {
      snprintf(name_buf, sizeof(name_buf), "%s %s", vendor, model);
    } else if (model) {
      snprintf(name_buf, sizeof(name_buf), "%s", model);
    } else if (conn) {
      snprintf(name_buf, sizeof(name_buf), "%s", conn);
    } else {
      snprintf(name_buf, sizeof(name_buf), "device-%d", i);
    }
    strncpy(entry->name, name_buf, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
  }

  // Sentinel.
  array[count].handle = 0;
  array[count].name[0] = '\0';

  out_count = count;
  // actived_index: track via DeviceAgent's current handle.
  ds_device_handle cur = _state->device_agent().handle();
  actived_index = (cur > 0 && cur <= (ds_device_handle)count) ? (int)(cur - 1) : -1;

  return array;
}

void SigSession::refresh_device_list() {
  if (!_sr_ctx) {
    return;
  }

  struct sr_dev_driver **drivers = sr_driver_list(_sr_ctx);
  if (!drivers) {
    pxv_err("refresh_device_list: sr_driver_list returned nullptr");
    return;
  }

  // Scan all upstream drivers via sr_driver_list + sr_driver_scan.
  std::vector<struct sr_dev_inst *> all_sdi;
  int drv_count = 0;
  int init_fail_count = 0;
  int scan_found_count = 0;
  for (int i = 0; drivers[i]; i++) {
    struct sr_dev_driver *drv = drivers[i];
    if (!drv)
      continue;
    drv_count++;
    // Initialize driver on first use.
    if (sr_driver_init(_sr_ctx, drv) != SR_OK) {
      init_fail_count++;
      pxv_dbg("refresh_device_list: sr_driver_init failed for '%s'",
              drv->name ? drv->name : "(nullptr)");
      continue;
    }
    GSList *devs = sr_driver_scan(drv, nullptr);
    int found = g_slist_length(devs);
    if (found > 0) {
      scan_found_count += found;
      pxv_info("refresh_device_list: driver '%s' found %d device(s)",
               drv->name ? drv->name : "(nullptr)", found);
    }
    for (GSList *l = devs; l; l = l->next) {
      struct sr_dev_inst *sdi = (struct sr_dev_inst *)l->data;
      if (sdi)
        all_sdi.push_back(sdi);
    }
    // Note: sr_driver_scan returns a list owned by the driver; do not free.
  }

  pxv_info("refresh_device_list: total %d drivers, init_fail=%d, devices found=%d",
           drv_count, init_fail_count, (int)all_sdi.size());

  _state->device_agent().set_scanned_devices(all_sdi);
}

uint64_t SigSession::cur_samplerate() {
  // samplerate for current viewport
  if (_state->device_agent().get_work_mode() == DSO)
    return _state->device_agent().get_sample_rate();
  else
    return cur_snap_samplerate();
}

uint64_t SigSession::cur_snap_samplerate() {
  // samplerate for current snapshot
  return _state->capture_data()->_cur_snap_samplerate;
}

uint64_t SigSession::cur_samplelimits() {
  return _state->capture_data()->_cur_samplelimits;
}

double SigSession::cur_sampletime() {
  return cur_samplelimits() * 1.0 / cur_samplerate();
}

double SigSession::cur_snap_sampletime() {
  return cur_samplelimits() * 1.0 / cur_snap_samplerate();
}

double SigSession::get_logic_data_view_time() {
  // Use capture_data (live buffer) during realtime refresh in double-buffer mode.
  SessionData *data = _state->view_data();
  if (_capture_manager->is_realtime_refresh() &&
      _state->capture_data() != _state->view_data())
    data = _state->capture_data();
  return data->get_logic()->get_ring_sample_count() * 1.0 /
         cur_snap_samplerate();
}

double SigSession::cur_view_time() {
  return _state->device_agent().get_time_base() * DS_CONF_DSO_HDIVS * 1.0 / SR_SEC(1);
}

void SigSession::set_cur_snap_samplerate(uint64_t samplerate) {
  if (samplerate == 0) {
    pxv_err("set_cur_snap_samplerate: samplerate=0, ignoring");
    return;
  }

  _state->capture_data()->_cur_snap_samplerate = samplerate;
  _state->capture_data()->get_logic()->set_samplerate(samplerate);
  _state->capture_data()->get_analog()->set_samplerate(samplerate);
  _state->capture_data()->get_dso()->set_samplerate(samplerate);

  int mode = _state->device_agent().get_work_mode();

  if (mode == DSO) {
    for (auto m : _state->signal_models()) {
      if (m->type() == SR_CHANNEL_DSO) {
        // TODO: verify - vfactor and vdiv replace view::DsoSignal getters.
        _state->capture_data()->get_dso()->set_measure_voltage_factor(
            (uint64_t)m->vfactor(), m->index());
        _state->capture_data()->get_dso()->set_data_scale(m->vdiv(), m->index());
      }
    }
  }

  // DecoderStack
  for (auto d : decode_traces()) {
    d->set_samplerate(samplerate);
  }

  // Math
  if (_state->math_stack())
    _state->math_stack()->set_samplerate(_state->device_agent().get_sample_rate());
  // SpectrumStack
  for (auto m : _state->spectrum_stacks()) {
    m->set_samplerate(samplerate);
  }

  cur_snap_samplerate_changed();
}

void SigSession::set_cur_samplelimits(uint64_t samplelimits) {
  if (samplelimits == 0) {
    pxv_err("set_cur_samplelimits: samplelimits=0, ignoring");
    return;
  }
  _state->capture_data()->_cur_samplelimits = samplelimits;
  // R1: symmetric to set_cur_snap_samplerate which fires
  // cur_snap_samplerate_changed(); notify capture listeners that the
  // sample limit changed.
  broadcast_async<interface::SampleLimitsChanged>({});
}

std::vector<std::shared_ptr<data::SignalModel>> &
SigSession::get_signal_models() {
return _state->signal_models();
}

// TS-2 fix: thread-safe snapshot for callers that don't hold the mutex.
std::vector<std::shared_ptr<data::SignalModel>>
SigSession::get_signal_models_snapshot() {
return _state->signal_models_snapshot();
}

void SigSession::init_signals() {
  if (_state->device_agent().have_instance() == false) {
    pxv_err("init_signals: no device instance, aborting");
    return;
  }

  std::vector<std::shared_ptr<data::SignalModel>> models;
  unsigned int logic_probe_count = 0;
  unsigned int dso_probe_count = 0;
  unsigned int analog_probe_count = 0;

  _state->capture_data()->clear();
  _state->view_data()->clear();
  set_cur_snap_samplerate(_state->device_agent().get_sample_rate());
  set_cur_samplelimits(_state->device_agent().get_sample_limit());

  // Detect what data types we will receive
  if (_state->device_agent().have_instance()) {
    for (const GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
      const sr_channel *const probe = (const sr_channel *)l->data;

      switch (probe->type) {
      case SR_CHANNEL_LOGIC:
        if (probe->enabled)
          logic_probe_count++;
        break;

      case SR_CHANNEL_DSO:
        dso_probe_count++;
        break;

      case SR_CHANNEL_ANALOG:
        if (probe->enabled)
          analog_probe_count++;
        break;
      }
    }
  }

  int mode = _state->device_agent().get_work_mode();
  int channel_count = g_slist_length((GSList *)_state->device_agent().get_channels());
  pxv_info("SigSession::init_signals() start. mode=%d, channel_count=%d", mode, channel_count);

  // Ensure at least one channel of the current work mode's type is enabled.
  // If all channels are disabled (e.g., VCD file with no enabled channels,
  // or user disabled all channels), force-enable the first matching one
  // to prevent a blank viewport with zero signal models.
  {
    bool has_enabled = false;
    for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
      sr_channel *p = (sr_channel *)l->data;
      if (!p) continue;
      if (mode == LOGIC && p->type != SR_CHANNEL_LOGIC) continue;
      if (mode == DSO && p->type != SR_CHANNEL_DSO) continue;
      if (mode == ANALOG && p->type != SR_CHANNEL_ANALOG) continue;
      if (mode == MSO && p->type == SR_CHANNEL_DSO) continue;
      if (p->enabled) { has_enabled = true; break; }
    }
    if (!has_enabled) {
      for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
        sr_channel *p = (sr_channel *)l->data;
        if (!p) continue;
        if (mode == LOGIC && p->type != SR_CHANNEL_LOGIC) continue;
        if (mode == DSO && p->type != SR_CHANNEL_DSO) continue;
        if (mode == ANALOG && p->type != SR_CHANNEL_ANALOG) continue;
        if (mode == MSO && p->type == SR_CHANNEL_DSO) continue;
        _state->device_agent().enable_probe(p, true);
        pxv_warn("init_signals: no enabled channel for mode %d, "
                 "force-enabling channel index=%d name=%s",
                 mode, p->index, p->name ? p->name : "nullptr");
        break;
      }
    }
  }

  for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
    sr_channel *probe = (sr_channel *)l->data;
    if (!probe) {
      pxv_warn("%s", "SigSession: probe is nullptr in channel loop, skipping");
      continue;
    }
    assert(probe);

    // Channel visibility by work mode:
    //   LOGIC  — only SR_CHANNEL_LOGIC (logic analyzer)
    //   DSO    — only SR_CHANNEL_DSO (oscilloscope)
    //   ANALOG — only SR_CHANNEL_ANALOG (data acquisition / logger)
    //   MSO    — SR_CHANNEL_LOGIC + SR_CHANNEL_ANALOG (mixed signal;
    //            no DSO controls — MSO analog channels are DAQ-style)
    if (mode == LOGIC && probe->type != SR_CHANNEL_LOGIC) {
      continue;
    }
    if (mode == DSO && probe->type != SR_CHANNEL_DSO) {
      continue;
    }
    if (mode == ANALOG && probe->type != SR_CHANNEL_ANALOG) {
      continue;
    }
    if (mode == MSO && probe->type == SR_CHANNEL_DSO) {
      continue;
    }

    pxv_info("init_signals probe examine: index=%d name=%s type=%d enabled=%d",
             probe->index, probe->name ? probe->name : "nullptr", probe->type, probe->enabled);

    bool should_create = false;
    int ch_type = SR_CHANNEL_LOGIC;

    switch (probe->type) {
    case SR_CHANNEL_LOGIC:
      if (probe->enabled) {
        should_create = true;
        ch_type = SR_CHANNEL_LOGIC;
      }
      break;

    case SR_CHANNEL_DSO:
      should_create = true;
      ch_type = SR_CHANNEL_DSO;
      break;

    case SR_CHANNEL_ANALOG:
      if (probe->enabled) {
        should_create = true;
        ch_type = SR_CHANNEL_ANALOG;
      }
      break;
    }

    if (should_create) {
      auto model = std::make_shared<data::SignalModel>();
      model->set_index(probe->index);
      model->set_name(probe->name ? probe->name : "");
      model->set_type(ch_type);
      model->set_enabled(probe->enabled);

      // Inject weak references so the model can write back to the
      // sr_channel struct and the DeviceAgent API. See
      // SignalModel::commit_to_device() and the enhanced setters.
      model->set_session(this);
      model->set_sr_channel(probe);

      // Read probe configuration for DSO/ANALOG channels.
      // Sources must match view::DsoSignal/AnalogSignal getters so that the
      // SignalModel mirrors what the View layer reports:
      //   - vfactor    <- SR_CONF_PROBE_FACTOR    (DsoSignal::get_factor)
      //   - hw_offset  <- SR_CONF_PROBE_HW_OFFSET (DsoSignal::get_hw_offset)
      //   - zero_offset<- SR_CONF_PROBE_OFFSET    (DsoSignal::load_settings)
      // vdiv / coupling were fork DSO keys (deleted); model defaults are used.
      // Use typed wrappers (DeviceAgent::get_probe_factor / get_probe_hw_offset
      // / get_probe_offset / get_probe_map_default) — they short-circuit on
      // non-DSL devices (is_dsl_device() guard) so we don't flood the log with
      // "Option 'probe_factor' not available" errors on demo/fx2lafw.
      if (ch_type == SR_CHANNEL_DSO ||
          ch_type == SR_CHANNEL_ANALOG) {
        uint64_t vfactor = 1;
        if (_state->device_agent().get_probe_factor(vfactor, probe)) {
          // Guard: saved waveform files may have probe factor = 0 in metadata.
          // Storing 0 in the model would later trigger assertion failures in
          // dslDial::set_factor / MathStack::default_factor.
          if (vfactor == 0) {
            pxv_warn("SigSession: vfactor==0 from driver, clamping to 1");
            vfactor = 1;
          }
          model->set_vfactor((double)vfactor);
        } else
          model->set_vfactor(1.0);

        bool map_default = true;
        _state->device_agent().get_probe_map_default(map_default, probe);
        model->set_map_default(map_default);

        // Only set model fields if the driver GET succeeded. If GET fails,
        // the model keeps its default (0.0) and does NOT push 0 back to the
        // device via set_config_uint16, which would overwrite the driver's
        // default (e.g. DSO_DEFAULT_OFFSET=128) with 0 and cause the cursor
        // to appear at the top of the screen.
        int hw_offset = 0;
        bool hw_ok = _state->device_agent().get_probe_hw_offset(hw_offset, probe);
        if (hw_ok)
          model->set_hw_offset(hw_offset);

        int zero_offset = 0;
        bool zero_ok = _state->device_agent().get_probe_offset(zero_offset, probe);
        if (zero_ok)
          model->set_zero_offset(zero_offset);
      }

      models.push_back(model);
    }
  }

  clear_signals();
  {
    std::unique_lock<std::shared_mutex> lk(_state->signal_models_mutex());
    std::vector<std::shared_ptr<data::SignalModel>>().swap(_state->signal_models());
    _state->signal_models() = models;
  }
  make_channels_view_index();

  // After recreating SignalModels, immediately set snapshot pointers from
  // the current view data. Same rationale as reload(): without this, decode
  // threads that are already running will see nullptr snapshots.
  if (_state->view_data()) {
    for (auto &m : _state->signal_models()) {
      switch (m->type()) {
      case SR_CHANNEL_LOGIC:
        m->set_snapshot(_state->view_data()->logic_shared());
        break;
      case SR_CHANNEL_ANALOG:
        m->set_snapshot(_state->view_data()->analog_shared());
        break;
      case SR_CHANNEL_DSO:
        m->set_snapshot(_state->view_data()->dso_shared());
        break;
      }
    }
  }

  spectrum_rebuild();
  lissajous_disable();
  math_disable();

  // Notify View layer to rebuild signals from the new SignalModels.
  // Without this, LogicSignals keep stale model pointers (old models were
  // deleted above) and never receive property-change notifications.
  signals_changed();

  if (_state->signal_models().empty()) {
    pxv_info("ERROR: Unable to create any channel. (models is empty)");
  } else {
    pxv_info("SigSession::init_signals() end. models.size()=%d", (int)_state->signal_models().size());
  }
}

void SigSession::reload() {
  if (_state->device_agent().have_instance() == false) {
    pxv_err("reload: no device instance, aborting");
    return;
  }

  if (_state->is_working())
    return;

  std::vector<std::shared_ptr<data::SignalModel>> models;
  int mode = _state->device_agent().get_work_mode();
  int channel_count = g_slist_length((GSList *)_state->device_agent().get_channels());
  pxv_info("SigSession::reload() start. mode=%d, channel_count=%d", mode, channel_count);

  // Ensure at least one channel of the current work mode's type is enabled.
  // Mirrors init_signals() — if all channels are disabled, force-enable the
  // first matching one to prevent a blank viewport with zero signal models.
  {
    bool has_enabled = false;
    for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
      sr_channel *p = (sr_channel *)l->data;
      if (!p) continue;
      if (mode == LOGIC && p->type != SR_CHANNEL_LOGIC) continue;
      if (mode == DSO && p->type != SR_CHANNEL_DSO) continue;
      if (mode == ANALOG && p->type != SR_CHANNEL_ANALOG) continue;
      if (mode == MSO && p->type == SR_CHANNEL_DSO) continue;
      if (p->enabled) { has_enabled = true; break; }
    }
    if (!has_enabled) {
      for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
        sr_channel *p = (sr_channel *)l->data;
        if (!p) continue;
        if (mode == LOGIC && p->type != SR_CHANNEL_LOGIC) continue;
        if (mode == DSO && p->type != SR_CHANNEL_DSO) continue;
        if (mode == ANALOG && p->type != SR_CHANNEL_ANALOG) continue;
        if (mode == MSO && p->type == SR_CHANNEL_DSO) continue;
        _state->device_agent().enable_probe(p, true);
        pxv_warn("reload: no enabled channel for mode %d, "
                 "force-enabling channel index=%d name=%s",
                 mode, p->index, p->name ? p->name : "nullptr");
        break;
      }
    }
  }

  uint64_t sr = _state->device_agent().get_sample_rate();
  uint64_t sl = _state->device_agent().get_sample_limit();
  pxv_info("[DEBUG-DSO] reload: get_sample_rate=%llu get_sample_limit=%llu", (unsigned long long)sr, (unsigned long long)sl);
  set_cur_snap_samplerate(sr);
  set_cur_samplelimits(sl);

  for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
    sr_channel *probe = (sr_channel *)l->data;
    if (!probe) {
      pxv_warn("%s", "SigSession: probe is nullptr in channel loop, skipping");
      continue;
    }
    assert(probe);

    // Channel visibility by work mode (mirrors init_signals()).
    if (mode == LOGIC && probe->type != SR_CHANNEL_LOGIC) {
      continue;
    }
    if (mode == DSO && probe->type != SR_CHANNEL_DSO) {
      continue;
    }
    if (mode == ANALOG && probe->type != SR_CHANNEL_ANALOG) {
      continue;
    }
    if (mode == MSO && probe->type == SR_CHANNEL_DSO) {
      continue;
    }

    pxv_info("reload probe examine: index=%d name=%s type=%d enabled=%d",
             probe->index, probe->name ? probe->name : "nullptr", probe->type, probe->enabled);

    bool should_create = false;
    int ch_type = SR_CHANNEL_LOGIC;

    switch (probe->type) {
    case SR_CHANNEL_LOGIC:
      if (probe->enabled) {
        should_create = true;
        ch_type = SR_CHANNEL_LOGIC;
      }
      break;

    case SR_CHANNEL_DSO:
      should_create = true;
      ch_type = SR_CHANNEL_DSO;
      break;

    case SR_CHANNEL_ANALOG:
      if (probe->enabled) {
        should_create = true;
        ch_type = SR_CHANNEL_ANALOG;
      }
      break;
    }

    if (should_create) {
      // Try to preserve settings from the existing model with the same index
      std::shared_ptr<data::SignalModel> old_model = nullptr;
      for (auto &m : _state->signal_models()) {
        if (m->index() == (int)probe->index) {
          old_model = m;
          break;
        }
      }

      auto model = std::make_shared<data::SignalModel>();
      model->set_index(probe->index);
      model->set_name(probe->name ? probe->name : "");
      model->set_type(ch_type);
      model->set_enabled(probe->enabled);

      // Inject weak references (same as init_signals) so the rebuilt model
      // can write back to sr_channel / DeviceAgent and so
      // commit_to_device() works after reload.
      model->set_session(this);
      model->set_sr_channel(probe);

      if (ch_type == SR_CHANNEL_DSO ||
          ch_type == SR_CHANNEL_ANALOG) {
        // vdiv / coupling were fork DSO keys (deleted); model defaults are used.
        // Use typed wrappers (is_dsl_device() guard) — non-DSL devices skip
        // the queries entirely, avoiding "not available" log noise on demo.
        uint64_t vfactor = 1;
        if (_state->device_agent().get_probe_factor(vfactor, probe)) {
          // Guard: saved waveform files may have probe factor = 0 in metadata.
          // Storing 0 in the model would later trigger assertion failures in
          // dslDial::set_factor / MathStack::default_factor.
          if (vfactor == 0) {
            pxv_warn("SigSession: vfactor==0 from driver, clamping to 1");
            vfactor = 1;
          }
          model->set_vfactor((double)vfactor);
        } else
          model->set_vfactor(1.0);

        bool map_default = true;
        _state->device_agent().get_probe_map_default(map_default, probe);
        model->set_map_default(map_default);

        // Only set model fields if the driver GET succeeded (same guard as
        // build_signals path above — prevents overwriting driver defaults).
        int hw_offset = 0;
        bool hw_ok = _state->device_agent().get_probe_hw_offset(hw_offset, probe);
        if (hw_ok)
          model->set_hw_offset(hw_offset);

        int zero_offset = 0;
        bool zero_ok = _state->device_agent().get_probe_offset(zero_offset, probe);
        if (zero_ok)
          model->set_zero_offset(zero_offset);
      }

      if (old_model) {
        model->set_trig_type(old_model->trig_type());
        model->set_color(old_model->color());
      }

      models.push_back(model);
    }
  }

  if (!models.empty()) {
    pxv_info("SigSession::reload() end. clear signals, models.size()=%d", (int)models.size());
    clear_signals();
    std::vector<std::shared_ptr<data::SignalModel>>().swap(_state->signal_models());
    _state->signal_models() = models;
    make_channels_view_index();

    // CRITICAL: After recreating SignalModels, immediately set snapshot
    // pointers from the current view data. Without this, decode threads
    // that are already running (e.g., started by start_all_decode_tasks()
    // before this reload() was triggered via an async DeviceOptionsUpdated
    // event) will see nullptr snapshots and fail with "required channels
    // have not been specified". This is the root cause of the race condition
    // where adding a decoder causes other decoders to intermittently fail.
    if (_state->view_data()) {
      for (auto &m : _state->signal_models()) {
        switch (m->type()) {
        case SR_CHANNEL_LOGIC:
          m->set_snapshot(_state->view_data()->logic_shared());
          break;
        case SR_CHANNEL_ANALOG:
          m->set_snapshot(_state->view_data()->analog_shared());
          break;
        case SR_CHANNEL_DSO:
          m->set_snapshot(_state->view_data()->dso_shared());
          break;
        }
      }
    }
  } else if (mode == LOGIC || mode == ANALOG || mode == DSO || mode == MSO) {
    pxv_info("ERROR: Unable to create any channel in reload(). channels is empty or all skipped.");
    clear_signals();
  }

  spectrum_rebuild();

  // CRITICAL: reload() wholesale-replaces _state->signal_models() (new shared_ptr
  // objects, old ones destroyed). Without signals_changed(), the View's
  // DsoSignal/AnalogSignal keep stale _model pointers to the freed
  // SignalModels (0xfeeefeee), and any subsequent access UAFs. This is
  // symmetric with init_signals() which also ends with signals_changed().
  // Trigger path: load_config_from_json -> _session->reload() -> [here]
  // -> immediately after, load_config_from_json iterates
  // current_view()->get_own_signals() and calls DsoSignal::set_zero_ratio ->
  // _model->set_zero_offset. Without this notification the _model is dangling.
  // compute_change_event detects the pointer identity change and returns
  // AllReplaced, so View fully rebinds _model to the new SignalModels.
  // NOTE: reload() now handles DSO mode (case SR_CHANNEL_DSO), same as
  // init_signals(). Previously reload() skipped DSO channels entirely, making
  // it a no-op in DSO mode — so load_config_from_json's probe property
  // updates (vdiv/coupling/vfactor) were never reflected in SignalModel, and
  // the View kept stale _model pointers.
  signals_changed();
}

uint16_t SigSession::get_ch_num(int type) {
  uint16_t num_channels = 0;
  uint16_t logic_ch_num = 0;
  uint16_t dso_ch_num = 0;
  uint16_t analog_ch_num = 0;

  if (_state->device_agent().have_instance()) {
    for (auto m : _state->signal_models()) {
      if (!m->enabled())
        continue;

      if (m->type() == SR_CHANNEL_LOGIC)
        logic_ch_num++;
      else if (m->type() == SR_CHANNEL_DSO)
        dso_ch_num++;
      else if (m->type() == SR_CHANNEL_ANALOG)
        analog_ch_num++;
    }
  }

  switch (type) {
  case SR_CHANNEL_LOGIC:
    num_channels = logic_ch_num;
    break;
  case SR_CHANNEL_DSO:
    num_channels = dso_ch_num;
    break;
  case SR_CHANNEL_ANALOG:
    num_channels = analog_ch_num;
    break;
  default:
    num_channels = logic_ch_num + dso_ch_num + analog_ch_num;
    break;
  }

  return num_channels;
}

std::vector<std::shared_ptr<data::DecoderStack>> &
SigSession::get_decoder_stacks(data::SessionDocument *doc) {
  return _state->get_decoder_stacks(doc);
}

bool SigSession::add_decoder(
    srd_decoder *const dec, bool silent, DecoderStatus *dstatus,
    std::list<pv::data::decode::Decoder *> &sub_decoders,
    std::shared_ptr<data::DecoderStack> &out_stack,
    data::SessionDocument *doc) {
  (void)silent;
  if (dec == nullptr) {
    pxv_err("Decoder instance is nullptr!");
    return false;
  }

  data::SessionDocument *target = doc ? doc : _document_registry->get_active_document();

  out_stack = nullptr;

  try {
    bool ret = false;

    // Create the decoder
    std::map<const srd_channel *, int> probes;
    auto decoder_stack =
        std::make_shared<data::DecoderStack>(this, dec, dstatus);
    assert(decoder_stack);
    // Assign a unique handle id so the API/MCP layer can stably reference
    // this stack. A re-created stack (e.g. via a future add_decoder call)
    // always receives a fresh id, distinguishing it from reused stacks.
    decoder_stack->set_handle_id(_state->next_decoder_handle_id());

    // Make a list of all the probes
    std::vector<const srd_channel *> all_probes;

    for (const GSList *i = dec->channels; i; i = i->next) {
      all_probes.push_back((const srd_channel *)i->data);
    }

    for (const GSList *i = dec->opt_channels; i; i = i->next) {
      all_probes.push_back((const srd_channel *)i->data);
    }

    decoder_stack->stack().front()->set_probes(probes);

    // add sub decoder
    for (auto sub : sub_decoders) {
      decoder_stack->add_sub_decoder(std::unique_ptr<decode::Decoder>(sub));
    }

    if (sub_decoders.size() > 0) {
      auto lst_sub = sub_decoders.end();
      lst_sub--;
      QString sub_dec_name((*lst_sub)->decoder()->name);
      if (sub_dec_name != "") {
        // TODO: verify - decoder name was previously set on view::DecodeTrace.
        // DecoderStack has no set_name method; name management needs a new
        // mechanism.
      }
    }

    sub_decoders.clear();

    // The decoder options dialog (DecodeTrace::create_popup) is now shown
    // by the View layer (View::add_decoder) after Core returns the newly
    // created DecoderStack. Core never touches Qt Widgets, so it always
    // reports success here regardless of `silent`. The `silent` parameter
    // is kept for API compatibility but no longer triggers automatic
    // decode-task startup here (see the NOTE below).
    ret = true;

    if (ret) {
      if (target) {
        target->get_decoder_stacks().push_back(decoder_stack);
      }
      // When target is nullptr (neither doc nor _active_document is bound, a
      // rare edge case now that MCP uses _api_document and UI uses
      // _active_document), the newly created DecoderStack is intentionally
      // NOT stored in any container — it is returned via out_stack and the
      // caller owns it. set_owner_document(nullptr) is safe (simple setter).
      // The legacy _empty_decoder_stacks staging path was removed together
      // with the set_active_document migration logic.
      decoder_stack->set_owner_document(target);

      // NOTE: Starting the decode task here is intentionally avoided.
      // Previously this called `add_decode_task(decoder_stack)` when
      // (!silent && have_view_data()). However, after de-view-ization the
      // decoder options dialog (DecodeTrace::create_popup) is shown by
      // the View layer AFTER this method returns, so the user has not yet
      // had a chance to configure channel mappings when we would start the
      // decode thread. The decode thread would then run with empty probes
      // and bail out with "required channels have not been specified".
      //
      // Callers are responsible for starting the decode task at the right
      // time:
      //   - UI path (View::add_decoder): after create_popup() returns true
      //     (user accepted the dialog and configured channels).
      //   - MCP path (SessionService::add_decoder): via
      //     QTimer::singleShot(0, ...) after do_add() returns to avoid
      //     Qt signal races during rebuild_decoder_pannel().
      //   - Capture pipeline: when CopyToDocDone fires and the
      //     stack was added before capture, frame_ended() + add_decode_task()
      //     is invoked by the message handler.
      data_updated();

      out_stack = decoder_stack;
    }

    return ret;
  } catch (...) {
    pxv_err("Error!add_decoder() throws an exception.");
  }

  return false;
}

int SigSession::get_trace_index_by_key_handel(void *handel,
                                              data::SessionDocument *doc) {
  int dex = 0;

  for (auto stack : decode_traces(doc)) {
    if (stack->get_key_handel() == handel) {
      return dex;
    }
    ++dex;
  }

  return -1;
}

void SigSession::remove_decoder(int index, data::SessionDocument *doc) {
  data::SessionDocument *target = doc ? doc : _document_registry->get_active_document();
  int size = (int)decode_traces(target).size();
  (void)size;
  assert(index < size);

  auto it = decode_traces(target).begin() + index;
  auto stack = (*it);
  decode_traces(target).erase(it);

  // decode_traces(target) returns target->get_decoder_stacks() (or
  // _empty_decoder_stacks), so the erase above already removed it from the
  // document's list.

  // P0-3 fix: Stop the decode work. The stack's lifetime is managed by
  // shared_ptr — when the last reference is released (after the decode
  // thread finishes), the object is automatically destroyed.
  remove_decode_task(stack);

  // Check if the decode thread is still using this stack.
  bool thread_holds_stack = _decode_task_manager->is_task_running(stack);

  if (!thread_holds_stack) {
    signals_changed();
  }
  // If thread still holds the stack, it will finish and the shared_ptr
  // reference in _running_tasks will be released, triggering signals_changed()
}

void SigSession::remove_decoder_by_key_handel(void *handel,
                                              data::SessionDocument *doc) {
  data::SessionDocument *target = doc ? doc : _document_registry->get_active_document();
  int dex = get_trace_index_by_key_handel(handel, target);
  remove_decoder(dex, target);
}

void SigSession::spectrum_rebuild() {
  bool has_dso_signal = false;

  for (auto m : _state->signal_models()) {
    if (m->type() == SR_CHANNEL_DSO) {
      has_dso_signal = true;
      // check already have
      auto iter = _state->spectrum_stacks().begin();

      for (unsigned int i = 0; i < _state->spectrum_stacks().size(); i++, iter++) {
        if ((*iter)->get_index() == m->index())
          break;
      }

      // if not, rebuild
      if (iter == _state->spectrum_stacks().end()) {
        auto spectrum_stack =
            std::make_shared<data::SpectrumStack>(this, m->index());
        _state->spectrum_stacks().push_back(spectrum_stack);
      }
    }
  }

  if (!has_dso_signal) {
    _state->spectrum_stacks().clear();
  }

  signals_changed();
}

void SigSession::lissajous_rebuild(bool enable, int xindex, int yindex,
                                   double percent) {
  // Track B2: LissajousModel owned via unique_ptr — assignment auto-frees old
  auto m = std::make_unique<data::LissajousModel>();
  m->set_enabled(enable);
  m->set_x_index(xindex);
  m->set_y_index(yindex);
  m->set_percent((int)percent);
  _state->set_lissajous_model(std::move(m));
  signals_changed();
}

void SigSession::lissajous_disable() {
  if (_state->lissajous_model())
    _state->lissajous_model()->set_enabled(false);
}

void SigSession::math_rebuild(bool enable, int ch1_index, int ch2_index,
                              data::MathStack::MathType type) {
  ds_lock_guard lock(_state->data_mutex());

  _state->set_math_stack(nullptr);

  // The MathStack constructor now accepts channel indices and resolves the
  // DSO parameters (vdiv / vfactor / hw_offset / snapshot) through
  // SignalModel — no view::DsoSignal dependency. The View layer is
  // responsible for creating the matching MathTrace later (see
  // View::sync_derived_traces).
  //
  // When the user disables math (enable=false), we destroy any existing
  // MathStack and do not create a new one. The View's sync_derived_traces
  // observes the nullptr MathStack and tears down its MathTrace.
  if (enable) {
    _state->set_math_stack(
        std::make_shared<data::MathStack>(this, ch1_index, ch2_index, type));
  }

  signals_changed();
}

void SigSession::math_disable() {
  if (_state->math_stack())
    _state->math_stack()->init();
}

data::Snapshot *SigSession::get_snapshot(int type) {
  // Memory-safety: view_data() can return nullptr during startup/shutdown
  // or when called from the StoreSession worker thread during a tab switch.
  // Guard against null dereference.
  auto *vd = _state->view_data();
  if (!vd)
    return nullptr;
  if (type == SR_CHANNEL_LOGIC)
    return vd->get_logic();
  else if (type == SR_CHANNEL_ANALOG)
    return vd->get_analog();
  else if (type == SR_CHANNEL_DSO)
    return vd->get_dso();
  else
    return nullptr;
}

void SigSession::clear_error() {
  _state->set_error_pattern(0);
  _state->set_error(No_err);
}

void SigSession::Open() {}

void SigSession::Close() {
  if (_state->bClose())
    return;

  // Join any background file import thread before clearing data.
  // Close() clears data_list entries (p->clear()) which the import
  // thread may still be writing to via DataFeedParser.
  wait_for_import_complete_();

  _state->set_bClose(true);

  // Stop decode thread.
  clear_all_documents_decoders();

  pxv_info("SigSession::Close(), stop capture");
  stop_capture();

  // A3 fix: Stop glitch filter and signal invert background threads before
  // tearing down data. Set running flags false first so the task functions
  // know no new work should be accepted, then join the thread if still
  // joinable. Without this, a joinable std::thread would std::terminate on
  // destruction.
  _filter_processor->stop();

// Gap 3: join_copy_thread removed — zero-copy, no thread to join.

  for (auto &p : _state->data_list()) {
    p->clear();
  }
}

void SigSession::clear_all_decoder(bool bUpdateView) {
  if (decode_traces().empty())
    return;

  int dex = -1;
  clear_all_decode_task(dex);

  // P0-3 fix: _delete_flag removed — shared_ptr manages lifetime.
  // The running stack (if any) will be released when its decode thread
  // finishes and decode_single_task() removes it from _running_tasks.

  decode_traces().clear();

  // decode_traces() returns _active_document->get_decoder_stacks() (or
  // _empty_decoder_stacks), so the clear above already removed them from
  // the document's list. No need to clear
  // _active_document->get_decoder_stacks() a second time.

  if (!_state->bClose() && bUpdateView)
    signals_changed();
}

void SigSession::clear_all_documents_decoders() {
  int dex = -1;
  clear_all_decode_task(dex);

  _document_registry->clear_all_documents_decoders();
}

void SigSession::clear_all_decode_task(int &runningDex) {
  _decode_task_manager->clear_all_decode_task(runningDex);
}

void SigSession::clear_all_decode_task2() {
  _decode_task_manager->clear_all_decode_task2();
}

void SigSession::add_decode_task(std::shared_ptr<data::DecoderStack> stack) {
  _decode_task_manager->add_decode_task(stack);
}

std::shared_ptr<data::DecoderStack>
SigSession::get_decoder_trace(int index, data::SessionDocument *doc) {
  auto &traces = decode_traces(doc);
  if (index >= 0 && index < (int)traces.size()) {
    return traces[index];
  }
  pxv_err("get_decode_trace_by_index: index %d out of range (size=%d)", index, (int)traces.size());
  return nullptr;
}

Snapshot *SigSession::get_signal_snapshot() {
  int mode = _state->device_agent().get_work_mode();
  // During realtime refresh in double-buffer mode, check the live capture
  // buffer (capture_data) instead of the stale view_data.
  SessionData *data = _state->view_data();
  if (_capture_manager->is_realtime_refresh() &&
      _state->capture_data() != _state->view_data())
    data = _state->capture_data();
  if (mode == ANALOG)
    return data->get_analog();
  else if (mode == DSO)
    return data->get_dso();
  else
    return data->get_logic();
}

// Note: device_lib_event_callback_ex / on_device_lib_event removed.
// Fork libsigrok's ds_set_event_callback_ex API is gone; upstream libsigrok
// uses sr_session_stopped_callback for session-end notification and the
// datafeed callback for packet events. Hotplug (DS_EV_NEW_DEVICE_ATTACH etc.)
// is not supported in this migration (would need libusb hotplug API directly).
// The CollectStart/CollectEnd/EndCollectWork events are now emitted by
// CaptureManager which owns the capture lifecycle.

// Note: add_event_listener / remove_event_listener / remove_callback /
// broadcast<T>() / broadcast_sync<T>() / broadcast_async<T>() /
// dispatch_to<Iface>() are now inline forwarders in sigsession.h that delegate
// to the EventBus owned by _event_bus. The _broadcast_depth guard and the
// _callbacks / _event_listeners vectors live inside EventBus.

// ============================================================================
// IEventListener overrides — Core-internal state-machine events.
// These 5 events were previously handled by the former OnMessage switch
// (now removed). The logic is copied verbatim. The self-emits inside
// on_event(RevEndPacket) now use broadcast_async<TypedEvent> (worker-thread
// safe via qApp queue).
// ============================================================================

void SigSession::on_device_options_updated() {
  reload();
}

void SigSession::on_trig_next_collect() {
  if (_state->is_working() && is_repeat_mode()) {
    // Note: We do NOT call clear_all_decode_task() here. The decoder SIGSEGV
    // is now prevented by two other fixes:
    // 1. first_payload() reuses the MmapAllocator (no UnmapViewOfFile) in
    //    repeat mode, so decoder's di->inbuf pointers stay valid.
    // 2. LogicSignal holds a shared_ptr (_data_ref) to the Snapshot, preventing
    //    use-after-free on the main thread.
    // Stopping decode threads here caused: (a) decode results lost on each
    // repeat, (b) 5-6s main-thread freeze from thread join, (c) blank screen
    // on first stop because decode was killed before completion.

    if (_capture_manager->get_repeat_intvl() > 0) {
      _capture_manager->set_repeat_hold_prg(100);
      _capture_manager->start_repeat_timer(_capture_manager->get_repeat_intvl() * 1000);
      int intvl = _capture_manager->get_repeat_intvl() * 1000 / 20;

      if (intvl >= 100) {
        _capture_manager->set_repeat_wait_prog_step(5);
      } else if (_capture_manager->get_repeat_intvl() >= 1) {
        intvl = _capture_manager->get_repeat_intvl() * 1000 / 10;
        _capture_manager->set_repeat_wait_prog_step(10);
      } else {
        intvl = _capture_manager->get_repeat_intvl() * 1000 / 5;
        _capture_manager->set_repeat_wait_prog_step(20);
      }

      _capture_manager->start_repeat_wait_prog_timer(intvl);
    } else {
      _capture_manager->set_repeat_hold_prg(0);
      _capture_manager->exec_capture();
    }
  }
}

void SigSession::on_rev_end_packet() {
  pxv_info("SigSession::on_event(RevEndPacket): mode=%d stream=%d single=%d",
           _state->device_agent().get_work_mode(),
           _capture_manager->is_stream_mode(),
           _capture_manager->is_single_mode());
  // MSO (Mixed Signal Oscilloscope) includes logic channels — decoders
  // and the capture→copy→decode pipeline must run in MSO just as in LOGIC.
  // This matches is_logic_rendering_mode() (view.cpp) and the existing
  // LOGIC||MSO merges in switch_work_mode / get_ring_sample_count.
  int cur_mode = _state->device_agent().get_work_mode();
  if (cur_mode == LOGIC || cur_mode == MSO) {
    bool bAddDecoder = false;
    bool bSwapBuffer = false;

    if (is_single_mode()) {
      // Always add decoder and copy to document. With zero-copy (shared_ptr
      // sharing), copy_data_to_document is instant — no reason to skip it
      // for stream mode. Without it, the document never gets data,
      // CopyToDocDone is never broadcast, set_data_document is never called,
      // and the View's signal data pointers may not be properly bound.
      bAddDecoder = true;
    } else if (is_repeat_mode()) {
      if (!_capture_manager->is_stream_mode()) {
        // Non-stream repeat: single buffer (capture_data == view_data).
        // No swap needed — data already in view_data. Just add decoder.
        bAddDecoder = true;
      } else if (_capture_manager->capture_times() > 1) {
        bAddDecoder = true;
        bSwapBuffer = true;
      }
    } else if (is_loop_mode()) {
      bAddDecoder = true;
    }

    if (is_repeat_mode()) {
      AppConfig &app = AppConfig::Instance();
      bool swapBackBufferAlways = app.appOptions.swapBackBufferAlways;
      if (!swapBackBufferAlways && !_state->is_working() && _capture_manager->capture_times() > 1) {
        if (_capture_manager->is_stream_mode()) {
          // Stream mode: capture_data is a separate back buffer. Discard it
          // on stop to free memory. Keep the previous view_data as-is.
          bAddDecoder = false;
          bSwapBuffer = false;
          _state->capture_data()->clear();
        }
        // Non-stream mode: capture_data == view_data. Do NOT clear — the user
        // expects to see the last capture's data when they stop. Also keep
        // bAddDecoder=true (from line 1986) so decoders process the last
        // capture.
      }
    }

    if (bAddDecoder) {
      clear_all_decode_task2();
      _capture_manager->clear_decode_result();
    }

    _capture_manager->stop_trig_check_timer();

    // Switch the caputrued data buffer to view.
    if (bSwapBuffer) {
      // No copy_thread to join — copy_data_to_document is now zero-copy
      // (shared_ptr sharing, instant). The old join was needed because the
      // background deep-copy thread might still be reading from view_data
      // when the next RevEndPacket arrived.
      if (_state->view_data() != _state->capture_data())
        _state->view_data()->clear();

      _state->set_view_data(_state->capture_data());
      attach_data_to_signal(_state->view_data());
      _state->set_session_time(_state->trig_time());

      receive_trigger(_state->view_data()->_trig_pos); // Update trig position.

      _event_bus->broadcast_async<interface::DataPoolChanged>({});
    } else if (is_repeat_mode() && !_capture_manager->is_stream_mode()) {
      // Single-buffer repeat: data already in view_data (capture_data ==
      // view_data). Still need to update session time and trigger position.
      _state->set_session_time(_state->trig_time());
      receive_trigger(_state->view_data()->_trig_pos);
      _event_bus->broadcast_async<interface::DataPoolChanged>({});
    }

    if (bAddDecoder && _document_registry->get_active_document()) {
      // Zero-copy: copy_data_to_document now shares shared_ptrs (instant).
      // No background thread, no mutex, no CopyInProgressChanged — the old
      // deep-copy thread is no longer needed.
      data::SessionDocument *doc =
          _document_registry->get_capture_owner_document()
              ? _document_registry->get_capture_owner_document()
              : _document_registry->get_active_document();
      copy_data_to_document(doc);
      _event_bus->broadcast_async<interface::CopyToDocDone>({nullptr});
    } else {
      // No active document (typical in headless mode) OR stream mode (no
      // copy thread needed). Skip the deep copy to a SessionDocument and
      // start the decoders directly. The decoders read their snapshots
      // from _view_data via get_signal_models(), so they don't need a
      // SessionDocument to be set up.
      pxv_info("RevEndPacket ELSE branch: starting decoders (single=%d)",
               _capture_manager->is_single_mode());
      start_all_decode_tasks();

      // CaptureOwnerGuard 释放 + EndCollectWork 广播由 SessionStopped 事件统一
      // 处理（在 DeviceAgent worker 线程的 sr_session_run() 返回后触发）。
      // SR_DF_END / RevEndPacket 触发时 libsigrok 的 main loop 仍在运行，
      // 提前释放 guard 会让第二次 sr_session_start() 与上一次 session 的停止
      // 发生竞争（第二次采集不自动停止的根因）。
      // repeat/loop 模式保留原 index 清理逻辑（仅清 index 不释放 guard）。
      if (!_capture_manager->is_single_mode()) {
        std::lock_guard<std::mutex> lock(_document_registry->capture_state_mutex());
        _document_registry->set_capture_owner_index_locked(SIZE_MAX);
      }
    }

    // 采集完成后自动重新应用毛刺滤波(若用户启用了 auto-apply)
    if (_state->view_data()->_glitch_filter_auto_apply &&
        !_state->view_data()->_glitch_filter_thresholds.empty() &&
        _state->view_data()->get_logic() && !_state->view_data()->get_logic()->empty()) {
      _filter_processor->set_glitch_filter(
          _state->view_data()->_glitch_filter_thresholds,
          _state->view_data()->_glitch_filter_modes);
    }

    if (is_repeat_mode()) {
      _repeat_wait_decode =
          is_repeat_mode() && _state->is_working() && !_state->decode_traces().empty();
      _repeat_session_stopped = false;
      _repeat_decode_done = !_repeat_wait_decode;
      if (_state->is_working()) {
        reset_repeat_analog_trigger_frame();
        if (_repeat_analog_trigger_active)
          set_repeat_analog_trigger_display_hold(true);
      } else {
        _repeat_analog_trigger_active = false;
        _repeat_wait_decode = false;
        _repeat_session_stopped = false;
        _repeat_decode_done = false;
        set_repeat_analog_trigger_display_hold(false);
      }
    }
    frame_ended();
  }
}

void SigSession::on_copy_to_doc_done() {
  // copy_data_to_document has completed (now synchronous/instant — zero-copy
  // shared_ptr sharing). Start decoders.
  // NOTE: _capture_owner_document is NOT cleared here for repeat/loop mode —
  // it is managed by CaptureOwnerGuard for the whole capture session.
  // In repeat mode the owner persists across frames; the guard is reset
  // only on stop_capture or tab close (clear_capture_owner_document).
  start_all_decode_tasks();
  pxv_info("Background copy_data_to_document completed. Decoders started.");

  // CaptureOwnerGuard 释放 + EndCollectWork 广播由 SessionStopped 事件统一
  // 处理（在 DeviceAgent worker 线程的 sr_session_run() 返回后触发）。
  // 原代码在 CopyToDocDone 时释放 guard 会与 libsigrok main loop 的停止
  // 发生竞争（第二次采集不自动停止的根因）。
}

void SigSession::on_device_speed_not_match() {
  QString strMsg(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DEVICE_SPEED_TOO_LOW),
                     "Speed too low!"));
  delay_prop_msg(strMsg);
}

void SigSession::DeviceConfigChanged() {
  // broadcast_async<SampleCountUpdated> is queued on qApp via
  // Qt::QueuedConnection, so the previous _suppress_config_broadcast guard
  // (which prevented nested reload -> signals_changed -> View AllReplaced UAF
  // during JSON restore) is no longer needed: the caller's stack frame
  // completes before any listener processes the event.
  // Notify UI that device config changed (e.g. disk cache toggle),
  // so sampling duration can be recalculated from SR_CONF_HW_DEPTH
  _event_bus->broadcast_async<interface::SampleCountUpdated>(
      {(uint64_t)get_ring_sample_count()});
}

void SigSession::DeviceSessionStopped() {
  // Called from DeviceAgent's worker thread AFTER sr_session_run() returned.
  //
  // Phase 1 refactoring: set _is_working=false + notify the condition_variable
  // DIRECTLY on the worker thread, BEFORE broadcasting SessionStopped. This
  // allows wait_capture_complete() (main thread) to be woken via cv.wait_for
  // instead of QEventLoop::exec(), breaking the circular dependency where
  // the main thread was blocked in QEventLoop and couldn't pump the event
  // queue to receive the SessionStopped event.
  //
  // The SessionStopped broadcast_async is still sent for UI notification
  // (main-thread cleanup: release_capture_owner, EndCollectWork, etc.).
  pxv_info("DeviceSessionStopped: sr_session_run() returned, "
           "setting is_working=false + notifying cv, then broadcasting SessionStopped.");
  _state->notify_capture_complete();
  _event_bus->broadcast_async<interface::SessionStopped>({});
}

void SigSession::on_session_stopped_event() {
  // Main-thread handler for the SessionStopped event re-broadcast by
  // DeviceSessionStopped().
  //
  // Phase 1 refactoring: _is_working is now set to false by
  // DeviceSessionStopped() on the worker thread (via notify_capture_complete),
  // BEFORE this handler runs. So we can no longer use is_working() to
  // distinguish the manual-stop path from the auto-stop path.
  //
  // Instead, we use has_capture_owner() as the guard: if the CaptureOwnerGuard
  // was already released by action_stop_capture (manual stop), skip cleanup.
  // If the guard is still held (auto-stop), proceed with main-thread cleanup.
  if (!_document_registry->has_capture_owner()) {
    pxv_info("SigSession::on_event(SessionStopped): capture owner already "
             "released (manual stop path). Skipping cleanup.");
    _repeat_wait_decode = false;
    _repeat_session_stopped = false;
    _repeat_decode_done = false;
    _repeat_analog_trigger_active = false;
    set_repeat_analog_trigger_display_hold(false);
    return;
  }

  // Auto-stop path (capture completed normally, sr_session_run() returned).
  // _is_working was already set to false by DeviceSessionStopped() on the
  // worker thread — no need to set it here.
  //
  // Matches DSView's DS_EV_COLLECT_TASK_END handler: in repeat mode, keep
  // _is_working and the CaptureOwnerGuard alive, and broadcast
  // TrigNextCollect to trigger the next collection. In all other modes,
  // release the guard and broadcast EndCollectWork.
  if (is_repeat_mode()) {
    _capture_manager->data_unlock();
    _repeat_session_stopped = true;
    if (_repeat_wait_decode) {
      pxv_info("SigSession::on_event(SessionStopped): repeat mode — waiting "
               "for decoder completion before next collection.");
      continue_repeat_after_decode_if_ready();
    } else {
      pxv_info("SigSession::on_event(SessionStopped): repeat mode — keeping "
               "guard alive, broadcasting TrigNextCollect.");
      _repeat_session_stopped = false;
      _event_bus->broadcast_async<interface::TrigNextCollect>({});
    }
  } else {
    _repeat_wait_decode = false;
    _repeat_session_stopped = false;
    _repeat_decode_done = false;
    _repeat_analog_trigger_active = false;
    set_repeat_analog_trigger_display_hold(false);
    pxv_info("SigSession::on_event(SessionStopped): releasing CaptureOwnerGuard "
             "(auto-stop path).");
    _capture_manager->data_unlock();
    // Plan B Phase 1: broadcast_sync → broadcast_async.
    _event_bus->broadcast_async<interface::EndCollectWorkPrev>({});
    _document_registry->release_capture_owner();
    _event_bus->broadcast_async<interface::EndCollectWork>({});
  }
}

void SigSession::on_decode_done_event() {
  // Phase 3: Signal the SharedState so that wait_for_decode_complete()
  // (API/RPC layer) is woken without depending on the Qt event queue.
  _state->notify_decode_complete();

  // Repeat analog trigger evaluation
  if (!_repeat_wait_decode || !_state->is_working() || !is_repeat_mode())
    return;
  if (_repeat_analog_trigger_active)
    evaluate_repeat_analog_trigger();
  _repeat_decode_done = true;
  if (PXV_VERBOSE_REPEAT_LOG)
    pxv_info("Repeat decode gate: done trigger=%d match=%d session_stopped=%d",
           _repeat_analog_trigger_active ? 1 : 0,
           _repeat_analog_trigger_match ? 1 : 0,
           _repeat_session_stopped ? 1 : 0);
  continue_repeat_after_decode_if_ready();
}

void SigSession::on_end_collect_work_prev() {
  // Manual-stop cleanup: cancel every pending repeat/trigger gate on a manual stop.
  if (_repeat_wait_decode || _repeat_session_stopped || _repeat_decode_done ||
      _repeat_analog_trigger_active || _repeat_analog_trigger_display_hold) {
    pxv_info("Manual stop cleanup: cancel repeat/decode trigger gate and release analog HOLD.");
  }

  _repeat_wait_decode = false;
  _repeat_session_stopped = false;
  _repeat_decode_done = false;
  _repeat_analog_trigger_active = false;
  _repeat_analog_trigger_match = false;
  _repeat_analog_trigger_sample = 0;
  _repeat_analog_trigger_config = data::DecoderAnalogTriggerConfig{};

  // Invalidate every queued analog trigger UI event from the just-stopped generation.
  const uint64_t old_generation =
      _repeat_analog_trigger_ui_generation.fetch_add(1, std::memory_order_acq_rel);
  _repeat_analog_trigger_display_hold = false;
  pxv_info("Manual stop cleanup: invalidated analog trigger UI generation %llu -> %llu.",
           (unsigned long long)old_generation,
           (unsigned long long)(old_generation + 1));
}

bool SigSession::repeat_analog_display_trigger_enabled() {
  if (!is_repeat_mode())
    return false;
  for (const auto &stack : _state->decode_traces()) {
    data::DecoderAnalogTriggerConfig config;
    if (stack && stack->get_analog_display_trigger_config(config))
      return true;
  }
  return false;
}

void SigSession::reset_repeat_analog_trigger_frame() {
  _repeat_analog_trigger_active = repeat_analog_display_trigger_enabled();
  _repeat_analog_trigger_match = false;
  _repeat_analog_trigger_sample = 0;
  _repeat_analog_trigger_config = data::DecoderAnalogTriggerConfig{};
  if (_repeat_analog_trigger_active) {
    for (const auto &stack : _state->decode_traces()) {
      if (stack && stack->get_analog_display_trigger_config(
                       _repeat_analog_trigger_config))
        break;
    }
    if (PXV_VERBOSE_REPEAT_LOG)
      pxv_info("Analog display trigger armed: mode=%s ch=%d edge=%d level=%.9g pos=%d%%",
             _repeat_analog_trigger_config.mode == data::DecoderAnalogTriggerMode::Normal ? "normal" : "auto",
             _repeat_analog_trigger_config.channel,
             (int)_repeat_analog_trigger_config.edge,
             _repeat_analog_trigger_config.level,
             _repeat_analog_trigger_config.display_position_percent);
  }
}

void SigSession::evaluate_repeat_analog_trigger() {
  _repeat_analog_trigger_match = false;
  for (const auto &stack : _state->decode_traces()) {
    if (!stack)
      continue;
    uint64_t sample = 0;
    data::DecoderAnalogTriggerConfig config;
    if (!stack->get_analog_display_trigger_config(config))
      continue;
    _repeat_analog_trigger_config = config;
    if (stack->find_analog_display_trigger(sample, &config)) {
      _repeat_analog_trigger_match = true;
      _repeat_analog_trigger_sample = sample;
      _repeat_analog_trigger_config = config;
      break;
    }
  }
  if (PXV_VERBOSE_REPEAT_LOG)
    pxv_info("Analog display trigger evaluated: match=%d sample=%llu mode=%s",
           _repeat_analog_trigger_match ? 1 : 0,
           (unsigned long long)_repeat_analog_trigger_sample,
           _repeat_analog_trigger_config.mode == data::DecoderAnalogTriggerMode::Normal ? "normal" : "auto");
}

void SigSession::set_repeat_analog_trigger_display_hold(bool hold) {
  if (_repeat_analog_trigger_display_hold == hold && hold)
    return;
  _repeat_analog_trigger_display_hold = hold;
  const uint64_t generation =
      _repeat_analog_trigger_ui_generation.load(std::memory_order_acquire);
  _event_bus->broadcast_async<interface::DecoderAnalogTriggerDisplayHold>(
      {hold, generation});
}

void SigSession::continue_repeat_after_decode_if_ready() {
  if (!_repeat_wait_decode || !_repeat_session_stopped || !_repeat_decode_done)
    return;
  if (!_state->is_working() || !is_repeat_mode()) {
    _repeat_wait_decode = false;
    _repeat_session_stopped = false;
    _repeat_decode_done = false;
    _repeat_analog_trigger_active = false;
    set_repeat_analog_trigger_display_hold(false);
    return;
  }

  if (_repeat_analog_trigger_active) {
    if (_repeat_analog_trigger_match) {
      _state->view_data()->_trig_pos = _repeat_analog_trigger_sample;
      if (auto *document = _document_registry->get_active_document())
        document->set_trigger_pos(_repeat_analog_trigger_sample);
      _event_bus->broadcast_async<interface::DecoderAnalogTriggerFound>({
          _repeat_analog_trigger_sample,
          _repeat_analog_trigger_config.display_position_percent,
          _repeat_analog_trigger_config.channel,
          _repeat_analog_trigger_config.level,
          _repeat_analog_trigger_ui_generation.load(std::memory_order_acquire)});
      set_repeat_analog_trigger_display_hold(false);
      set_repeat_analog_trigger_display_hold(true);
    } else if (_repeat_analog_trigger_config.mode == data::DecoderAnalogTriggerMode::Auto) {
      set_repeat_analog_trigger_display_hold(false);
      set_repeat_analog_trigger_display_hold(true);
    } else {
      pxv_info("Analog display trigger NORMAL: no match, keeping previous frame.");
    }
  }

  _repeat_wait_decode = false;
  _repeat_session_stopped = false;
  _repeat_decode_done = false;
  _repeat_analog_trigger_active = false;
  _event_bus->broadcast_async<interface::TrigNextCollect>({});
}

void SigSession::force_release_capture_state() {
  // Emergency fallback called by wait_capture_complete() when the
  // SessionStopped event was suppressed by the EventBus broadcast depth
  // guard. Mirrors the non-repeat branch of on_event(SessionStopped):
  //   1. set_is_working(false)
  //   2. data_unlock()
  //   3. broadcast_sync<EndCollectWorkPrev> (may also be suppressed, but
  //      that's OK — the important thing is releasing _is_working)
  //   4. release_capture_owner()
  //   5. broadcast_async<EndCollectWork>
  //
  // This is idempotent: if on_event(SessionStopped) already ran (manual
  // stop path), is_working() is already false and this is a no-op.
  if (!_state->is_working()) {
    pxv_info("force_release_capture_state: is_working already false, nothing to do.");
    return;
  }
  pxv_warn("force_release_capture_state: releasing CaptureOwnerGuard "
           "(SessionStopped event was suppressed by EventBus).");
  _state->set_is_working(false);
  _capture_manager->data_unlock();
  // Plan B Phase 1: broadcast_sync → broadcast_async.
  _event_bus->broadcast_async<interface::EndCollectWorkPrev>({});
  _document_registry->release_capture_owner();
  _event_bus->broadcast_async<interface::EndCollectWork>({});
}

bool SigSession::switch_work_mode(int mode) {
  assert(!_state->is_working());
  int cur_mode = _state->device_agent().get_work_mode();

  if (cur_mode != mode) {
    set_collect_mode(COLLECT_SINGLE);

    // Update the work mode via DeviceAgent (handles both DSL/PXLogic driver-
    // side SR_CONF_DEVICE_MODE and app-layer cache for demo/file/compat).
    _state->device_agent().set_work_mode(mode);

    // Sync channel enabled flags to the new work mode. Upstream libsigrok
    // demo driver's demo_prepare_data() inspects ch->enabled to decide
    // whether to send SR_DF_DSO vs SR_DF_LOGIC/ANALOG. If LOGIC channels
    // remain enabled after switching to DSO mode, the driver takes the
    // logic/analog path and never emits SR_DF_DSO — leaving the DSO view
    // empty. The old DSView fork handled this inside ds_set_actived_device_mode();
    // under upstream libsigrok we must do it here.
    //
    // Rules:
    //   LOGIC mode — enable LOGIC channels, disable ANALOG/DSO.
    //   DSO mode   — enable DSO channels, disable LOGIC/ANALOG.
    //   ANALOG mode— enable ANALOG channels, disable LOGIC/DSO.
    //   MSO mode   — enable LOGIC + ANALOG, disable DSO.
    for (GSList *l = _state->device_agent().get_channels(); l; l = l->next) {
      sr_channel *probe = (sr_channel *)l->data;
      if (!probe)
        continue;
      bool want_enabled = false;
      switch (mode) {
      case LOGIC:  want_enabled = (probe->type == SR_CHANNEL_LOGIC); break;
      case DSO:    want_enabled = (probe->type == SR_CHANNEL_DSO);   break;
      case ANALOG: want_enabled = (probe->type == SR_CHANNEL_ANALOG);break;
      case MSO:    want_enabled = (probe->type == SR_CHANNEL_LOGIC ||
                                   probe->type == SR_CHANNEL_ANALOG); break;
      default: break;
      }
      if (probe->enabled != want_enabled) {
        sr_dev_channel_enable(probe, want_enabled);
        pxv_info("switch_work_mode: ch[%d] '%s' type=%d enabled %d->%d",
                 probe->index, probe->name ? probe->name : "(nullptr)",
                 probe->type, probe->enabled, want_enabled);
      }
    }

    if (cur_mode == LOGIC || cur_mode == MSO) {
      clear_all_decode_task2();
      _capture_manager->clear_decode_result();
    }

    _capture_manager->set_is_stream_mode(false);
    // Stream mode is relevant for logic-capable modes (LOGIC / MSO).
    if (mode == LOGIC || mode == MSO) {
      if (_state->device_agent().is_hardware()) {
        _capture_manager->set_is_stream_mode(_state->device_agent().is_stream_mode());
      }
    }

    _state->capture_data()->clear();
    _state->view_data()->clear();
    _state->set_capture_data(_state->view_data());

    init_signals();

    set_cur_snap_samplerate(_state->device_agent().get_sample_rate());
    set_cur_samplelimits(_state->device_agent().get_sample_limit());

    pxv_info("Switch work mode to:%d", mode);

    // broadcast_async<DeviceModeChanged> is queued on qApp via
    // Qt::QueuedConnection, so View finishes its signals_changed rebuild
    // before handlers access view::Signal::_model. No separate _deferred
    // variant is needed.
    _event_bus->broadcast_async<interface::DeviceModeChanged>({});

    return true;
  }
  return false;
}

void SigSession::clear_signals() {
  _state->set_math_stack(nullptr);

  _state->signal_models().clear();
  // (signal_models_mutex not needed here — all callers are on the UI thread
  // during session teardown, and no decode/save threads are running.)
}

std::shared_ptr<data::SignalModel> SigSession::get_signal_by_index(int index) {
  for (auto &m : _state->signal_models()) {
    if (m->index() == index)
      return m;
  }
  return nullptr;
}

void SigSession::on_load_config_end() {
  set_cur_snap_samplerate(_state->device_agent().get_sample_rate());
  set_cur_samplelimits(_state->device_agent().get_sample_limit());
}

void SigSession::clear_view_data() {
  _state->view_data()->clear();
  data_updated();
}

void SigSession::set_trace_name(std::shared_ptr<data::SignalModel> model,
                                QString name) {
  if (!model) {
    pxv_warn("%s", "SigSession::set_trace_name: model is nullptr");
    return;
  }
  assert(model);

  model->set_name(name.toStdString());

  // SignalModel covers Logic/Analog/Dso channel types. The decoder case is
  // handled separately via set_decoder_row_label().
  if (model->type() == SR_CHANNEL_LOGIC ||
      model->type() == SR_CHANNEL_ANALOG) {
    _state->device_agent().set_channel_name(model->index(), name.toUtf8());
  }
}

void SigSession::set_decoder_row_label(int index, QString label) {
  // Set the custom label on the DecoderStack at the given index so that
  // export functions and list_analyzers can distinguish multiple instances
  // of the same decoder (e.g. "SPI(CH2.SPI)" vs "SPI(CH3.SPI)").
  auto &stacks = get_decoder_stacks();
  if (index >= 0 && index < (int)stacks.size()) {
    if (stacks[index])
      stacks[index]->set_label(label);
  }
}

std::shared_ptr<data::SignalModel>
SigSession::get_channel_by_index(int orgIndex) {
  for (auto &m : _state->signal_models()) {
    if (m->index() == orgIndex) {
      return m;
    }
  }
  return nullptr;
}

void SigSession::make_channels_view_index(int start_dex) {
  // SignalModel is a pure data model and has no view_index property.
  // The View layer is responsible for tracking view index on its own
  // view::Signal objects (created from SignalModel via SignalFactory).
  // This method is now a no-op.
  (void)start_dex;
}

void SigSession::update_dso_data_scale() {
  int mode = _state->device_agent().get_work_mode();

  if (mode == DSO) {
    // TODO: view::DsoSignal::get_scale() returned a UI rendering scale
    // computed from view rect height, vdiv, vfactor and hw_offset. After
    // de-view-ization, SignalModel holds vdiv/vfactor/hw_offset but not the
    // view rect height, so the rendering scale cannot be computed here.
    // The View layer is responsible for calling DsoSnapshot::set_data_scale()
    // on its own cloned DsoSignal objects.
    (void)mode;
  }
}

int64_t SigSession::get_ring_sample_count() {
  int mode = _state->device_agent().get_work_mode();
  // Memory-safety: view_data() can return nullptr during startup/shutdown.
  auto *vd = _state->view_data();
  if (!vd)
    return 0;
  if (mode == LOGIC || mode == MSO) {
    auto *logic = vd->get_logic();
    return logic ? logic->get_ring_sample_count() : 0;
  } else if (mode == DSO) {
    auto *dso = vd->get_dso();
    return dso ? dso->get_ring_sample_count() : 0;
  } else {
    auto *analog = vd->get_analog();
    return analog ? analog->get_ring_sample_count() : 0;
  }
}

void SigSession::update_lang_text() {
  // TODO: view::SpectrumTrace::update_lang_text() was a UI rendering method
  // that refreshed localized text on spectrum trace widgets. After
  // de-view-ization, SigSession no longer owns view::SpectrumTrace instances.
  // The View layer is responsible for updating language text on its own
  // rendering objects.
}

bool SigSession::have_decoded_result() {
  for (auto stack : decode_traces()) {
    if (stack->get_result_count() > 0) {
      return true;
    }
  }

  return false;
}

void SigSession::apply_samplerate() { on_load_config_end(); }

data::LogicSnapshot *SigSession::get_logic_snapshot() {
  // Memory-safety: view_data()/capture_data() can return nullptr.
  if (_capture_manager->is_realtime_refresh() &&
      _state->capture_data() != _state->view_data()) {
    auto *cd = _state->capture_data();
    return cd ? cd->get_logic() : nullptr;
  }
  auto *vd = _state->view_data();
  return vd ? vd->get_logic() : nullptr;
}

std::shared_ptr<data::LogicSnapshot> SigSession::get_logic_snapshot_shared() {
  if (_capture_manager->is_realtime_refresh() &&
      _state->capture_data() != _state->view_data())
    return _state->capture_data()->logic_shared();
  return _state->view_data()->logic_shared();
}

data::AnalogSnapshot *SigSession::get_analog_snapshot() {
  if (_capture_manager->is_realtime_refresh() &&
      _state->capture_data() != _state->view_data()) {
    auto *cd = _state->capture_data();
    return cd ? cd->get_analog() : nullptr;
  }
  auto *vd = _state->view_data();
  return vd ? vd->get_analog() : nullptr;
}

data::DsoSnapshot *SigSession::get_dso_snapshot() {
  if (_capture_manager->is_realtime_refresh() &&
      _state->capture_data() != _state->view_data()) {
    auto *cd = _state->capture_data();
    return cd ? cd->get_dso() : nullptr;
  }
  auto *vd = _state->view_data();
  return vd ? vd->get_dso() : nullptr;
}

// Task C1.5: DSO measurement computation via core::MeasureCalculator.
// Computes raw MeasurementResult list from the view_data() DsoSnapshot +
// signal_models, then converts each result to api::MeasurementValue list
// using the per-channel data_scale (vdiv) + measure_vf (probe factor from
// SignalModel via DsoSnapshot) + vfactor (probe factor from SignalModel —
// same value as the View layer's _vDial->get_factor(), kept in sync via
// DsoSignal::set_factor → model->set_vfactor).
//
// The voltage formula preserves the original DsoMeasure behavior exactly:
//   v_mV = raw_adc * data_scale * measure_vf * vfactor * DS_CONF_DSO_VDIVS
//          / view_rect_height
// where measure_vf and vfactor are both the probe factor (this matches the
// original code where k = get_measure_voltage_factor() and _vDial->
// get_factor() were both the probe factor). See dso_measure.cpp original
// get_voltage(double v, int p, scaled=false).
//
// view_rect_height: 0 = use headless default (256). The View layer passes
// its actual get_view_rect().height() so GUI-displayed voltages match the
// original DsoMeasure computation exactly.
std::vector<api::MeasurementValue>
SigSession::get_measurements(int channel_index, int view_rect_height) {
  std::vector<api::MeasurementValue> result;

  SessionData *data = _state->view_data();
  if (!data)
    return result;

  auto *dso = data->get_dso();
  if (!dso || dso->empty())
    return result;

  auto signal_models = get_signal_models_snapshot();

  // Step 1: compute raw MeasurementResult list (max/min/rms/mean per channel)
  auto raw_results = core::MeasureCalculator::compute(
      data, signal_models, channel_index, view_rect_height);

  // Step 2: convert each raw result to api::MeasurementValue list
  for (const auto &r : raw_results) {
    // Look up data_scale and measure_vf from the DsoSnapshot (same source
    // as the original DsoMeasure::get_voltage — set by
    // SessionStateContext::set_cur_snap_samplerate from m->vdiv() and
    // m->vfactor()).
    double data_scale = 0.0;
    uint64_t measure_vf = 1;
    uint64_t vfactor = 1;

    if (dso->has_data(r.channel_index)) {
      data_scale = (double)dso->get_data_scale(r.channel_index);
      measure_vf = dso->get_measure_voltage_factor(r.channel_index);
    }

    // Look up vfactor from the SignalModel (same value as the View layer's
    // _vDial->get_factor() — kept in sync via DsoSignal::set_factor →
    // model->set_vfactor). This is the Core-layer substitute for the
    // View-only _vDial widget.
    for (const auto &m : signal_models) {
      if (m && m->index() == r.channel_index) {
        vfactor = (uint64_t)m->vfactor();
        break;
      }
    }

    auto values = core::MeasureCalculator::to_measurement_values(
        r, data_scale, measure_vf, vfactor, view_rect_height);

    // Flatten into the result vector
    for (auto &v : values) {
      result.push_back(std::move(v));
    }
  }

  return result;
}

// Task C2.4: cursor state forwarded to SessionStateContext::cursor_registry().
// The registry is the single Core-layer source of truth for cursor positions;
// the View layer (view::Cursor / ViewCursors) reads and writes through these
// methods via the DataSource interface, and the MCP API (SessionService::
// add_cursor / remove_cursor / get_cursors) does the same so headless mode
// returns real data. add_cursor returns the positional index of the new entry.
std::vector<core::CursorEntry> SigSession::get_cursors() const {
  return _state->cursor_registry().get_cursors();
}

int SigSession::add_cursor(uint64_t sample_position) {
  return _state->cursor_registry().add_cursor(sample_position);
}

bool SigSession::remove_cursor(int index) {
  return _state->cursor_registry().remove_cursor(index);
}

bool SigSession::set_cursor_position(int index, uint64_t sample_position) {
  return _state->cursor_registry().set_cursor_position(index, sample_position);
}

void SigSession::clear_cursors() {
  _state->cursor_registry().clear();
}

void SigSession::set_active_document(data::SessionDocument *doc) {
  _document_registry->set_active_document(doc);
}

void SigSession::clear_capture_owner_document(data::SessionDocument *doc) {
  _document_registry->clear_capture_owner_document(doc);
}


bool SigSession::is_copy_in_progress() const {
  return _document_registry->is_copy_in_progress();
}

data::SessionDocument *SigSession::get_capture_owner_document() const {
  return _document_registry->get_capture_owner_document();
}

data::SessionDocument *SigSession::get_active_document() {
  return _document_registry->get_active_document();
}

// phase 2: SigSession::register_document / unregister_document removed.
// Document ownership is now held by DocumentRegistry. Callers use
// _session->document_registry()->take_document(...) / release_document(...).

void SigSession::set_trigger_config(const data::TriggerConfig &cfg) {
  _state->set_trigger_config(cfg);
  _event_bus->broadcast_async<interface::TriggerConfigChanged>(
      {&_state->trigger_config()});
}

void SigSession::sync_trigger_to_libsigrok(bool disable_trigger) {
  // Delegate to SessionStateContext::sync_trigger_to_libsigrok() — the actual
  // implementation called by CaptureManager. This wrapper exists for backward
  // compatibility but is not in the active call path (CaptureManager calls
  // _state->sync_trigger_to_libsigrok() directly).
  _state->sync_trigger_to_libsigrok(disable_trigger);
}

void SigSession::copy_data_to_document(data::SessionDocument *doc) {
  if (!doc || !_state->view_data() || !have_view_data())
    return;

  doc->set_samplerate(_state->view_data()->_cur_snap_samplerate);
  doc->set_samplelimits(_state->view_data()->_cur_samplelimits);
  doc->set_trigger_pos(_state->view_data()->_trig_pos);

  // Zero-copy: share the shared_ptr (increment ref count) instead of
  // deep-copying GB of snapshot data. Both SessionData and SessionDocument
  // now point to the same underlying snapshot. When SessionData::clear()
  // resets its shared_ptr (for the next capture), the snapshot stays alive
  // because SessionDocument still holds a reference.
  doc->share_from_logic(_state->view_data()->logic_shared());
  doc->share_from_analog(_state->view_data()->analog_shared());
  doc->share_from_dso(_state->view_data()->dso_shared());
}

void SigSession::attach_data_to_signal(SessionData *data) {
  _decode_task_manager->attach_data_to_signal(data);
}

// --- FilterProcessor forwarding wrappers ----------------------------------
void SigSession::set_glitch_filter(
    const std::map<int, uint32_t> &thresholds,
    const std::map<int, GlitchFilterMode> &filter_modes) {
  _filter_processor->set_glitch_filter(thresholds, filter_modes);
}
void SigSession::clear_glitch_filter() {
  _filter_processor->clear_glitch_filter();
}
bool SigSession::is_glitch_filter_active() {
  return _filter_processor->is_glitch_filter_active();
}

void SigSession::clear_glitch_filter_state_for_capture() {
  // 新采集开始时调用:清除滤波激活状态和 backup,
  // 但保留 thresholds/modes(供 auto-apply 使用)。
  // 不恢复数据 — _state->view_data()->get_logic() 已被 clear(),无数据可恢复。
  // Track B3: unique_ptr reset() replaces manual delete
  if (_state->view_data()->_logic_backup) {
    _state->view_data()->_logic_backup.reset();
  }
  if (_state->view_data()->_glitch_filter_active) {
    _state->view_data()->_glitch_filter_active = false;
    _event_bus->broadcast_async<interface::GlitchFilterCleared>({});
  }
}
void SigSession::set_signal_invert(const std::vector<bool> &channels) {
  _filter_processor->set_signal_invert(channels);
}
void SigSession::clear_signal_invert() {
  _filter_processor->clear_signal_invert();
}
bool SigSession::is_signal_invert_active() {
  return _filter_processor->is_signal_invert_active();
}

void SigSession::restart_decoders() {
  if (decode_traces().empty())
    return;

  // Stop running decoders
  clear_all_decode_task2();
  _capture_manager->clear_decode_result();

  // Copy current data to document for decoders
  auto doc =
      _document_registry->get_capture_owner_document()
          ? _document_registry->get_capture_owner_document()
          : _document_registry->get_active_document();
  if (doc) {
    copy_data_to_document(doc);
  }

  // restart_decoders() reuses the existing DecoderStack instances in place
  // (it does NOT create new ones, so they keep their handle_id). Bump the
  // version on each stack so API/MCP consumers can invalidate any cached
  // results bound to a prior version.
  for (auto stack : decode_traces()) {
    if (stack)
      stack->bump_version();
  }

  start_all_decode_tasks();
}

void SigSession::start_all_decode_tasks() {
  _decode_task_manager->start_all_decode_tasks();
}

// --- DecodeTaskManager forwarding wrappers --------------------------------
void SigSession::rst_decoder(int index, data::SessionDocument *doc) {
  _decode_task_manager->rst_decoder(index, doc);
}

void SigSession::rst_decoder_by_key_handel(void *handel,
                                           data::SessionDocument *doc) {
  _decode_task_manager->rst_decoder_by_key_handel(handel, doc);
}

void SigSession::remove_decode_task(
    std::shared_ptr<data::DecoderStack> stack) {
  _decode_task_manager->remove_decode_task(stack);
}

size_t SigSession::get_disk_write_queue_depth() {
  if (_state->view_data()->get_logic()->is_disk_cache_active())
    return _state->view_data()->get_logic()->get_disk_write_queue_depth();
  return 0;
}

double SigSession::get_disk_write_speed_mbps() {
  if (_state->view_data()->get_logic()->is_disk_cache_active())
    return _state->view_data()->get_logic()->get_disk_write_speed_mbps();
  return 0.0;
}

bool SigSession::is_disk_write_disk_full() { return false; }

// ============================================================================
// USB hotplug (libsigrok sr_listen_hotplug) — Tasks 9/10/11 + Task 4/5.
//
// Flow: libsigrok internal GThread -> hotplug_cb_ (static trampoline) ->
//   QMetaObject::invokeMethod(Qt::QueuedConnection) ->
//   on_hotplug_event_() (main thread, safe to touch Qt / EventBus).
//
// Reconnect tolerance: on DETACH during capture, start a 500ms watchdog.
// If ATTACH arrives before timeout, update_device_handle_() rebinds the
// active sdi to the re-enumerated device (matched by VID/PID). Otherwise
// stop_capture() + broadcast DeviceDetached.
//
// Device identification (Task 4): sr_hotplug_callback now receives the
// libusb_device* for BOTH ATTACH and DETACH (see hotplug.c). For DETACH,
// is_current_device_gone_() compares the detached device_handle pointer
// value against sr_dev_inst_libusb_device_get() of the active sdi — a pure
// value comparison that is safe even after libusb frees the underlying
// device (no dereference). For ATTACH, update_device_handle_() matches by
// VID/PID via sr_dev_inst_usb_vidpid_get() (the freshly-scanned sdis have
// no open handle, so pointer comparison is not possible there).
// ============================================================================

void SigSession::hotplug_cb_(int event, void *user_data, void *device_handle) {
  // Runs on a libsigrok internal GThread — MUST NOT touch Qt objects.
  // Forward to the main thread via Qt::QueuedConnection so on_hotplug_event_
  // can safely use QTimer / EventBus / DeviceAgent. device_handle is
  // captured by value (a raw libusb_device* for both ATTACH and DETACH).
  // Comparing the captured value later is safe even if libusb has since
  // freed the underlying device — only the pointer VALUE is compared,
  // never dereferenced.
  SigSession *self = static_cast<SigSession*>(user_data);
  if (!self)
    return;
  // SigSession is NOT a QObject — use EventBus::post_async_dispatch to
  // forward to the main thread. This uses QCoreApplication::postEvent
  // (not QMetaObject::invokeMethod) to avoid creating QThreadData on the
  // hotplug thread, which would crash on thread exit (see eventbus.h:100-111).
  // `self` is safe to capture: SigSession outlives libusb hotplug thread
  // (joined in uninit() before _state is destroyed).
  self->_event_bus->dispatch_async([self, event, device_handle]() {
    self->on_hotplug_event_(event, device_handle);
  });
}

void SigSession::on_hotplug_event_(int event, void *device_handle) {
  // Main thread — safe to touch Qt objects and the EventBus.
  if (event == SR_HOTPLUG_ATTACH) {
    // If the reconnect watchdog is active, this is the device returning
    // during an in-flight capture — stop the watchdog and rebind the sdi
    // to the freshly-enumerated device (matched by VID/PID).
    if (reconnect_timer_ && reconnect_timer_->isActive()) {
      reconnect_timer_->stop();
      update_device_handle_(device_handle);
      return;
    }
    pxv_info("Hotplug: device arrived");
    // Filter out unsupported USB devices (e.g. ESP32 CDC serial, keyboards).
    // libusb hotplug fires for ANY USB attach/detach. sr_driver_scan reallocates
    // sdi pointers on every call, so pointer comparison is unreliable — compare
    // connection_id (USB port path) instead, which is stable across rescans.
    auto &old_sdis = _state->device_agent().scanned_sdi();
    std::set<std::string> old_conn_ids;
    for (auto *sdi : old_sdis) {
      const char *cid = sr_dev_inst_connid_get(sdi);
      if (cid)
        old_conn_ids.insert(cid);
    }
    refresh_device_list();
    auto &new_sdis = _state->device_agent().scanned_sdi();
    bool has_new_device = false;
    for (auto *sdi : new_sdis) {
      const char *cid = sr_dev_inst_connid_get(sdi);
      if (cid && old_conn_ids.find(cid) == old_conn_ids.end()) {
        has_new_device = true;
        break;
      }
    }
    if (has_new_device) {
      _event_bus->broadcast_async<interface::UsbDeviceArrived>({});
    } else {
      pxv_info("Hotplug: device arrived but no new supported device found, ignoring");
    }
  } else if (event == SR_HOTPLUG_DETACH) {
    pxv_info("Hotplug: device detached");
    // Identify whether the detached device is the currently-open one by
    // comparing the libusb_device* pointer value (Task 4).
    if (!is_current_device_gone_(device_handle)) {
      // A different (non-current) device detached — refresh the device
      // list so the UI dropdown is up to date, but don't disturb the
      // current capture.
      refresh_device_list();
      return;
    }
    // Current device gone.
    if (_state->is_working()) {
      // Capture in flight — give the device a 500ms grace period to
      // re-enumerate (e.g. firmware re-download) before tearing down.
      start_reconnect_watchdog_();
    } else {
      // Idle — refresh list and notify immediately.
      refresh_device_list();
      _event_bus->broadcast_async<interface::DeviceDetached>({});
    }
  }
}

void SigSession::start_reconnect_watchdog_() {
  if (!reconnect_timer_) {
    // SigSession is NOT a QObject — QTimer cannot be parented to `this`.
    // unique_ptr manages lifetime. The connect context argument uses qApp
    // to ensure the lambda runs on the main thread.
    reconnect_timer_ = std::make_unique<QTimer>(nullptr);
    reconnect_timer_->setSingleShot(true);
    QObject::connect(reconnect_timer_.get(), &QTimer::timeout, qApp, [this]() {
      this->on_reconnect_timeout_();
    });
  }
  reconnect_timer_->start(500);  // 500ms grace period
  pxv_info("Device detached during capture, waiting 500ms for reconnect...");
}

void SigSession::on_reconnect_timeout_() {
  pxv_info("Reconnect timeout, stopping capture");
  if (_capture_manager) {
    _capture_manager->stop_capture();
  }
  refresh_device_list();
  _event_bus->broadcast_async<interface::DeviceDetached>({});
}

bool SigSession::is_current_device_gone_(void *device_handle) {
  // Conservative fallback: no device_handle means we can't identify the
  // detached device, so assume the worst (current device may be gone).
  // This preserves the pre-Task-4 behavior for any path that still passes
  // nullptr (e.g. a future libsigrok backend without libusb_device* support).
  if (!device_handle)
    return true;

  DeviceAgent *agent = get_device();
  // No active device — a DETACH can't affect us. Return false so the caller
  // only refreshes the device list without triggering the watchdog.
  if (!agent || !agent->have_instance())
    return false;

  // Get the current device's libusb_device*. This is a pointer-identity
  // comparison: even after libusb frees the underlying device (which
  // happens after the hotplug callback returns), comparing two pointer
  // VALUES is safe — no dereference is performed. If the values match, the
  // detached device IS the currently-open one.
  void *cur_dev = agent->get_libusb_device();
  // If we can't get the current device's libusb_device* (e.g. handle was
  // never opened, or non-USB device), be conservative.
  if (!cur_dev)
    return true;

  return cur_dev == device_handle;
}

void SigSession::update_device_handle_(void *device_handle) {
  // Task 5: rebind the active sdi to a freshly-scanned device matching the
  // current device's VID/PID. device_handle (the ATTACHed libusb_device*) is
  // reserved for future pointer-based matching; freshly-scanned sdis have
  // no open handle so pointer comparison isn't possible — VID/PID is the
  // reliable identity for a re-enumerated device.
  (void)device_handle;

  DeviceAgent *agent = get_device();
  if (!agent || !agent->have_instance()) {
    pxv_warn("update_device_handle_: no active device to rebind");
    return;
  }

  // Step 1: capture the current device's VID/PID and sdi pointer BEFORE any
  // state changes. The VID/PID is the identity to match against; old_sdi is
  // used to skip the stale entry in the refreshed list (the driver may or
  // may not free old sdis on re-scan — skipping by pointer avoids matching
  // a freed/reused entry). These must be captured now because release()
  // below clears _di.
  uint16_t cur_vid = 0, cur_pid = 0;
  if (!agent->get_vid_pid(cur_vid, cur_pid)) {
    pxv_warn("update_device_handle_: cannot get current VID/PID, stopping capture");
    stop_capture();
    set_default_device();
    return;
  }
  struct sr_dev_inst *old_sdi = agent->inst();

  // Step 2: stop any in-flight capture and release the old device. release()
  // closes the old sdi (sr_dev_close) and clears _di, so the subsequent
  // refresh_device_list() (which may free old sdis via sr_driver_scan) cannot
  // cause a use-after-free on _di. Doing release() BEFORE refresh is
  // critical: if we refreshed first, sr_driver_scan could free the old sdi
  // while _di still pointed to it, and release()'s sr_dev_close(_di) would
  // then be a use-after-free.
  if (_state->is_working())
    stop_capture();
  agent->release();

  // Step 3: refresh the scanned device list so the re-enumerated device is
  // present among the freshly-scanned sdis.
  refresh_device_list();

  // Step 4: find a freshly-scanned sdi matching the current VID/PID. Skip
  // the old sdi pointer (now closed; may have been freed by the driver
  // during re-scan — comparing the pointer VALUE is safe even if freed,
  // since no dereference is performed).
  const auto &sdis = agent->scanned_sdi();
  ds_device_handle new_handle = NULL_HANDLE;
  for (size_t i = 0; i < sdis.size(); i++) {
    struct sr_dev_inst *sdi = sdis[i];
    if (!sdi || sdi == old_sdi)
      continue;
    uint16_t vid = 0, pid = 0;
    if (sr_dev_inst_usb_vidpid_get(sdi, &vid, &pid) == SR_OK &&
        vid == cur_vid && pid == cur_pid) {
      new_handle = (ds_device_handle)(i + 1);  // handle = index + 1
      break;
    }
  }

  if (new_handle == NULL_HANDLE) {
    // Step 5: no match — fall back to default device (capture already
    // stopped above). set_default_device() will refresh + pick a device.
    pxv_warn("Device reconnected but vid:pid %04x:%04x not found in scanned list, stopping capture",
             cur_vid, cur_pid);
    set_default_device();
    return;
  }

  // Step 5: match found — open the new device (sr_dev_open inside
  // open_by_handle) and rebind DeviceAgent. open_by_handle creates a fresh
  // sr_session and re-registers the datafeed callback (stored in _datafeed_cb,
  // which survives release()). The in-flight capture was stopped in step 2
  // (the old USB handle was dead anyway); the user can restart capture to
  // resume with the new device.
  pxv_info("update_device_handle_: rebinding to vid:pid %04x:%04x (handle=%llu)",
           cur_vid, cur_pid, (unsigned long long)new_handle);
  if (!agent->open_by_handle(new_handle, _sr_ctx)) {
    pxv_err("update_device_handle_: open_by_handle failed, setting default device");
    set_default_device();
    return;
  }
  agent->update();
  pxv_info("Device reconnected, handle rebound");
}

} // namespace pv
