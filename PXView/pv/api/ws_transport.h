#pragma once

#include "transport.h"
#include "types.h"

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>

#include <set>
#include <mutex>

namespace pv::api {

class WsTransport : public QObject, public ITransport, public IServiceEventListener {
    Q_OBJECT

public:
    WsTransport(IJsonRpcHandler* handler, int port = 10430);
    ~WsTransport();

    bool start() override;
    void stop() override;
    bool is_running() const override;

    // IServiceEventListener - broadcast events to all connected clients
    void on_service_event(const ServiceEventData& data) override;

private slots:
    void on_new_connection();
    void on_text_message(const QString& message);
    void on_client_disconnected();

private:
    IJsonRpcHandler* _handler;
    int _port;
    QWebSocketServer* _server = nullptr;
    std::set<QWebSocket*> _clients;
    mutable std::mutex _clients_mutex;
};

} // namespace pv::api
