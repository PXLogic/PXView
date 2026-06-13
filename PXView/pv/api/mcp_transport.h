#pragma once

#include "transport.h"

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSet>
#include <map>

namespace pv::api {

class McpTransport : public QObject, public ITransport {
    Q_OBJECT
public:
    McpTransport(IJsonRpcHandler* handler, int port = 10110);
    ~McpTransport();

    bool start() override;
    void stop() override;
    bool is_running() const override;

    // SSE support
    void send_sse_headers(QTcpSocket* socket);
    void send_sse_event(QTcpSocket* socket, const std::string& event, const std::string& data);
    void send_sse_done(QTcpSocket* socket, const std::string& event, const std::string& final_result_json);

private slots:
    void on_new_connection();
    void on_ready_read();

private:
    IJsonRpcHandler* _handler;
    int _port;
    QTcpServer* _server = nullptr;
    QSet<QTcpSocket*> _pending_sockets;

    void try_handle_request(QTcpSocket* socket);
    void handle_http_request(QTcpSocket* socket, const QByteArray& data);
    void send_http_response(QTcpSocket* socket, int status, const QByteArray& body,
                            const char* content_type = "application/json");
    void send_http_204(QTcpSocket* socket);
    void handle_sse_wait_capture(QTcpSocket* socket, const JsonRpcRequest& req);
};

} // namespace pv::api
