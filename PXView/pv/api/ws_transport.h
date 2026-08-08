#pragma once

#include "transport.h"
#include "types.h"

#include <nlohmann/json.hpp>

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QTimer>

#include <set>
#include <map>
#include <mutex>
#include <string>

namespace pv::api {

// ============================================================================
// WsTransport — WebSocket transport for PXView API.
//
// Upgraded with P0 + P1 features:
//   P0-1: Topic-based selective subscription (subscribe/unsubscribe)
//   P0-2: Versioned state notifications (version + timestamp in every push)
//   P0-3: Binary frame transmission (sendBinaryMessage for waveform data)
//   P1-1: Viewport subscription with periodic push (subscribe_viewport + timer)
//   P1-2: Delta frame support (only push new data beyond last_sent_sample)
//   P1-3: Batch call routing (handled by RpcDispatcher)
// ============================================================================

class WsTransport : public QObject, public ITransport, public IServiceEventListener {
    Q_OBJECT

public:
    WsTransport(IJsonRpcHandler* handler, int port = 10430);
    ~WsTransport();

    bool start() override;
    void stop() override;
    bool is_running() const override;

    // IServiceEventListener — broadcast events to subscribed clients
    void on_service_event(const ServiceEventData& data) override;

private slots:
    void on_new_connection();
    void on_text_message(const QString& message);
    void on_client_disconnected();
    // P1-1: Periodic viewport data push timer
    void on_viewport_timer();

private:
    // Marshal-aware broadcast helpers
    void send_to_clients(const QString& msg);
    void send_to_client(QWebSocket* client, const QString& msg);

    // P0-1: Topic subscription — check if a client is subscribed to a topic
    bool client_subscribed_to(QWebSocket* client, const std::string& topic) const;

    // P0-1: Handle subscribe/unsubscribe locally (not forwarded to RpcDispatcher)
    void handle_subscribe(QWebSocket* client, int id, const nlohmann::json& params);
    void handle_unsubscribe(QWebSocket* client, int id, const nlohmann::json& params);

    // P1-1/P1-2: Viewport subscription handlers
    void handle_subscribe_viewport(QWebSocket* client, int id, const nlohmann::json& params);
    void handle_update_viewport(QWebSocket* client, int id, const nlohmann::json& params);
    void handle_unsubscribe_viewport(QWebSocket* client, int id, const nlohmann::json& params);

    // P0-2: Build notification JSON with version/timestamp
    nlohmann::json build_notification(const ServiceEventData& data) const;

    // P1-1: Push viewport data to a single subscribed client
    void push_viewport_data(QWebSocket* client, ViewportSubscription& sub);

    // P0-3/P1-2: Send a binary frame to a client (marshalled to main thread)
    void send_binary_to_client(QWebSocket* client, const std::vector<uint8_t>& payload);

    IJsonRpcHandler* _handler;
    int _port;
    QWebSocketServer* _server = nullptr;

    // Client set and per-client subscription state
    struct ClientState {
        // P0-1: Topics this client has subscribed to.
        // Empty set means "subscribe to all" (backward compatibility).
        std::set<std::string> subscribed_topics;
        // P1-1: Active viewport subscription
        ViewportSubscription viewport;
    };

    std::map<QWebSocket*, ClientState> _client_states;
    std::set<QWebSocket*> _clients;
    mutable std::mutex _clients_mutex;

    // P1-1: Timer for periodic viewport data push (~30fps = 33ms)
    QTimer* _viewport_timer = nullptr;

    // P0-2: Global state version counter (monotonically increasing)
    // Set from SessionService::broadcast_event via ServiceEventData.version,
    // but also tracked here for events that don't carry a version.
    static uint64_t s_next_version;
};

} // namespace pv::api
