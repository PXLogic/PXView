#include "mcp_transport.h"

#include <nlohmann/json.hpp>

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QTimer>

using json = nlohmann::json;

namespace pv::api {

McpTransport::McpTransport(IJsonRpcHandler* handler, int port)
    : QObject(nullptr), _handler(handler), _port(port)
{
}

McpTransport::~McpTransport()
{
    stop();
}

bool McpTransport::start()
{
    if (_server && _server->isListening())
        return true;

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
    if (_server) {
        _server->close();
        delete _server;
        _server = nullptr;
    }
}

bool McpTransport::is_running() const
{
    return _server && _server->isListening();
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
        }, Qt::UniqueConnection);
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
        }, Qt::UniqueConnection);
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

        // Check if this is a wait_capture tool call — use SSE streaming
        if (req.mcp_tool_name == "wait_capture") {
            handle_sse_wait_capture(socket, req);
            return;
        }
    } else {
        // For other methods (initialize, tools/list, ping), pass params as-is
        if (j.contains("params"))
            req.params_json = j["params"].dump();
    }

    // Dispatch to handler (standard JSON response)
    JsonRpcResponse resp = _handler->handle_request(req);

    // Build MCP response JSON
    json resp_json;
    resp_json["jsonrpc"] = "2.0";
    resp_json["id"] = req.id;

    if (resp.is_mcp_direct) {
        // For initialize and tools/list: result is returned directly (not wrapped in content)
        if (!resp.result_json.empty()) {
            resp_json["result"] = json::parse(resp.result_json);
        } else {
            resp_json["result"] = nullptr;
        }
    } else if (resp.is_mcp_error) {
        // MCP error: {"content":[{"type":"text","text":"[ErrorCode] message"}],"isError":true}
        if (!resp.error_json.empty()) {
            resp_json["result"] = json::parse(resp.error_json);
        } else {
            resp_json["result"] = {
                {"content", json::array({{{"type", "text"}, {"text", "Internal error"}}})},
                {"isError", true}
            };
        }
    } else if (resp.success) {
        // MCP tool call success: wrap result in content array
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
        // Legacy error path — wrap as MCP error
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

    QByteArray resp_body = QByteArray::fromStdString(resp_json.dump());
    send_http_response(socket, 200, resp_body);
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

void McpTransport::handle_sse_wait_capture(QTcpSocket* socket,
                                            const JsonRpcRequest& req)
{
    // Send SSE headers immediately
    send_sse_headers(socket);

    // Parse timeout from arguments
    double timeout_seconds = 300.0;
    try {
        json args = json::parse(req.mcp_tool_args);
        if (args.contains("timeoutSeconds") && args["timeoutSeconds"].is_number())
            timeout_seconds = args["timeoutSeconds"].get<double>();
        else if (args.contains("timeout_seconds") && args["timeout_seconds"].is_number())
            timeout_seconds = args["timeout_seconds"].get<double>();
    } catch (...) {}
    (void)timeout_seconds;

    // Dispatch the wait_capture call to the handler.
    // The handler's wait_capture_complete uses its own QEventLoop internally,
    // which processes Qt events. We add a progress timer that fires during
    // that event loop to send SSE progress events.
    int elapsed_ms = 0;
    const int progress_interval_ms = 500;

    QTimer progress_timer;
    progress_timer.setSingleShot(false);

    QObject::connect(&progress_timer, &QTimer::timeout, [&]() {
        json progress_data;
        progress_data["status"] = "capturing";
        progress_data["elapsed_seconds"] = elapsed_ms / 1000.0;
        send_sse_event(socket, "progress", progress_data.dump());
        elapsed_ms += progress_interval_ms;
    });

    progress_timer.start(progress_interval_ms);

    // This call blocks internally using QEventLoop, but the progress timer
    // fires during that event loop since Qt processes timer events.
    JsonRpcResponse resp = _handler->handle_request(req);

    progress_timer.stop();

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

    // Send the final result as an SSE event and close
    send_sse_done(socket, "result", resp_json.dump());
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
