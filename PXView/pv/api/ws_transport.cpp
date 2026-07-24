#include "ws_transport.h"

#include <nlohmann/json.hpp>

#include "../core/eventbus.h"
#include <QCoreApplication>
#include <QHostAddress>
#include <QThread>

namespace pv::api {

WsTransport::WsTransport(IJsonRpcHandler* handler, int port)
    : QObject(nullptr)
    , _handler(handler)
    , _port(port)
{
}

WsTransport::~WsTransport()
{
    stop();
}

bool WsTransport::start()
{
    if (_server)
        return true;

    _server = new QWebSocketServer(QStringLiteral("PXView API"),
                                   QWebSocketServer::NonSecureMode, this);
    connect(_server, &QWebSocketServer::newConnection,
            this, &WsTransport::on_new_connection);

    if (!_server->listen(QHostAddress::LocalHost, _port)) {
        delete _server;
        _server = nullptr;
        return false;
    }

    return true;
}

void WsTransport::stop()
{
    if (!_server)
        return;

    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (auto* client : _clients) {
            client->close();
            client->deleteLater();
        }
        _clients.clear();
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
    }
}

void WsTransport::on_text_message(const QString& message)
{
    auto* client = qobject_cast<QWebSocket*>(sender());
    if (!client || !_handler)
        return;

    try {
        auto j = nlohmann::json::parse(message.toStdString());

        JsonRpcRequest req;
        req.method = j.value("method", std::string(""));
        req.params_json = j.contains("params") ? j["params"].dump() : "{}";
        req.id = j.value("id", 0);

        JsonRpcResponse resp = _handler->handle_request(req);

        nlohmann::json resp_json;
        resp_json["jsonrpc"] = "2.0";
        resp_json["id"] = resp.id;

        if (resp.success) {
            if (!resp.result_json.empty()) {
                resp_json["result"] = nlohmann::json::parse(resp.result_json);
            } else {
                resp_json["result"] = nullptr;
            }
        } else {
            if (!resp.error_json.empty()) {
                resp_json["error"] = nlohmann::json::parse(resp.error_json);
            } else {
                resp_json["error"] = {{"code", -1}, {"message", "Unknown error"}};
            }
        }

        client->sendTextMessage(QString::fromStdString(resp_json.dump()));
    } catch (const nlohmann::json::exception&) {
        nlohmann::json err;
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
    }

    client->deleteLater();
}

void WsTransport::send_to_clients(const QString& msg)
{
    // QWebSocket::sendTextMessage is not thread-safe; on_service_event may be
    // invoked from a worker thread (e.g. feed/device threads via
    // SessionService::broadcast_event). Marshal to the GUI thread first.
    //
    // CRITICAL: Use EventBus::post_async_dispatch (QCoreApplication::postEvent)
    // instead of QMetaObject::invokeMethod — see mcp_transport.cpp for details.
    if (!pv::core::EventBus::on_main_thread()) {
        pv::core::EventBus::post_async_dispatch(
            [this, msg]() { send_to_clients(msg); });
        return;
    }

    std::lock_guard<std::mutex> lock(_clients_mutex);
    for (auto* client : _clients) {
        client->sendTextMessage(msg);
    }
}

void WsTransport::on_service_event(const ServiceEventData& data)
{
    nlohmann::json notification;
    notification["type"] = "notification";

    nlohmann::json params;
    nlohmann::json params_map = nlohmann::json::object();
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
        // Map detail to a state string
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

    default:
        // Generic event notification
        notification["method"] = "on_event";
        params["event"] = static_cast<int32_t>(data.event);
        params["params"] = params_map;
        notification["params"] = params;
        break;
    }

    const auto msg = QString::fromStdString(notification.dump());

    // Marshal-aware broadcast (handles cross-thread invocation).
    send_to_clients(msg);
}

} // namespace pv::api
