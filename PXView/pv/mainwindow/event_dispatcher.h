// mainwindow_event_dispatcher.h
#ifndef PXVIEW_PV_MAINWINDOW_EVENT_DISPATCHER_H
#define PXVIEW_PV_MAINWINDOW_EVENT_DISPATCHER_H

#include <memory>
#include <QPointer>

#include "pv/interface/events.h"

namespace pv {
namespace api { struct ServiceEventData; }

class MainWindow;
class View;

// Forward declaration so the safe_current_view() signature does not depend on
// the full view/view.h include order (event_dispatcher.h may be parsed before
// mainwindow.h declares pv::view::View).
namespace view {
class View;
}

// SessionEventDispatcher — delegate for MainWindow's IEventListener
// implementation. Extracted during Phase 2 of the view-layer modernization
// to reduce MainWindow's responsibilities. All 45 typed on_event overrides
// live here; MainWindow holds a unique_ptr to this delegate and forwards
// the IEventListener interface to it.
//
// The dispatcher is a friend of MainWindow so it can access private members
// (toolbars, docks, session, device_agent, etc.) directly — same pattern as
// MainWindowConfigIO.
class SessionEventDispatcher : public pv::interface::IEventListener {
public:
    explicit SessionEventDispatcher(MainWindow *window) : _window(window) {}

    // P0/P1: Safely resolve the current view. Returns nullptr if the owning
    // MainWindow has been destroyed (QPointer) or no tab is active, so callers
    // MUST null-check before dereferencing.
    pv::view::View *safe_current_view() const;

    // --- IEventListener: 45 typed event overrides ---
    void on_event(const pv::interface::CaptureStateChanged &) override;
    void on_event(const pv::interface::CaptureOwnerChanged &) override;
    void on_event(const pv::interface::TriggerConfigChanged &) override;
    void on_event(const pv::interface::SampleCountUpdated &) override;
    void on_event(const pv::interface::DeviceOptionsUpdated &) override;
    void on_event(const pv::interface::DsoViewOptionChanged &) override;
    void on_event(const pv::interface::ActiveDocumentChanged &) override;
    void on_event(const pv::interface::CopyToDocDone &) override;
    void on_event(const pv::interface::DecodeDone &) override;
    void on_event(const pv::interface::SignalsChanged &) override;
    void on_event(const pv::interface::DataUpdated &) override;
    void on_event(const pv::interface::DeviceModeChanged &) override;
    void on_event(const pv::interface::CollectModeChanged &) override;
    void on_event(const pv::interface::DeviceListUpdated &) override;
    void on_event(const pv::interface::CurrentDeviceChanged &) override;
    void on_event(const pv::interface::DeviceOpenFailed &) override;
    void on_event(const pv::interface::UsbDeviceArrived &) override;
    void on_event(const pv::interface::DeviceDetached &) override;
    void on_event(const pv::interface::SampleRateChanged &) override;
    void on_event(const pv::interface::SaveComplete &) override;
    void on_event(const pv::interface::StartCollectWork &) override;
    void on_event(const pv::interface::CollectStart &) override;
    void on_event(const pv::interface::CollectEnd &) override;
    void on_event(const pv::interface::EndCollectWork &) override;
    void on_event(const pv::interface::EndDeviceOptions &) override;
    void on_event(const pv::interface::DeviceConfigUpdated &) override;
    void on_event(const pv::interface::DemoModeChanged &) override;
    void on_event(const pv::interface::DataPoolChanged &) override;
    void on_event(const pv::interface::SimpleTriggerChanged &) override;
    void on_event(const pv::interface::GlitchFilterStarted &) override;
    void on_event(const pv::interface::GlitchFilterProgress &) override;
    void on_event(const pv::interface::GlitchFilterCompleted &) override;
    void on_event(const pv::interface::GlitchFilterCleared &) override;
    void on_event(const pv::interface::SignalInvertStarted &) override;
    void on_event(const pv::interface::SignalInvertCompleted &) override;
    void on_event(const pv::interface::SignalInvertCleared &) override;
    void on_event(const pv::interface::CopyInProgressChanged &) override;
    void on_event(const pv::interface::TrigNextCollect &) override;
    void on_event(const pv::interface::ClearDecodeData &) override;
    void on_event(const pv::interface::AppOptionsChanged &) override;
    void on_event(const pv::interface::FontOptionsChanged &) override;
    void on_event(const pv::interface::ShortcutChanged &) override;
    void on_event(const pv::interface::StyleChanged &) override;
    void on_event(const pv::interface::StoreConfPrev &) override;
    void on_event(const pv::interface::CurrentDeviceChangePrev &) override;
    void on_event(const pv::interface::StartCollectWorkPrev &) override;
    void on_event(const pv::interface::EndCollectWorkPrev &) override;

    // Spec v2 Task 7: Events migrated from ISessionCallback dispatch_to<>
    void on_event(const pv::interface::DataLenUpdated &) override;
    void on_event(const pv::interface::HeaderReceived &) override;
    void on_event(const pv::interface::CaptureUpdated &) override;
    void on_event(const pv::interface::ShowRegion &) override;
    void on_event(const pv::interface::RepeatHold &) override;
    void on_event(const pv::interface::TriggerReceived &) override;
    void on_event(const pv::interface::ShowWaitTrigger &) override;
    void on_event(const pv::interface::SessionError &) override;
    void on_event(const pv::interface::SaveRequested &) override;
    void on_event(const pv::interface::DelayedPropMsg &) override;
    void on_event(const pv::interface::SampleLimitsChanged &) override;

    // --- IServiceEventListener forwarding ---
    // Routes View operation broadcasts from SessionService (MCP/WS API)
    // to the active View. MainWindow::on_service_event forwards here.
    void on_service_event(const pv::api::ServiceEventData &data);

    // --- Phase 2: additional delegated logic ---
    // Session error dialog display (moved from MainWindow::on_session_error).
    void handle_session_error();
    // USB device speed check (moved from MainWindow::check_usb_device_speed).
    void check_usb_device_speed();

private:
    QPointer<MainWindow> _window;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_EVENT_DISPATCHER_H
