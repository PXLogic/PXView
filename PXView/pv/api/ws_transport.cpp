#include "pv/api/ws_transport.h"

#include "pv/api/binary_codec.h"

#include <nlohmann/json.hpp>

#include "pv/core/eventbus.h"
#include "pv/core/qt_async_dispatcher.h"
#include <QCoreApplication>
#include <QHostAddress>
#include <QThread>
#include <QDateTime>
#include <QPointer>
#include <cstring>

namespace pv::api {

using json = nlohmann::json;

// P0-2: Global state version counter
uint64_t WsTransport::s_next_version = 0;

WsTransport::WsTransport(IJsonRpcHandler* handler, int port)
    : QObject(nullptr)
    , _handler(handler)
    , _port(port)
{
}

void WsTransport::customEvent(QEvent* event)
{
    if (event->type() == pv::core::QtAsyncDispatcher::AsyncEvent::eventType()) {
        auto* e = static_cast<pv::core::QtAsyncDispatcher::AsyncEvent*>(event);
        // Acquire-load pairs with the release-store in the AsyncEvent
        // constructor (see AsyncEvent::ready) — silences the TSan false
        // positive on the worker -> IO thread handoff.
        e->ready.load(std::memory_order_acquire);
        if (e->fn)
            e->fn();
        return;
    }
    QObject::customEvent(event);
}

WsTransport::~WsTransport()
{
    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
    // stop() is called here for cleanup. No derived class overrides it.
    stop();
}

bool WsTransport::start()
{
    if (_server)
        return true;

    // Create worker pool for offloading business logic.
    if (!_worker_pool)
        _worker_pool = std::make_unique<pv::core::ThreadPool>(2);

    _server = new QWebSocketServer(QStringLiteral("PXView API"),
                                   QWebSocketServer::NonSecureMode, this);
    connect(_server, &QWebSocketServer::newConnection,
            this, &WsTransport::on_new_connection);

    if (!_server->listen(QHostAddress::LocalHost, _port)) {
        delete _server;
        _server = nullptr;
        return false;
    }

    // P1-1: Start viewport push timer (~30fps = 33ms interval)
    _viewport_timer = new QTimer(this);
    _viewport_timer->setInterval(33);
    connect(_viewport_timer, &QTimer::timeout,
            this, &WsTransport::on_viewport_timer);
    _viewport_timer->start();

    return true;
}

void WsTransport::stop()
{
    // Shut down the worker pool first so no worker thread is accessing
    // a client while we clean up below.
    _worker_pool.reset();

    if (!_server)
        return;

    // L2 fix: disconnect signals before clearing clients
    disconnect(_server, &QWebSocketServer::newConnection,
               this, &WsTransport::on_new_connection);

    // P1-1: Stop viewport timer
    if (_viewport_timer) {
        _viewport_timer->stop();
        delete _viewport_timer;
        _viewport_timer = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (auto* client : _clients) {
            disconnect(client, &QWebSocket::textMessageReceived,
                       this, &WsTransport::on_text_message);
            disconnect(client, &QWebSocket::disconnected,
                       this, &WsTransport::on_client_disconnected);
            client->close();
            client->deleteLater();
        }
        _clients.clear();
        _client_states.clear();
    }

    _server->close();
    delete _server;
    _server = nullptr;
}

bool WsTransport::is_running() const
{
    return _server && _server->isListening();
}

void WsTransport::on_new_connection()
{
    QWebSocket* client = _server->nextPendingConnection();
    if (!client)
        return;

    connect(client, &QWebSocket::textMessageReceived,
            this, &WsTransport::on_text_message);
    connect(client, &QWebSocket::disconnected,
            this, &WsTransport::on_client_disconnected);

    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        _clients.insert(client);
        _client_states[client] = ClientState{};  // default: subscribed to all topics
    }
}

void WsTransport::on_text_message(const QString& message)
{
    auto* client = qobject_cast<QWebSocket*>(sender());
    if (!client || !_handler)
        return;

    try {
        auto j = json::parse(message.toStdString());

        std::string method = j.value("method", std::string(""));
        int id = j.value("id", 0);

        // ---- P0-1: Transport-level methods handled locally ----
        // These methods affect the transport layer's behavior and must be
        // handled here rather than forwarded to RpcDispatcher.
        if (method == "subscribe") {
            json params = j.contains("params") ? j["params"] : json::object();
            handle_subscribe(client, id, params);
            return;
        }
        if (method == "unsubscribe") {
            json params = j.contains("params") ? j["params"] : json::object();
            handle_unsubscribe(client, id, params);
            return;
        }

        // ---- P1-1/P1-2: Viewport subscription methods handled locally ----
        if (method == "subscribe_viewport") {
            json params = j.contains("params") ? j["params"] : json::object();
            handle_subscribe_viewport(client, id, params);
            return;
        }
        if (method == "update_viewport") {
            json params = j.contains("params") ? j["params"] : json::object();
            handle_update_viewport(client, id, params);
            return;
        }
        if (method == "unsubscribe_viewport") {
            handle_unsubscribe_viewport(client, id, j.contains("params") ? j["params"] : json::object());
            return;
        }

        // ---- Standard JSON-RPC routing ----
        JsonRpcRequest req;
        req.method = method;
        req.params_json = j.contains("params") ? j["params"].dump() : "{}";
        req.id = id;

        if (_worker_pool) {
            QPointer<QWebSocket> guard(client);
            _worker_pool->submit([this, req, guard]() {
                JsonRpcResponse resp = _handler->handle_request(req);
                // Post response back to IO thread for socket write
                post_to_self([this, guard, resp]() {
                    if (!guard)
                        return;
                    // P0-3: Check for binary response
                    if (resp.is_binary && !resp.binary_payload.empty()) {
                        json header = {
                            {"jsonrpc", "2.0"},
                            {"id", resp.id},
                            {"result", {
                                {"binary", true},
                                {"content_type", resp.binary_content_type},
                                {"size", resp.binary_payload.size()}
                            }}
                        };
                        guard->sendTextMessage(QString::fromStdString(header.dump()));
                        QByteArray bin_data(reinterpret_cast<const char*>(resp.binary_payload.data()),
                                           static_cast<int>(resp.binary_payload.size()));
                        guard->sendBinaryMessage(bin_data);
                        return;
                    }
                    // Standard JSON response
                    json resp_json;
                    resp_json["jsonrpc"] = "2.0";
                    resp_json["id"] = resp.id;
                    if (resp.success) {
                        if (!resp.result_json.empty()) {
                            resp_json["result"] = json::parse(resp.result_json);
                        } else {
                            resp_json["result"] = nullptr;
                        }
                    } else {
                        if (!resp.error_json.empty()) {
                            resp_json["error"] = json::parse(resp.error_json);
                        } else {
                            resp_json["error"] = {{"code", -1}, {"message", "Unknown error"}};
                        }
                    }
                    guard->sendTextMessage(QString::fromStdString(resp_json.dump()));
                });
            });
        } else {
            // Fallback: synchronous on IO thread (worker pool not initialized)
            JsonRpcResponse resp = _handler->handle_request(req);

            // P0-3: Check for binary response
            if (resp.is_binary && !resp.binary_payload.empty()) {
                json header = {
                    {"jsonrpc", "2.0"},
                    {"id", resp.id},
                    {"result", {
                        {"binary", true},
                        {"content_type", resp.binary_content_type},
                        {"size", resp.binary_payload.size()}
                    }}
                };
                client->sendTextMessage(QString::fromStdString(header.dump()));
                QByteArray bin_data(reinterpret_cast<const char*>(resp.binary_payload.data()),
                                   static_cast<int>(resp.binary_payload.size()));
                client->sendBinaryMessage(bin_data);
                return;
            }

            // Standard JSON response
            json resp_json;
            resp_json["jsonrpc"] = "2.0";
            resp_json["id"] = resp.id;
            if (resp.success) {
                if (!resp.result_json.empty()) {
                    resp_json["result"] = json::parse(resp.result_json);
                } else {
                    resp_json["result"] = nullptr;
                }
            } else {
                if (!resp.error_json.empty()) {
                    resp_json["error"] = json::parse(resp.error_json);
                } else {
                    resp_json["error"] = {{"code", -1}, {"message", "Unknown error"}};
                }
            }
            client->sendTextMessage(QString::fromStdString(resp_json.dump()));
        }
    } catch (const nlohmann::json::exception&) {
        json err;
        err["jsonrpc"] = "2.0";
        err["id"] = nullptr;
        err["error"] = {{"code", -32700}, {"message", "Parse error"}};
        client->sendTextMessage(QString::fromStdString(err.dump()));
    }
}

void WsTransport::on_client_disconnected()
{
    auto* client = qobject_cast<QWebSocket*>(sender());
    if (!client)
        return;

    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        _clients.erase(client);
        _client_states.erase(client);
    }

    client->deleteLater();
}

// ============================================================================
// P0-1: Topic subscription
// ============================================================================

bool WsTransport::client_subscribed_to(QWebSocket* client, const std::string& topic) const
{
    auto it = _client_states.find(client);
    if (it == _client_states.end())
        return true;  // unknown client — default to subscribed

    // Empty subscription set = subscribe to all (backward compatibility)
    if (it->second.subscribed_topics.empty())
        return true;

    return it->second.subscribed_topics.count(topic) > 0;
}

void WsTransport::handle_subscribe(QWebSocket* client, int id, const json& params)
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    auto& state = _client_states[client];

    if (params.contains("topics") && params["topics"].is_array()) {
        for (const auto& t : params["topics"]) {
            state.subscribed_topics.insert(t.get<std::string>());
        }
    }

    json resp = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"subscribed", state.subscribed_topics}
        }}
    };
    client->sendTextMessage(QString::fromStdString(resp.dump()));
}

void WsTransport::handle_unsubscribe(QWebSocket* client, int id, const json& params)
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    auto& state = _client_states[client];

    if (params.contains("topics") && params["topics"].is_array()) {
        for (const auto& t : params["topics"]) {
            state.subscribed_topics.erase(t.get<std::string>());
        }
    } else {
        // Unsubscribe from all
        state.subscribed_topics.clear();
    }

    json resp = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"subscribed", state.subscribed_topics}
        }}
    };
    client->sendTextMessage(QString::fromStdString(resp.dump()));
}

// ============================================================================
// P1-1/P1-2: Viewport subscription
// ============================================================================

void WsTransport::handle_subscribe_viewport(QWebSocket* client, int id, const json& params)
{
    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        auto& state = _client_states[client];
        state.viewport.start_sample = params.value("start_sample", uint64_t(0));
        state.viewport.end_sample = params.value("end_sample", uint64_t(0));
        state.viewport.width_px = params.value("width", 0);
        state.viewport.is_active = true;
        // P1-2: Reset delta tracking
        state.viewport.last_sent_sample = state.viewport.start_sample;

        // Parse channel list if provided
        state.viewport.channels.clear();
        if (params.contains("channels") && params["channels"].is_array()) {
            for (const auto& ch : params["channels"]) {
                state.viewport.channels.push_back(ch.get<int16_t>());
            }
        }
    }

    json resp = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {"ok"}}
    };
    client->sendTextMessage(QString::fromStdString(resp.dump()));
}

void WsTransport::handle_update_viewport(QWebSocket* client, int id, const json& params)
{
    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        auto& state = _client_states[client];
        if (!state.viewport.is_active) {
            json err = {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -1}, {"message", "No active viewport subscription"}}}
            };
            client->sendTextMessage(QString::fromStdString(err.dump()));
            return;
        }

        uint64_t new_start = params.value("start_sample", state.viewport.start_sample);
        // P1-2: If start_sample changed, reset delta tracking (full frame)
        if (new_start != state.viewport.start_sample) {
            state.viewport.last_sent_sample = new_start;
        }
        state.viewport.start_sample = new_start;
        state.viewport.end_sample = params.value("end_sample", state.viewport.end_sample);
        state.viewport.width_px = params.value("width", state.viewport.width_px);
    }

    json resp = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {"ok"}}
    };
    client->sendTextMessage(QString::fromStdString(resp.dump()));
}

void WsTransport::handle_unsubscribe_viewport(QWebSocket* client, int id, const json& /*params*/)
{
    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        auto& state = _client_states[client];
        state.viewport.is_active = false;
        state.viewport.last_sent_sample = 0;
    }

    json resp = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {"ok"}}
    };
    client->sendTextMessage(QString::fromStdString(resp.dump()));
}

// ============================================================================
// P1-1: Viewport timer — periodic data push
// ============================================================================

void WsTransport::on_viewport_timer()
{
    // Snapshot the subscription states under lock, then push without lock
    // to avoid holding the mutex during sendBinaryMessage calls.
    std::vector<std::pair<QWebSocket*, ViewportSubscription>> active_subs;

    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (auto& [client, state] : _client_states) {
            if (state.viewport.is_active) {
                active_subs.emplace_back(client, state.viewport);
            }
        }
    }

    // Push data to each subscribed client
    for (auto& [client, sub_copy] : active_subs) {
        push_viewport_data(client, sub_copy);

        // Update the last_sent_sample in the real state under lock
        std::lock_guard<std::mutex> lock(_clients_mutex);
        auto it = _client_states.find(client);
        if (it != _client_states.end()) {
            it->second.viewport.last_sent_sample = sub_copy.last_sent_sample;
        }
    }
}

void WsTransport::push_viewport_data(QWebSocket* client, ViewportSubscription& sub)
{
    // This is a lightweight push: we send a viewport_reset binary frame
    // to signal the client to prepare for new data, then the actual data
    // would be fetched via get_viewport_binary by the client on demand.
    //
    // The timer-driven push is intentionally minimal — it sends a "tick"
    // notification that new data may be available, and the client decides
    // when to request the actual binary frame. This avoids pushing large
    // binary payloads that the client may not be ready to consume.
    //
    // For real-time loop capture, the client should call get_viewport_binary
    // in response to the tick notification.

    uint32_t ts = static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

    // Send a small "data_available" JSON notification
    json tick = {
        {"type", "notification"},
        {"topic", "viewport_tick"},
        {"method", "on_viewport_data_available"},
        {"params", {
            {"start_sample", sub.start_sample},
            {"end_sample", sub.end_sample},
            {"width", sub.width_px},
            {"last_sent_sample", sub.last_sent_sample}
        }},
        {"version", ++s_next_version},
        {"timestamp", static_cast<int64_t>(ts)}
    };

    // Update last_sent_sample to current end_sample (delta tracking)
    sub.last_sent_sample = sub.end_sample;

    client->sendTextMessage(QString::fromStdString(tick.dump()));
}

// ============================================================================
// P0-3: Binary frame sending
// ============================================================================

void WsTransport::send_binary_to_client(QWebSocket* client, const std::vector<uint8_t>& payload)
{
    // Always post to IO thread — QWebSocket has thread affinity and must
    // only be written from the IO thread.
    QPointer<QWebSocket> guard(client);
    auto payload_copy = payload;
    post_to_self([this, guard, payload_copy = std::move(payload_copy)]() {
        if (!guard)
            return;
        QByteArray bin_data(reinterpret_cast<const char*>(payload_copy.data()),
                           static_cast<int>(payload_copy.size()));
        guard->sendBinaryMessage(bin_data);
    });
}

// ============================================================================
// P0-1/P0-2: Selective + versioned broadcast
// ============================================================================

void WsTransport::send_to_clients(const QString& msg)
{
    // Always post to IO thread — QWebSocket has thread affinity.
    post_to_self([this, msg]() {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (auto* client : _clients) {
            client->sendTextMessage(msg);
        }
    });
}

void WsTransport::send_to_client(QWebSocket* client, const QString& msg)
{
    // Always post to IO thread — QWebSocket has thread affinity.
    QPointer<QWebSocket> guard(client);
    post_to_self([this, guard, msg]() {
        if (!guard)
            return;
        guard->sendTextMessage(msg);
    });
}

// P0-2: Build a versioned notification JSON
json WsTransport::build_notification(const ServiceEventData& data) const
{
    json notification;
    notification["type"] = "notification";

    // P0-1: Add topic field
    std::string topic = service_event_topic(data.event);
    notification["topic"] = topic;

    json params;
    json params_map = json::object();
    for (const auto& [key, value] : data.params) {
        params_map[key] = value;
    }

    switch (data.event) {
    case ServiceEvent::CaptureProgress:
        notification["method"] = "on_capture_progress";
        params["progress"] = std::stoi(params_map.value("progress", "0"));
        notification["params"] = params;
        break;

    case ServiceEvent::DecodeProgress:
        notification["method"] = "on_decode_progress";
        params["decoder_id"] = params_map.value("instance_id", "");
        params["progress"] = std::stoi(params_map.value("progress", "0"));
        notification["params"] = params;
        break;

    case ServiceEvent::CaptureStateChanged: {
        notification["method"] = "on_capture_state_changed";
        std::string detail = params_map.value("detail", "");
        std::string state;
        if (detail == "collect_end" || detail == "end_collect" ||
            detail == "end_collect_prev")
            state = "stopped";
        else if (detail == "collect_start" || detail == "start_collect" ||
                 detail == "start_collect_prev")
            state = "running";
        else if (detail == "waiting_trigger")
            state = "waiting_trigger";
        else if (detail == "header_received")
            state = "capturing";
        else
            state = "unknown";
        params["state"] = state;
        notification["params"] = params;
        break;
    }

    case ServiceEvent::DataUpdated:
        notification["method"] = "on_data_updated";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::SampleConfigChanged:
        notification["method"] = "on_sample_config_changed";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::ChannelConfigChanged:
        notification["method"] = "on_channel_config_changed";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::TriggerConfigChanged:
        notification["method"] = "on_trigger_config_changed";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::LoadComplete:
        notification["method"] = "on_load_complete";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::SaveComplete:
        notification["method"] = "on_save_complete";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::ExportComplete:
        notification["method"] = "on_export_complete";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::DecodeDone:
        notification["method"] = "on_decode_done";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::TriggerReceived:
        notification["method"] = "on_trigger_received";
        params = params_map;
        notification["params"] = params;
        break;

    case ServiceEvent::FrameBegan:
        notification["method"] = "on_frame_began";
        notification["params"] = params_map;
        break;

    case ServiceEvent::FrameEnded:
        notification["method"] = "on_frame_ended";
        notification["params"] = params_map;
        break;

    case ServiceEvent::DeviceListUpdated:
        notification["method"] = "on_device_list_updated";
        notification["params"] = params_map;
        break;

    case ServiceEvent::DeviceDetached:
        notification["method"] = "on_device_detached";
        notification["params"] = params_map;
        break;

    case ServiceEvent::SignalsChanged:
        notification["method"] = "on_signals_changed";
        notification["params"] = params_map;
        break;

    default:
        notification["method"] = "on_event";
        params["event"] = static_cast<int32_t>(data.event);
        params["params"] = params_map;
        notification["params"] = params;
        break;
    }

    // P0-2: Version + timestamp (if not already set by SessionService)
    uint64_t version = data.version;
    int64_t ts = data.timestamp_ms;
    if (version == 0) version = ++s_next_version;
    if (ts == 0) ts = static_cast<int64_t>(QDateTime::currentMSecsSinceEpoch());

    notification["version"] = version;
    notification["timestamp"] = ts;

    return notification;
}

void WsTransport::on_service_event(const ServiceEventData& data)
{
    // Always post to IO thread — QWebSocket has thread affinity and must
    // only be written from the IO thread.
    post_to_self([this, data]() {
        json notification = build_notification(data);
        std::string topic = notification.value("topic", "misc");
        auto msg = QString::fromStdString(notification.dump());

        // Selective broadcast to subscribed clients
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (auto* client : _clients) {
            auto it = _client_states.find(client);
            bool subscribed = true;
            if (it != _client_states.end() && !it->second.subscribed_topics.empty()) {
                subscribed = it->second.subscribed_topics.count(topic) > 0;
            }
            if (subscribed) {
                client->sendTextMessage(msg);
            }
        }
    });
}

} // namespace pv::api
