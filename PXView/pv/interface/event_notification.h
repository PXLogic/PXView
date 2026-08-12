// event_notification.h — Unified event notification carrier with JSON payload.
//
// EventNotification replaces the ServiceEvent enum + params map<string,string>
// intermediate layer for external clients (MCP/WebSocket). It carries the
// event type as a string name, the event fields as a JSON payload, and
// routing metadata (topic, version, timestamp).
//
// Layer: Core (depends on nlohmann::json + STL).

#ifndef PXVIEW_INTERFACE_EVENT_NOTIFICATION_H
#define PXVIEW_INTERFACE_EVENT_NOTIFICATION_H

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>

namespace pv {
namespace interface {

/**
 * EventNotification — unified event notification for external clients.
 *
 * Field semantics:
 *   event_name    — stable string identifier (e.g. "CaptureStateChanged")
 *   payload       — JSON serialization of event fields (null for empty events)
 *   topic         — routing topic for selective subscription (e.g. "capture_state")
 *   version       — monotonically increasing version number for staleness detection
 *   timestamp_ms  — millisecond timestamp for dedup/ordering
 */
struct EventNotification {
    std::string     event_name;
    nlohmann::json  payload = nullptr;
    std::string     topic;
    uint64_t        version = 0;
    int64_t         timestamp_ms = 0;
};

// Serialize EventNotification itself (for JSON-RPC notification params).
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EventNotification,
    event_name, payload, topic, version, timestamp_ms)

} // namespace interface
} // namespace pv

#endif // PXVIEW_INTERFACE_EVENT_NOTIFICATION_H
