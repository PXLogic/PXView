/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
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

// Typed event bus for PXView.
//
// STATUS (modernize-core-layer-radical — Task 12 complete):
//   * The legacy IMessageListener / DSV_MSG_* / broadcast_msg / trigger_message
//     infrastructure has been COMPLETELY REMOVED. All dispatch now goes through
//     broadcast<T>() (sync), broadcast_sync<T>() (sync direct), or
//     broadcast_async<T>() (async via Qt::QueuedConnection).
//   * 4 pre/post ordering codes (CurrentDeviceChangePrev / StartCollectWorkPrev
//     / EndCollectWorkPrev / StoreConfPrev) are emitted via broadcast_sync<T>()
//     — synchronous direct dispatch, no Qt::QueuedConnection queue. Callers
//     MUST be on the main thread.
//   * MainWindow overrides ALL 45 on_event(const T&) virtuals (41 original +
//     StoreConfPrev + CurrentDeviceChangePrev + StartCollectWorkPrev +
//     EndCollectWorkPrev). Each override contains its handler body directly
//     (no int dispatch, no switch).
//   * DataUpdated is emitted by DataFeedParser::feed_in_* (radical Task 13).
//   * The "new code MUST use IEventListener" hard constraint is in effect
//     (see AGENTS.md "Typed event bus (HARD CONSTRAINT — C1+ complete)").
//
// This header defines a set of semantic event structs and the IEventListener
// interface. Each event carries its full context as typed fields rather than a
// bare (int msg, int param) pair, so consumers cannot accidentally mis-handle a
// message code or forget a payload.
//
// New code MUST register an IEventListener with SigSession and override only
// the event handlers it cares about.
//
// Layer: this is a Core-layer header. It may depend only on Qt6::Core and the
// STL — it MUST NOT include QWidget/QMainWindow/QDialog or any pv/view/*.h.

#ifndef PXVIEW_PV_INTERFACE_EVENTS_H
#define PXVIEW_PV_INTERFACE_EVENTS_H

#include <cstdint>

#include <memory>
#include <QString>
#include <vector>

#include "pv/data/triggerconfig.h"  // for pv::data::TriggerConfig (complete type)

// The Windows SDK (shobjidl.h / objbase.h) defines `interface` as a
// preprocessor macro for COM interface declarations. This conflicts with the
// `namespace interface` declaration below. When this header is included into a
// translation unit that has already pulled in the Windows SDK (e.g. via
// mainframe.h -> wintaskbarprogress.h -> shobjidl.h), the macro is cleared so
// the namespace declaration parses correctly. The macro is NOT restored
// afterwards: PXView's own code does not use the `interface` COM macro
// anywhere (verified by grep), and restoring it would break any subsequent
// `pv::interface::` qualified name in the including file (e.g. mainwindow.h's
// `public pv::interface::IEventListener` base-class clause). Windows SDK
// headers that define `interface` carry include guards, so re-including them
// after this header will not re-define the macro anyway.
#ifdef interface
#  undef interface
#  define PXVIEW_EVENTS_UNDONE_INTERFACE 1
#endif

namespace pv {

// Forward declarations to avoid heavy includes. SessionDocument pulls in many
// Core data headers; a pointer member only needs the forward declaration.
namespace data {
class SessionDocument;
class SignalModel;
}

namespace interface {

// ---------------------------------------------------------------------------
// Semantic event structs.
//
// Field type notes:
//   * device_status holds a DEVICE_STATUS_TYPE value (ST_INIT/ST_RUNNING/
//     ST_STOPPED). That enum lives in sigsession.h (pv namespace). To keep
//     events.h decoupled from sigsession.h (which would create a circular
//     include — sigsession.h includes events.h), the field is typed as int.
//   * mode fields similarly hold DEVICE_COLLECT_MODE / work-mode integer
//     constants and are typed as int for the same reason.
// ---------------------------------------------------------------------------

// CaptureStateChanged — capture started / stopped.
struct CaptureStateChanged {
    bool is_working;
    int  device_status;  // DEVICE_STATUS_TYPE (ST_INIT/ST_RUNNING/ST_STOPPED)
};

// CaptureOwnerChanged — the SessionDocument owning the live capture changed.
struct CaptureOwnerChanged {
    data::SessionDocument *old_owner;
    data::SessionDocument *new_owner;
};

// TriggerConfigChanged — advanced/serial trigger config was rewritten.
// config points at SigSession::_trigger_config and is only valid for the
// duration of the dispatch (do not store).
struct TriggerConfigChanged {
    const data::TriggerConfig *config;
};

// SampleCountUpdated — sample-depth / sample-count metadata changed.
struct SampleCountUpdated {
    uint64_t sample_count;
};

// DeviceOptionsUpdated — device options changed; signals need reload.
struct DeviceOptionsUpdated {};

// DsoViewOptionChanged — DSO view-layer option changed from header interaction
// (vDial/factor/acCoupling). Unlike DeviceOptionsUpdated, this does NOT trigger
// reload()/rebuild_signals() because go_vDial*/set_factor/set_acCoupling have
// already synced driver (set_config_*), Core model (model->set_vdiv/...), and
// View state (_stop_scale/_scale/_vDial/_acCoupling). A rebuild here would
// drop View-only state (_stop_scale resets to 1 in path-B full rebuild,
// causing the waveform to no longer scale with vdiv — see go_vDialPre/Next).
// Listeners: MainWindow refreshes docks + persists config; SigSession skips
// reload; SessionService forwards to MCP/WS clients.
struct DsoViewOptionChanged {
    int channel_index; // -1 = unspecified / batch
    DsoViewOptionChanged(int idx = -1) : channel_index(idx) {}
};

// ActiveDocumentChanged — the active SessionDocument switched.
struct ActiveDocumentChanged {
    data::SessionDocument *old_doc;
    data::SessionDocument *new_doc;
};

// CopyToDocDone — background copy of capture data into a document finished;
// decoders can now be started.
struct CopyToDocDone {
    data::SessionDocument *doc;
};

// Decode task finished.
struct DecodeDone {};

// Signal list changed.
struct SignalsChanged {
    enum class RebuildKind {
        AllReplaced,
        Modified,
        Added,
        Removed
    };
    RebuildKind rebuild_kind = RebuildKind::AllReplaced;
    std::vector<std::shared_ptr<data::SignalModel>> new_model_ptrs;
};

// Underlying sample data updated.
struct DataUpdated {};

// DeviceModeChanged — LOGIC/DSO/ANALOG work mode switched.
struct DeviceModeChanged {
    int mode;  // LOGIC/DSO/ANALOG
};

// CollectModeChanged — single/repeat/loop collect mode switched.
struct CollectModeChanged {
    int mode;  // DEVICE_COLLECT_MODE (COLLECT_SINGLE/COLLECT_REPEAT/COLLECT_LOOP)
};

// DeviceListUpdated — the device list changed.
struct DeviceListUpdated {};

// CurrentDeviceChanged — the current device selection changed.
struct CurrentDeviceChanged {};

// DeviceOpenFailed — set_device() failed to open the new device via sr_dev_open.
// The old device has already been released (CurrentDeviceChangePrev ran), so
// _dev_handle is nullptr. Carries the driver name and error string for UI feedback.
// Without this event, the UI stays blank and 1000+ "_dev_handle is nullptr"
// warnings flood the log with no actionable message.
struct DeviceOpenFailed {
    std::string driver_name;
    std::string error_message;
};

// UsbDeviceArrived — a USB device arrived.
struct UsbDeviceArrived {};

// DeviceDetached — the current device was detached.
struct DeviceDetached {};

// SampleRateChanged — sample-rate / device duration changed.
struct SampleRateChanged {};

// SaveComplete — save operation finished.
struct SaveComplete {};

// StartCollectWork — capture starting.
struct StartCollectWork {};

// CollectStart — collection started.
struct CollectStart {};

// CollectEnd — collection ended.
struct CollectEnd {};

// EndCollectWork — capture fully stopped.
struct EndCollectWork {};

// SessionStopped — libsigrok session has fully stopped (sr_session_run()
// returned). Emitted from DeviceAgent's worker thread via
// IDeviceAgentCallback::DeviceSessionStopped, then re-broadcast_async by
// SigSession so listeners run on the main thread. This is the upstream
// equivalent of fork libsigrok's DS_EV_COLLECT_TASK_END — the reliable
// "session really stopped" signal that SR_DF_END cannot provide (at
// SR_DF_END time the GLib main loop is still draining, so a subsequent
// sr_session_start() can race on session->running / main_context state).
// CaptureManager listens for this event to release the CaptureOwnerGuard
// (which sets _is_working=false and broadcasts EndCollectWork) for both
// auto-stop and manual-stop paths.
struct SessionStopped {};

// RevEndPacket — capture-end packet received from libsigrok; Core swaps the
// capture/view buffer, kicks off copy-to-doc and starts decoders. Emitted from
// the libsigrok data-feed worker thread (DataFeedParser).
struct RevEndPacket {};

// EndDeviceOptions — device options batch update ended.
struct EndDeviceOptions {};

// DeviceConfigUpdated — device config changed.
struct DeviceConfigUpdated {};

// DemoModeChanged — demo operation mode changed.
struct DemoModeChanged {};

// DataPoolChanged — data pool swapped.
struct DataPoolChanged {};

// SimpleTriggerChanged — simple trigger (edge) changed.
struct SimpleTriggerChanged {};

// GlitchFilterStarted — glitch filter task started.
struct GlitchFilterStarted {};

// GlitchFilterProgress — glitch filter progress update.
struct GlitchFilterProgress {
    int progress;  // 0-100
};

// GlitchFilterCompleted — glitch filter task completed.
struct GlitchFilterCompleted {};

// GlitchFilterCleared — glitch filter cleared.
struct GlitchFilterCleared {};

// SignalInvertStarted — signal invert task started.
struct SignalInvertStarted {};

// SignalInvertCompleted — signal invert task completed.
struct SignalInvertCompleted {};

// SignalInvertCleared — signal invert cleared.
struct SignalInvertCleared {};

// CopyInProgressChanged — copy thread state changed.
struct CopyInProgressChanged {
    bool in_progress;
};

// TrigNextCollect — trigger next collection (repeat mode).
struct TrigNextCollect {};

// ClearDecodeData — decode data cleared.
struct ClearDecodeData {};

// AppOptionsChanged — app options changed.
struct AppOptionsChanged {};

// FontOptionsChanged — font options changed.
struct FontOptionsChanged {};

// ShortcutChanged — shortcut changed.
struct ShortcutChanged {};

// StyleChanged — style changed.
struct StyleChanged {};

// DS_EV_DEVICE_SPEED_NOT_MATCH — device USB speed too low; Core surfaces a
// delayed user-facing message. Emitted from the device event callback thread.
struct DeviceSpeedNotMatch {};

// modernize-core-layer-radical Task 10: StoreConfPrev pre-broadcast ordering
// event. Emitted synchronously via broadcast_sync() BEFORE SigSession commits
// a config-store mutation, so observers can read the pre-mutation state.
struct StoreConfPrev {};

// modernize-core-layer-radical Task 11: pre-broadcast ordering events for the
// remaining 3 PREV codes. Each is emitted synchronously via broadcast_sync()
// BEFORE the corresponding state mutation. Callers MUST be on the main thread
// (broadcast_sync is synchronous direct dispatch, no Qt::QueuedConnection).
struct CurrentDeviceChangePrev {};
struct StartCollectWorkPrev {};
struct EndCollectWorkPrev {};

// --- Spec v2 Task 7: Events migrated from ISessionCallback dispatch_to<> ---

// DataLenUpdated — received data length changed (was IDataCallback::receive_data_len).
struct DataLenUpdated {
    quint64 length;
};

// HeaderReceived — capture header received (was IDataCallback::receive_header).
struct HeaderReceived {};

// CaptureUpdated — capture state updated (was ICaptureCallback::update_capture).
struct CaptureUpdated {};

// ShowRegion — show a region of the capture (was ICaptureCallback::show_region).
struct ShowRegion {
    uint64_t start;
    uint64_t end;
    bool keep;
};

// RepeatHold — repeat-hold progress (was ICaptureCallback::repeat_hold).
struct RepeatHold {
    int percent;
};

// TriggerReceived — trigger fired at given position (was ITriggerCallback::receive_trigger).
struct TriggerReceived {
    quint64 trigger_pos;
};

// ShowWaitTrigger — show wait-trigger UI (was ITriggerCallback::show_wait_trigger).
struct ShowWaitTrigger {};

// SessionError — session error occurred (was ISessionStateCallback::session_error).
struct SessionError {};

// SaveRequested — session save requested (was ISessionStateCallback::session_save).
struct SaveRequested {};

// DelayedPropMsg — delayed property message (was ISessionStateCallback::delay_prop_msg).
struct DelayedPropMsg {
    QString message;
};

// SampleLimitsChanged — sample limits changed (was ICaptureCallback::cur_samplelimits_changed).
struct SampleLimitsChanged {};

// Note on DataUpdated: modernize-core-layer-radical Task 13 wired the emitter.
// It is now broadcast directly from DataFeedParser::feed_in_logic /
// feed_in_dso / feed_in_analog after each successful sample-data feed-in.

// IEventListener has been removed. Event consumers now use
// EventBus::subscribe<T>(lambda) with RAII Subscription management.
// See pv/core/eventbus.h for the new API.

} // namespace interface
} // namespace pv

#ifdef PXVIEW_EVENTS_UNDONE_INTERFACE
#  undef PXVIEW_EVENTS_UNDONE_INTERFACE
#endif

#endif // PXVIEW_PV_INTERFACE_EVENTS_H
