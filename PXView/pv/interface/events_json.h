// events_json.h — nlohmann::json serialization for typed events.
//
// This header provides to_json/from_json for events::interface event structs
// that have serializable fields. Events with raw pointer fields (SessionDocument*,
// SignalModel*, TriggerConfig*) are NOT serialized — they are marked
// HasPayload=false in event_map.def.
//
// Include this header only where JSON serialization is needed (transports,
// session_service notification dispatch). It is NOT included by events.h to
// avoid pulling nlohmann/json.hpp into every translation unit that uses events.
//
// Layer: Core (depends only on nlohmann::json + Qt6::Core for QString/quint64).

#ifndef PXVIEW_PV_INTERFACE_EVENTS_JSON_H
#define PXVIEW_PV_INTERFACE_EVENTS_JSON_H

#include <nlohmann/json.hpp>
#include <QString>

#include "pv/interface/events.h"

// ---- QString support for nlohmann::json ----
// A free `to_json`/`from_json` in `namespace nlohmann` is NOT enough on system
// nlohmann 3.11.x: there `json` lives behind an inline ABI namespace
// (json_abi_v3_11_3), so the unqualified call from the macro-generated code
// fails ADL. The version-robust, officially recommended mechanism is to
// specialize `nlohmann::adl_serializer<QString>` (see below).
namespace nlohmann {
// Specialize adl_serializer for QString so any nlohmann::json <-> QString
// conversion (e.g. a NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE member of type QString)
// works on every nlohmann build — including system 3.11.x, which hides `json`
// behind an inline ABI namespace (json_abi_v3_11_3) that breaks ADL lookup of a
// free `to_json` declared in `namespace nlohmann`. Specializing adl_serializer is
// the version-robust, officially recommended mechanism (no reliance on ADL).
template <>
struct adl_serializer<QString> {
    static void to_json(json& j, const QString& s) {
        j = s.toStdString();
    }
    static void from_json(const json& j, QString& s) {
        s = QString::fromStdString(j.get<std::string>());
    }
};
} // namespace nlohmann

namespace pv {
namespace interface {

// ---- Events with simple fields: auto-generate to_json / from_json ----

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CaptureStateChanged, is_working, device_status)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TriggerReceived, trigger_pos)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeviceModeChanged, mode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DsoViewOptionChanged, channel_index)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CollectModeChanged, mode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CopyInProgressChanged, in_progress)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SampleCountUpdated, sample_count)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GlitchFilterProgress, progress)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DataLenUpdated, length)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ShowRegion, start, end, keep)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RepeatHold, percent)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DelayedPropMsg, message)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeviceOpenFailed, driver_name, error_message)

// ---- Empty events: serialize as JSON null ----
// These are not strictly needed (HasPayload=false skips serialization),
// but provided for completeness so any event can be passed to nlohmann::json(ev).
#define PXVIEW_DEFINE_EMPTY_EVENT_JSON(EventName) \
    inline void to_json(nlohmann::json& j, const EventName&) { j = nullptr; } \
    inline void from_json(const nlohmann::json&, EventName&) {}

PXVIEW_DEFINE_EMPTY_EVENT_JSON(DataUpdated)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(DecodeDone)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(DeviceListUpdated)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(DeviceConfigUpdated)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(DeviceDetached)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(UsbDeviceArrived)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(CurrentDeviceChanged)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(SampleRateChanged)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(SaveComplete)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(StartCollectWork)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(CollectStart)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(CollectEnd)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(EndCollectWork)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(RevEndPacket)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(DataPoolChanged)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(SimpleTriggerChanged)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(GlitchFilterStarted)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(GlitchFilterCompleted)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(GlitchFilterCleared)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(SignalInvertStarted)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(SignalInvertCompleted)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(SignalInvertCleared)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(TrigNextCollect)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(ClearDecodeData)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(StoreConfPrev)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(StartCollectWorkPrev)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(EndCollectWorkPrev)
PXVIEW_DEFINE_EMPTY_EVENT_JSON(CurrentDeviceChangePrev)

#undef PXVIEW_DEFINE_EMPTY_EVENT_JSON

} // namespace interface
} // namespace pv

#endif // PXVIEW_PV_INTERFACE_EVENTS_JSON_H
