#include "pv/api/mcp_transport.h"

#include <nlohmann/json.hpp>

#include "pv/core/eventbus.h"
#include "pv/core/qt_async_dispatcher.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <atomic>
#include <chrono>
#include <thread>

using json = nlohmann::json;

namespace {

// Map ServiceEvent enum to a stable string identifier for JSON-RPC notifications.
const char* service_event_to_string(pv::api::ServiceEvent event)
{
    switch (event) {
    case pv::api::ServiceEvent::CaptureStateChanged:    return "CaptureStateChanged";
    case pv::api::ServiceEvent::DataUpdated:            return "DataUpdated";
    case pv::api::ServiceEvent::TriggerReceived:        return "TriggerReceived";
    case pv::api::ServiceEvent::FrameBegan:             return "FrameBegan";
    case pv::api::ServiceEvent::FrameEnded:             return "FrameEnded";
    case pv::api::ServiceEvent::CaptureProgress:        return "CaptureProgress";
    case pv::api::ServiceEvent::DeviceListUpdated:      return "DeviceListUpdated";
    case pv::api::ServiceEvent::DeviceModeChanged:      return "DeviceModeChanged";
    case pv::api::ServiceEvent::DeviceConfigChanged:    return "DeviceConfigChanged";
    case pv::api::ServiceEvent::DeviceDetached:         return "DeviceDetached";
    case pv::api::ServiceEvent::NewUsbDevice:           return "NewUsbDevice";
    case pv::api::ServiceEvent::GlitchFilterStarted:    return "GlitchFilterStarted";
    case pv::api::ServiceEvent::GlitchFilterProgress:   return "GlitchFilterProgress";
    case pv::api::ServiceEvent::GlitchFilterCompleted:  return "GlitchFilterCompleted";
    case pv::api::ServiceEvent::GlitchFilterCleared:    return "GlitchFilterCleared";
    case pv::api::ServiceEvent::SignalInvertStarted:    return "SignalInvertStarted";
    case pv::api::ServiceEvent::SignalInvertCompleted:  return "SignalInvertCompleted";
    case pv::api::ServiceEvent::SignalInvertCleared:    return "SignalInvertCleared";
    case pv::api::ServiceEvent::DecodeDone:             return "DecodeDone";
    case pv::api::ServiceEvent::DecoderAdded:           return "DecoderAdded";
    case pv::api::ServiceEvent::DecoderRemoved:         return "DecoderRemoved";
    case pv::api::ServiceEvent::DecodeProgress:         return "DecodeProgress";
    case pv::api::ServiceEvent::SampleConfigChanged:    return "SampleConfigChanged";
    case pv::api::ServiceEvent::ChannelConfigChanged:   return "ChannelConfigChanged";
    case pv::api::ServiceEvent::TriggerConfigChanged:   return "TriggerConfigChanged";
    case pv::api::ServiceEvent::SaveComplete:           return "SaveComplete";
    case pv::api::ServiceEvent::LoadComplete:           return "LoadComplete";
    case pv::api::ServiceEvent::ExportComplete:         return "ExportComplete";
    case pv::api::ServiceEvent::SignalsChanged:         return "SignalsChanged";
    case pv::api::ServiceEvent::ViewShowRegion:         return "ViewShowRegion";
    case pv::api::ServiceEvent::ViewZoomFit:            return "ViewZoomFit";
    case pv::api::ServiceEvent::ViewZoomIn:             return "ViewZoomIn";
    case pv::api::ServiceEvent::ViewZoomOut:            return "ViewZoomOut";
    case pv::api::ServiceEvent::ViewCursorAdded:        return "ViewCursorAdded";
    case pv::api::ServiceEvent::ViewCursorRemoved:      return "ViewCursorRemoved";
    case pv::api::ServiceEvent::ViewCursorsCleared:     return "ViewCursorsCleared";
    case pv::api::ServiceEvent::ErrorOccurred:          return "ErrorOccurred";
    }
    return "Unknown";
}

} // namespace

namespace pv::api {

McpTransport::McpTransport(IJsonRpcHandler* handler, int port)
    : QObject(nullptr), _handler(handler), _port(port)
{
}

void McpTransport::customEvent(QEvent* event)
{
    if (event->type() == pv::core::QtAsyncDispatcher::AsyncEvent::eventType()) {
        auto* e = static_cast<pv::core::QtAsyncDispatcher::AsyncEvent*>(event);
        if (e->fn)
            e->fn();
        return;
    }
    QObject::customEvent(event);
}

McpTransport::~McpTransport()
{
    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
    // stop() is called here for cleanup. No derived class overrides it.
    stop();
}

bool McpTransport::start()
{
    if (_server && _server->isListening())
        return true;

    // Create worker pool for offloading business logic.
    if (!_worker_pool)
        _worker_pool = std::make_unique<pv::core::ThreadPool>(2);

    _server = new QTcpServer(this);
    connect(_server, &QTcpServer::newConnection,
            this, &McpTransport::on_new_connection);

    if (!_server->listen(QHostAddress::LocalHost, _port)) {
        delete _server;
        _server = nullptr;
        return false;
    }

    return true;
}

void McpTransport::stop()
{
    // 0. Shut down the worker pool first so no worker thread is accessing
    //    a socket while we clean up below.  Workers that are blocked in
    //    run_string_on_main_thread will have their posted events processed
    //    by the main thread event loop (which is still running at this point).
    _worker_pool.reset();

    // 1. Clean up SSE clients first to prevent on_service_event from
    //    writing to sockets that are about to be destroyed.
    {
        std::lock_guard<std::mutex> lock(_sse_clients_mutex);
        for (auto* socket : _sse_clients) {
            if (socket) {
                // L3 fix: disconnect signals before abort to prevent
                // any queued slot invocations from touching the socket.
                disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                disconnect(socket, &QTcpSocket::disconnected, socket, nullptr);
                socket->abort();
                delete socket;
            }
        }
        _sse_clients.clear();
    }

    // 2. Clean up pending sockets (half-read HTTP requests)
    for (auto* socket : _pending_sockets) {
        if (socket) {
            disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            socket->abort();
            delete socket;
        }
    }
    _pending_sockets.clear();

    // 3. Now safe to destroy the server
    if (_server) {
        disconnect(_server, &QTcpServer::newConnection,
                   this, &McpTransport::on_new_connection);
        _server->close();
        delete _server;
        _server = nullptr;
    }
}

bool McpTransport::is_running() const
{
    return _server && _server->isListening();
}

void McpTransport::on_service_event(const ServiceEventData& data)
{
    // MCP transport sockets live on the IO thread. Always post the actual
    // work (JSON building + socket writes) to the IO thread via post_to_self.
    //
    // CRITICAL: Use post_to_self (QCoreApplication::postEvent(this, ...))
    // instead of QMetaObject::invokeMethod. invokeMethod internally calls
    // QThread::currentThread() which creates a QThreadData on the worker thread
    // → SIGSEGV on thread exit (LdrShutdownThread). postEvent only accesses
    // the receiver's QThreadData — safe for worker threads.
    post_to_self([this, data]() {
        // Build a JSON-RPC notification payload.
        json notification;
        notification["jsonrpc"] = "2.0";
        notification["method"] = "event";

        json params;
        params["type"] = service_event_to_string(data.event);

        json params_map = json::object();
        for (const auto& [key, value] : data.params) {
            params_map[key] = value;
        }
        params["data"] = params_map;
        notification["params"] = params;

        const std::string payload = notification.dump();

        // Push to every client currently holding an open SSE stream (e.g. a
        // wait_capture in progress). The IO thread event loop processes this
        // queued lambda while the SSE stream is still open.
        // HTTP MCP clients without an open SSE stream cannot be pushed to
        // (HTTP is request/response); they will see the updated state on
        // their next request.
        std::lock_guard<std::mutex> lock(_sse_clients_mutex);
        // Build a list of alive sockets, and prune dead ones in a single pass
        // to prevent use-after-free if a socket was disconnected but not yet
        // removed from _sse_clients.
        QList<QTcpSocket*> alive_sockets;
        QSet<QTcpSocket*> dead_sockets;
        for (auto* socket : _sse_clients) {
            if (socket && socket->state() == QAbstractSocket::ConnectedState) {
                alive_sockets.append(socket);
            } else {
                dead_sockets.insert(socket);
            }
        }
        // Remove dead sockets from the set so we never touch them again
        for (auto* s : dead_sockets) {
            _sse_clients.remove(s);
        }
        // Send only to verified-alive sockets
        for (auto* socket : alive_sockets) {
            send_sse_event(socket, "event", payload);
        }
    });
}

void McpTransport::on_new_connection()
{
    while (_server->hasPendingConnections()) {
        QTcpSocket* socket = _server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead,
                this, &McpTransport::on_ready_read);
        connect(socket, &QTcpSocket::disconnected,
                socket, &QTcpSocket::deleteLater);
    }
}

void McpTransport::on_ready_read()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;

    // Check if we already have a pending read for this socket
    if (_pending_sockets.contains(socket))
        return;

    // Try to read a complete HTTP request
    try_handle_request(socket);
}

void McpTransport::try_handle_request(QTcpSocket* socket)
{
    QByteArray data = socket->readAll();

    // Find header/body boundary
    int header_end = data.indexOf("\r\n\r\n");
    if (header_end < 0) {
        // Incomplete headers — wait for more data
        _pending_sockets.insert(socket);
        // Disconnect the current readyRead and connect a one-shot handler
        // NOTE: Do NOT use Qt::UniqueConnection with lambdas — Qt silently
        // rejects the connection (prints a warning) and the socket ends up
        // with no readyRead handler at all, causing it to hang forever.
        disconnect(socket, &QTcpSocket::readyRead, this, &McpTransport::on_ready_read);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            // Append new data
            QByteArray more = socket->readAll();
            // Accumulate in socket property
            QByteArray accumulated = socket->property("_http_buffer").toByteArray();
            accumulated.append(more);
            socket->setProperty("_http_buffer", accumulated);

            // Check if we have complete headers now
            int he = accumulated.indexOf("\r\n\r\n");
            if (he < 0) return; // Still incomplete

            // Check Content-Length
            int content_length = 0;
            QList<QByteArray> header_lines = accumulated.left(he).split('\n');
            for (int i = 1; i < header_lines.size(); ++i) {
                QByteArray line = header_lines[i].trimmed();
                if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
                    content_length = line.mid(15).trimmed().toInt();
                    break;
                }
            }

            QByteArray body = accumulated.mid(he + 4);
            if (content_length > 0 && body.size() < content_length) {
                return; // Body incomplete, wait for more
            }

            // Complete request received
            _pending_sockets.remove(socket);
            socket->setProperty("_http_buffer", QByteArray());
            // Reconnect the normal handler
            disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            connect(socket, &QTcpSocket::readyRead, this, &McpTransport::on_ready_read);
            handle_http_request(socket, accumulated);
        });
        socket->setProperty("_http_buffer", data);
        return;
    }

    // We have headers — check Content-Length
    int content_length = 0;
    QList<QByteArray> header_lines = data.left(header_end).split('\n');
    for (int i = 1; i < header_lines.size(); ++i) {
        QByteArray line = header_lines[i].trimmed();
        if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            content_length = line.mid(15).trimmed().toInt();
            break;
        }
    }

    QByteArray body = data.mid(header_end + 4);
    if (content_length > 0 && body.size() < content_length) {
        // Body incomplete — wait for more data
        _pending_sockets.insert(socket);
        disconnect(socket, &QTcpSocket::readyRead, this, &McpTransport::on_ready_read);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, content_length]() {
            QByteArray accumulated = socket->property("_http_buffer").toByteArray();
            accumulated.append(socket->readAll());
            socket->setProperty("_http_buffer", accumulated);

            int he = accumulated.indexOf("\r\n\r\n");
            if (he < 0) return;

            QByteArray body = accumulated.mid(he + 4);
            if (body.size() < content_length) return;

            // Complete
            _pending_sockets.remove(socket);
            socket->setProperty("_http_buffer", QByteArray());
            disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            connect(socket, &QTcpSocket::readyRead, this, &McpTransport::on_ready_read);
            handle_http_request(socket, accumulated);
        });
        socket->setProperty("_http_buffer", data);
        return;
    }

    // Complete request available
    handle_http_request(socket, data);
}

void McpTransport::handle_http_request(QTcpSocket* socket, const QByteArray& data)
{
    // Find header/body boundary
    int header_end = data.indexOf("\r\n\r\n");
    if (header_end < 0) {
        send_http_response(socket, 400,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Invalid HTTP request\"},\"id\":null}");
        return;
    }

    QByteArray header_part = data.left(header_end);
    QByteArray body = data.mid(header_end + 4);

    // Parse request line
    QList<QByteArray> request_lines = header_part.split('\n');
    if (request_lines.isEmpty()) {
        send_http_response(socket, 400,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Malformed request\"},\"id\":null}");
        return;
    }

    QByteArray request_line = request_lines[0].trimmed();
    QList<QByteArray> parts = request_line.split(' ');
    if (parts.size() < 2) {
        send_http_response(socket, 400,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Malformed request line\"},\"id\":null}");
        return;
    }

    QByteArray http_method = parts[0];

    // Handle GET requests — serve static files from webui/ directory
    if (http_method == "GET") {
        QByteArray path = parts[1].trimmed();

        // Default to index.html for root path
        if (path == "/")
            path = "/index.html";

        // Security: reject paths with ".." to prevent directory traversal
        if (path.contains("..")) {
            send_http_response(socket, 403, "Forbidden");
            return;
        }

        // Map to filesystem
        QString file_path = QCoreApplication::applicationDirPath()
                            + "/webui" + QString::fromUtf8(path);
        QFile file(file_path);
        if (!file.open(QIODevice::ReadOnly)) {
            send_http_response(socket, 404, "Not Found", "text/plain");
            return;
        }
        QByteArray file_data = file.readAll();
        file.close();

        // Determine MIME type from extension
        QByteArray mime_type = "application/octet-stream";
        if (file_path.endsWith(".html")) mime_type = "text/html; charset=utf-8";
        else if (file_path.endsWith(".js")) mime_type = "application/javascript; charset=utf-8";
        else if (file_path.endsWith(".mjs")) mime_type = "application/javascript; charset=utf-8";
        else if (file_path.endsWith(".css")) mime_type = "text/css; charset=utf-8";
        else if (file_path.endsWith(".svg")) mime_type = "image/svg+xml";
        else if (file_path.endsWith(".json")) mime_type = "application/json";
        else if (file_path.endsWith(".png")) mime_type = "image/png";
        else if (file_path.endsWith(".ico")) mime_type = "image/x-icon";
        else if (file_path.endsWith(".woff")) mime_type = "font/woff";
        else if (file_path.endsWith(".woff2")) mime_type = "font/woff2";

        send_http_response(socket, 200, file_data, mime_type.constData());
        return;
    }

    // Handle CORS preflight
    if (http_method == "OPTIONS") {
        send_http_response(socket, 200, "");
        return;
    }

    // Only POST is supported
    if (http_method != "POST") {
        send_http_response(socket, 405,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Method not allowed, use POST\"},\"id\":null}");
        return;
    }

    // Read Content-Length (already validated by try_handle_request, but parse for reference)
    int content_length = -1;
    for (int i = 1; i < request_lines.size(); ++i) {
        QByteArray line = request_lines[i].trimmed();
        if (line.startsWith("Content-Length:") || line.startsWith("content-length:")) {
            QByteArray value = line.mid(15).trimmed();
            content_length = value.toInt();
        }
    }
    (void)content_length;

    // Parse JSON-RPC request
    json j;
    try {
        j = json::parse(body.toStdString());
    } catch (const json::parse_error&) {
        send_http_response(socket, 400,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"},\"id\":null}");
        return;
    } catch (const std::exception&) {
        send_http_response(socket, 400,
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"},\"id\":null}");
        return;
    }

    // Extract method
    std::string method;
    if (j.contains("method") && j["method"].is_string())
        method = j["method"].get<std::string>();

    // Check if this is a notification (no "id" field) — MCP notifications/initialized
    bool has_id = j.contains("id");
    if (!has_id && method.rfind("notifications/", 0) == 0) {
        // MCP notification — return HTTP 204 with no body
        send_http_204(socket);
        return;
    }

    // Build JsonRpcRequest
    JsonRpcRequest req;
    req.method = method;
    req.is_mcp = true;
    req.has_id = has_id;

    if (has_id) {
        if (j["id"].is_number_integer())
            req.id = j["id"].get<int>();
        else if (j["id"].is_string())
            req.id = 0;
    }

    // For MCP tools/call, extract tool name and arguments from params
    if (method == "tools/call") {
        if (j.contains("params") && j["params"].is_object()) {
            auto& p = j["params"];
            if (p.contains("name") && p["name"].is_string())
                req.mcp_tool_name = p["name"].get<std::string>();
            if (p.contains("arguments") && p["arguments"].is_object())
                req.mcp_tool_args = p["arguments"].dump();
            else
                req.mcp_tool_args = "{}";
        }
        req.params_json = req.mcp_tool_args;

        // Check if this is a wait_capture tool call — use SSE streaming.
        // Submit to worker pool so the blocking wait_capture_complete()
        // (SharedState::wait, cv — no Qt event pumping) blocks a worker
        // thread, not the IO thread. The IO thread remains free to process
        // SSE events (on_service_event, progress_thread) and other requests.
        if (req.mcp_tool_name == "wait_capture") {
            QPointer<QTcpSocket> socket_guard(socket);
            disconnect(socket, &QTcpSocket::readyRead, this, &McpTransport::on_ready_read);
            _worker_pool->submit([this, req, socket_guard]() {
                handle_sse_wait_capture(socket_guard, req);
            });
            return;
        }
    } else {
        // For other methods (initialize, tools/list, ping), pass params as-is
        if (j.contains("params"))
            req.params_json = j["params"].dump();
    }

    // Dispatch to handler.
    //
    // Phase 2 (worker thread): RpcDispatcher::handle_request + response JSON
    // building.  SessionService methods that need the main thread use
    // run_string_on_main_thread() internally, so the worker blocks while
    // the main thread processes Qt object creation — but the main thread
    // remains free to handle UI events (paint, input, queued EventBus
    // events) in between.
    //
    // Phase 3 (main thread): send_http_response on the socket.
    //
    // If the worker pool is not initialized (e.g. start() not called), fall
    // back to synchronous execution on the main thread.
    if (_worker_pool) {
        QPointer<QTcpSocket> socket_guard(socket);
        // Disconnect readyRead to prevent re-entrant on_ready_read calls
        // while the worker is processing this request.
        disconnect(socket, &QTcpSocket::readyRead, this, &McpTransport::on_ready_read);

        _worker_pool->submit([this, req, socket_guard]() {
            // Phase 2: Business logic + response building (worker thread)
            JsonRpcResponse resp = _handler->handle_request(req);
            QByteArray resp_body = build_mcp_response_body(resp, req);

            // Phase 3: Post response back to IO thread for socket write
            post_to_self([this, resp_body, socket_guard]() {
                auto* s = socket_guard.data();
                if (!s)
                    return; // socket was deleted while worker ran
                send_http_response(s, 200, resp_body);
            });
        });
    } else {
        // Fallback: synchronous on main thread (worker pool not initialized)
        JsonRpcResponse resp = _handler->handle_request(req);
        QByteArray resp_body = build_mcp_response_body(resp, req);
        send_http_response(socket, 200, resp_body);
    }
}

// ---------------------------------------------------------------------------
// SSE support
// ---------------------------------------------------------------------------

void McpTransport::send_sse_headers(QTcpSocket* socket)
{
    QByteArray response;
    response.append("HTTP/1.1 200 OK\r\n");
    response.append("Content-Type: text/event-stream\r\n");
    response.append("Cache-Control: no-cache\r\n");
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n");
    response.append("Access-Control-Allow-Headers: Content-Type\r\n");
    response.append("\r\n");

    socket->write(response);
    socket->flush();
}

void McpTransport::send_sse_event(QTcpSocket* socket,
                                   const std::string& event,
                                   const std::string& data)
{
    QByteArray sse;
    sse.append("event: " + QByteArray::fromStdString(event) + "\n");
    sse.append("data: " + QByteArray::fromStdString(data) + "\n");
    sse.append("\n");

    socket->write(sse);
    socket->flush();
}

void McpTransport::send_sse_done(QTcpSocket* socket,
                                  const std::string& event,
                                  const std::string& final_result_json)
{
    // Send the final result as an SSE event
    send_sse_event(socket, event, final_result_json);

    // Close the connection
    socket->disconnectFromHost();
}

void McpTransport::handle_sse_wait_capture(QPointer<QTcpSocket> socket_guard,
                                            const JsonRpcRequest& req)
{
    // This function runs on a worker thread (submitted via _worker_pool).
    // The IO thread is free to process posted events (SSE headers, progress,
    // service events) while we block in handle_request below.

    // Send SSE headers — post to IO thread for socket write
    post_to_self([this, socket_guard]() {
        if (socket_guard)
            send_sse_headers(socket_guard);
    });

    // Register this socket as an active SSE subscriber so that
    // on_service_event can push JSON-RPC notifications (CaptureStateChanged,
    // SampleConfigChanged, ...) to the client while wait_capture is running.
    // Safe to do on the worker thread — _sse_clients is protected by mutex.
    {
        std::lock_guard<std::mutex> lock(_sse_clients_mutex);
        _sse_clients.insert(socket_guard.data());
    }

    // Parse timeout from arguments
    double timeout_seconds = 300.0;
    JsonRpcRequest tool_req = req; // mutable copy: timeoutMs is injected below
    try {
        json args = json::parse(req.mcp_tool_args);
        if (args.contains("timeoutSeconds") && args["timeoutSeconds"].is_number())
            timeout_seconds = args["timeoutSeconds"].get<double>();
        else if (args.contains("timeout_seconds") && args["timeout_seconds"].is_number())
            timeout_seconds = args["timeout_seconds"].get<double>();
        // Inject as timeoutMs so the wait_capture tool handler (which reads
        // p.get_or<uint64_t>("timeoutMs", 300000)) honors the client's
        // timeout. Without this, clients sending "timeoutSeconds" (the
        // pxview-automation client does) always hit the 300 s default,
        // turning any stuck capture into a multi-minute hang.
        if (!args.contains("timeoutMs") && timeout_seconds > 0) {
            args["timeoutMs"] = static_cast<uint64_t>(timeout_seconds * 1000.0);
            tool_req.mcp_tool_args = args.dump();
        }
    } catch (const std::exception& e) { (void)e; }

    // Progress pusher: runs on a separate std::thread, posts SSE events
    // to the IO thread for socket write (no cross-thread socket access).
    int elapsed_ms = 0;
    const int progress_interval_ms = 500;
    std::atomic<bool> progress_done{false};

    std::thread progress_thread([&]() {
        while (!progress_done.load()) {
            json progress_data;
            progress_data["status"] = "capturing";
            progress_data["elapsed_seconds"] = elapsed_ms / 1000.0;
            // Post SSE event to IO thread — no direct cross-thread socket write
            post_to_self([this, socket_guard, progress_data]() {
                if (socket_guard)
                    send_sse_event(socket_guard, "progress", progress_data.dump());
            });
            elapsed_ms += progress_interval_ms;
            // Sleep in small increments to check progress_done promptly
            for (int i = 0; i < progress_interval_ms / 50 && !progress_done.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // This call blocks using SharedState::wait() — no Qt event queue pumping.
    // The IO thread is free to process posted events (on_service_event,
    // progress_thread SSE events) while we wait.
    JsonRpcResponse resp = _handler->handle_request(tool_req);

    progress_done.store(true);
    progress_thread.join();

    // Build the final MCP response
    json resp_json;
    resp_json["jsonrpc"] = "2.0";
    resp_json["id"] = req.id;

    if (resp.success) {
        json content_item;
        content_item["type"] = "text";
        if (!resp.result_json.empty()) {
            content_item["text"] = resp.result_json;
        } else {
            content_item["text"] = "Capture complete";
        }
        resp_json["result"] = {
            {"content", json::array({content_item})}
        };
    } else {
        // Error response
        std::string error_text;
        if (!resp.error_json.empty()) {
            error_text = resp.error_json;
        } else {
            error_text = "Capture wait failed or timed out";
        }
        resp_json["result"] = {
            {"content", json::array({{{"type", "text"}, {"text", error_text}}})},
            {"isError", true}
        };
    }

    // Unregister before closing so on_service_event stops pushing to this
    // socket (send_sse_done will disconnect the host).
    {
        std::lock_guard<std::mutex> lock(_sse_clients_mutex);
        _sse_clients.remove(socket_guard.data());
    }

    // Send the final result as an SSE event and close — post to IO thread
    std::string final_json = resp_json.dump();
    post_to_self([this, socket_guard, final_json]() {
        if (socket_guard)
            send_sse_done(socket_guard, "result", final_json);
    });
}

QByteArray McpTransport::build_mcp_response_body(const JsonRpcResponse& resp,
                                                   const JsonRpcRequest& req)
{
    json resp_json;
    resp_json["jsonrpc"] = "2.0";
    resp_json["id"] = req.id;

    if (resp.is_mcp_direct) {
        if (!resp.result_json.empty()) {
            resp_json["result"] = json::parse(resp.result_json);
        } else {
            resp_json["result"] = nullptr;
        }
    } else if (resp.is_mcp_error) {
        if (!resp.error_json.empty()) {
            resp_json["result"] = json::parse(resp.error_json);
        } else {
            resp_json["result"] = {
                {"content", json::array({{{"type", "text"}, {"text", "Internal error"}}})},
                {"isError", true}
            };
        }
    } else if (resp.success) {
        json content_item;
        content_item["type"] = "text";
        if (!resp.result_json.empty()) {
            content_item["text"] = resp.result_json;
        } else {
            content_item["text"] = "null";
        }
        resp_json["result"] = {
            {"content", json::array({content_item})}
        };
    } else {
        std::string error_text;
        if (!resp.error_json.empty()) {
            error_text = resp.error_json;
        } else {
            error_text = R"({"code":-32603,"message":"Internal error"})";
        }
        resp_json["result"] = {
            {"content", json::array({{{"type", "text"}, {"text", error_text}}})},
            {"isError", true}
        };
    }

    return QByteArray::fromStdString(resp_json.dump());
}

void McpTransport::send_http_response(QTcpSocket* socket, int status,
                                       const QByteArray& body,
                                       const char* content_type)
{
    const char* status_text = "OK";
    switch (status) {
        case 200: status_text = "OK"; break;
        case 204: status_text = "No Content"; break;
        case 400: status_text = "Bad Request"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        default:  status_text = "Unknown"; break;
    }

    QByteArray response;
    response.append("HTTP/1.1 " + QByteArray::number(status) + " " + status_text + "\r\n");
    response.append("Content-Type: " + QByteArray(content_type) + "\r\n");
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n");
    response.append("Access-Control-Allow-Headers: Content-Type\r\n");
    response.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");
    response.append(body);

    socket->write(response);
    socket->flush();

    // Wait for bytes to be written before disconnecting
    if (socket->state() == QAbstractSocket::ConnectedState) {
        socket->waitForBytesWritten(3000);
    }
    socket->disconnectFromHost();
}

void McpTransport::send_http_204(QTcpSocket* socket)
{
    QByteArray response;
    response.append("HTTP/1.1 204 No Content\r\n");
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n");
    response.append("Access-Control-Allow-Headers: Content-Type\r\n");
    response.append("Content-Length: 0\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");

    socket->write(response);
    socket->flush();

    if (socket->state() == QAbstractSocket::ConnectedState) {
        socket->waitForBytesWritten(3000);
    }
    socket->disconnectFromHost();
}

} // namespace pv::api
