// mainwindow_event_dispatcher.h
#ifndef PXVIEW_PV_MAINWINDOW_EVENT_DISPATCHER_H
#define PXVIEW_PV_MAINWINDOW_EVENT_DISPATCHER_H

#include <memory>
#include <vector>
#include <QPointer>
#include <QTimer>

#include "pv/interface/events.h"
#include "pv/core/eventbus.h"

namespace pv {
namespace api { struct ServiceEventData; }

class MainWindow;
class View;

namespace view {
class View;
}

// SessionEventDispatcher — handles all session events for MainWindow.
//
// IEventListener has been removed. Events are registered via
// EventBus::subscribe<T>() in the constructor, with RAII Subscription
// management. The dispatcher accesses MainWindow's private members
// (friend class).
class SessionEventDispatcher {
public:
    SessionEventDispatcher(MainWindow *window, core::EventBus *bus);
    ~SessionEventDispatcher() = default;

    pv::view::View *safe_current_view() const;

    void on_service_event(const pv::api::ServiceEventData &data);
    void handle_session_error();
    void check_usb_device_speed();

private:
    QPointer<MainWindow> _window;
    core::EventBus *_bus;
    std::vector<core::Subscription> _subscriptions;

    // Event handler methods (called from subscribe<T>() lambdas)
    void on_capture_state_changed(const interface::CaptureStateChanged &);
    void on_capture_owner_changed(const interface::CaptureOwnerChanged &);
    void on_trigger_config_changed(const interface::TriggerConfigChanged &);
    void on_sample_count_updated(const interface::SampleCountUpdated &);
    void on_device_options_updated(const interface::DeviceOptionsUpdated &);
    void on_dso_view_option_changed(const interface::DsoViewOptionChanged &);
    void on_active_document_changed(const interface::ActiveDocumentChanged &);
    void on_copy_to_doc_done(const interface::CopyToDocDone &);
    void on_decode_done(const interface::DecodeDone &);
    void on_signals_changed(const interface::SignalsChanged &);
    void on_data_updated(const interface::DataUpdated &);
    void on_device_mode_changed(const interface::DeviceModeChanged &);
    void on_collect_mode_changed(const interface::CollectModeChanged &);
    void on_device_list_updated(const interface::DeviceListUpdated &);
    void on_current_device_changed(const interface::CurrentDeviceChanged &);
    void on_device_open_failed(const interface::DeviceOpenFailed &);
    void on_usb_device_arrived(const interface::UsbDeviceArrived &);
    void on_device_detached(const interface::DeviceDetached &);
    void on_sample_rate_changed(const interface::SampleRateChanged &);
    void on_save_complete(const interface::SaveComplete &);
    void on_start_collect_work(const interface::StartCollectWork &);
    void on_collect_start(const interface::CollectStart &);
    void on_collect_end(const interface::CollectEnd &);
    void on_end_collect_work(const interface::EndCollectWork &);
    void on_end_device_options(const interface::EndDeviceOptions &);
    void on_device_config_updated(const interface::DeviceConfigUpdated &);
    void on_demo_mode_changed(const interface::DemoModeChanged &);
    void on_data_pool_changed(const interface::DataPoolChanged &);
    void on_simple_trigger_changed(const interface::SimpleTriggerChanged &);
    void on_glitch_filter_started(const interface::GlitchFilterStarted &);
    void on_glitch_filter_progress(const interface::GlitchFilterProgress &);
    void on_glitch_filter_completed(const interface::GlitchFilterCompleted &);
    void on_glitch_filter_cleared(const interface::GlitchFilterCleared &);
    void on_signal_invert_started(const interface::SignalInvertStarted &);
    void on_signal_invert_completed(const interface::SignalInvertCompleted &);
    void on_signal_invert_cleared(const interface::SignalInvertCleared &);
    void on_copy_in_progress_changed(const interface::CopyInProgressChanged &);
    void on_trig_next_collect(const interface::TrigNextCollect &);
    void on_clear_decode_data(const interface::ClearDecodeData &);
    void on_app_options_changed(const interface::AppOptionsChanged &);
    void on_font_options_changed(const interface::FontOptionsChanged &);
    void on_shortcut_changed(const interface::ShortcutChanged &);
    void on_style_changed(const interface::StyleChanged &);
    void on_store_conf_prev(const interface::StoreConfPrev &);
    void on_current_device_change_prev(const interface::CurrentDeviceChangePrev &);
    void on_start_collect_work_prev(const interface::StartCollectWorkPrev &);
    void on_end_collect_work_prev(const interface::EndCollectWorkPrev &);
    void on_data_len_updated(const interface::DataLenUpdated &);
    void on_header_received(const interface::HeaderReceived &);
    void on_capture_updated(const interface::CaptureUpdated &);
    void on_show_region(const interface::ShowRegion &);
    void on_repeat_hold(const interface::RepeatHold &);
    void on_trigger_received(const interface::TriggerReceived &);
    void on_show_wait_trigger(const interface::ShowWaitTrigger &);
    void on_session_error(const interface::SessionError &);
    void on_save_requested(const interface::SaveRequested &);
    void on_delayed_prop_msg(const interface::DelayedPropMsg &);
    void on_sample_limits_changed(const interface::SampleLimitsChanged &);

    // Throttle timer for SignalsChanged events: when multiple
    // SignalsChanged arrive in rapid succession (e.g. when MCP adds
    // 16 decoders), only process the first one and defer it by 50ms.
    // Subsequent events during the deferral period are coalesced.
    // This prevents N full signal-layout passes (each O(M) where M
    // is the signal count) from running back-to-back on the main thread.
    QTimer _signals_changed_timer;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_EVENT_DISPATCHER_H
