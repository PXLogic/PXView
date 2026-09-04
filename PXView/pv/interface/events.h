/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
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
// R3: carries document SLOT INDEX (SIZE_MAX = none) instead of raw pointers.
// The event is dispatched asynchronously and does not extend the document's
// lifetime; consumers resolve the index via DocumentRegistry (may get nullptr
// if the slot was released in the meantime).
struct CaptureOwnerChanged {
    size_t old_owner_index;
    size_t new_owner_index;
};

// TriggerConfigChanged — advanced/serial trigger config was rewritten.
// R3: no payload. The previous `const data::TriggerConfig*` only pointed at
// SigSession::_trigger_config and was valid solely for the duration of the
// synchronous dispatch; consumers re-query the current config instead of
// storing the pointer.
struct TriggerConfigChanged {};

// SampleCountUpdated — sample-depth / sample-count metadata changed.
struct SampleCountUpdated {
    uint64_t sample_count;
};

// DeviceOptionsUpdated — device options changed; signals need reload.
// skip_model_reload: set by TabContext::apply_device_intent() — the intent
// apply path has already run reload() explicitly; SigSession skips its
// redundant second full model rebuild (event-cascade convergence). All other
// broadcast sites leave it false (unchanged behavior).
struct DeviceOptionsUpdated {
    bool skip_model_reload = false;
};

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
// R3: slot indices (SIZE_MAX = none); see CaptureOwnerChanged.
struct ActiveDocumentChanged {
    size_t old_index;
    size_t new_index;
};

// CopyToDocDone — background copy of capture data into a document finished;
// decoders can now be started. R3: slot index (SIZE_MAX = none).
struct CopyToDocDone {
    size_t doc_index;
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

// DeviceChangeReason — why the current device changed. Lets GUI consumers
// (device-profile auto-load, view refresh) distinguish a user-initiated
// device selection from a tab-switch restore, whose per-tab document config
// is already the authoritative source and must NOT be clobbered by the
// device profile (demo0.pxc / per-hardware profile).
enum class DeviceChangeReason {
    FirstInit,      // App startup picking the default device
    UserSelection,  // User/API/auto-switch picked a device (identity change)
    TabSwitch       // TabContext::activate() restoring this tab's own device
};

// DeviceModeChanged — LOGIC/DSO/ANALOG work mode switched.
// reason: the DeviceChangeReason of the most recent device switch, stamped by
// switch_work_mode() from SigSession's recorded state — events carry their own
// semantics; no GUI-side state forwarding needed.
struct DeviceModeChanged {
    int mode;  // LOGIC/DSO/ANALOG
    DeviceChangeReason reason = DeviceChangeReason::UserSelection;
};

// CollectModeChanged — single/repeat/loop collect mode switched.
struct CollectModeChanged {
    int mode;  // DEVICE_COLLECT_MODE (COLLECT_SINGLE/COLLECT_REPEAT/COLLECT_LOOP)
};

// DeviceListUpdated — the device list changed.
struct DeviceListUpdated {};

// CurrentDeviceChanged — the current device selection changed.
// handle: the newly-activated device's handle, so consumers don't have to
// query the global active device (paves the way for per-tab DeviceSlot).
// unsigned long long == ds_device_handle (uint64_t) on this platform; kept
// literal to avoid a type dependency in this low-level header.
struct CurrentDeviceChanged {
    DeviceChangeReason reason = DeviceChangeReason::UserSelection;
    unsigned long long handle = 0;
};

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

// DecoderAnalogTriggerFound — post-decode analog display trigger found.
// Emitted when a TDM/PWM analog waveform trigger crossing is located.
struct DecoderAnalogTriggerFound {
    uint64_t sample_position;
    int display_position_percent;
    int channel;
    double level;
    uint64_t generation;
};

// DecoderAnalogTriggerDisplayHold — request atomic page-flip for display.
// When hold=true, viewport updates are frozen until the trigger position
// is committed. When hold=false, normal rendering resumes.
struct DecoderAnalogTriggerDisplayHold {
    bool hold;
    uint64_t generation;
};

} // namespace interface
} // namespace pv

#ifdef PXVIEW_EVENTS_UNDONE_INTERFACE
#  undef PXVIEW_EVENTS_UNDONE_INTERFACE
#endif

#endif // PXVIEW_PV_INTERFACE_EVENTS_H
