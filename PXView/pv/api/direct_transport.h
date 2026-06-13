#pragma once
#include "transport.h"
#include "isession_service.h"

namespace pv::api {

class DirectTransport : public ITransport {
public:
    explicit DirectTransport(ISessionService* session) : _session(session) {}
    ~DirectTransport() = default;

    bool start() override { return _session != nullptr; }
    void stop() override {}
    bool is_running() const override { return _session != nullptr; }

    // Direct access to session service (zero overhead)
    ISessionService* session() { return _session; }

    // Optional: JSON-RPC protocol call (for testing/debugging)
    JsonRpcResponse handle_request(const JsonRpcRequest& req)
    {
        (void)req;
        JsonRpcResponse resp;
        resp.id = req.id;
        resp.success = false;
        resp.error_json = R"({"code":-32601,"message":"DirectTransport does not support JSON-RPC, use session() directly"})";
        return resp;
    }

private:
    ISessionService* _session;
};

} // namespace pv::api
