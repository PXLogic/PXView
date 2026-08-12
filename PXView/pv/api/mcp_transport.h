#pragma once

#include "pv/api/transport.h"
#include "pv/core/qt_async_dispatcher.h"
#include "pv/core/thread_pool.h"

#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSet>
#include <map>
#include <memory>
#include <mutex>

namespace pv::api {

class McpTransport : public QObject, public ITransport, public IServiceEventListener {
    Q_OBJECT
public:
    McpTransport(IJsonRpcHandler* handler, int port = 10110);
    ~McpTransport();

    bool start() override;
    void stop() override;
    bool is_running() const override;

    int get_port() const { return _port; }

    // IServiceEventListener - push service events to MCP clients (via the
    // active SSE streams opened by wait_capture, or as JSON-RPC notifications).
    void on_service_event(const ServiceEventData& data) override;

    // SSE support
    void send_sse_headers(QTcpSocket* socket);
    void send_sse_event(QTcpSocket* socket, const std::string& event, const std::string& data);
    void send_sse_done(QTcpSocket* socket, const std::string& event, const std::string& final_result_json);

private slots:
    void on_new_connection();
    void on_ready_read();

protected:
    // Handle AsyncEvent posted via post_to_self(). Runs on the IO thread.
    void customEvent(QEvent* event) override;

    // Post a functor to this transport's thread (IO thread). Safe to call
    // from any thread — uses QCoreApplication::postEvent which only
    // accesses the receiver's QThreadData.
    void post_to_self(std::function<void()> fn) {
        pv::core::QtAsyncDispatcher::post_to(this, std::move(fn));
    }

private:
    IJsonRpcHandler* _handler;
    int _port;
    QTcpServer* _server = nullptr;
    QSet<QTcpSocket*> _pending_sockets;

    // Sockets currently holding an open SSE stream (e.g. wait_capture in
    // progress). Service events are pushed to these so MCP clients receive
    // notifications without polling.
    QSet<QTcpSocket*> _sse_clients;
    mutable std::mutex _sse_clients_mutex;

    void try_handle_request(QTcpSocket* socket);
    void handle_http_request(QTcpSocket* socket, const QByteArray& data);
    void send_http_response(QTcpSocket* socket, int status, const QByteArray& body,
                            const char* content_type = "application/json");
    void send_http_204(QTcpSocket* socket);
    void handle_sse_wait_capture(QPointer<QTcpSocket> socket_guard, const JsonRpcRequest& req);

    // Build the MCP JSON-RPC response body from a JsonRpcResponse.
    // Pure data manipulation — safe to call from any thread.
    static QByteArray build_mcp_response_body(const JsonRpcResponse& resp,
                                               const JsonRpcRequest& req);

    // Worker thread pool for offloading business logic (RpcDispatcher dispatch)
    // from the main thread.  Created in start(), destroyed in stop().
    std::unique_ptr<pv::core::ThreadPool> _worker_pool;
};

} // namespace pv::api
