/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#pragma once

#include <string>
#include <vector>
#include <QRect>

struct sr_context;
class QWidget;

namespace pv{
    class SigSession;
}

namespace pv::api {
    class IAppService;
    class AppService;
    class RpcDispatcher;
    class WsTransport;
    class McpTransport;
    class DirectTransport;
}

class AppControl
{
private:
    explicit AppControl();
    ~AppControl();
    AppControl(AppControl &o);

public:
    static AppControl* Instance();

    void Destroy();

    bool Init();

    bool Start();

    void Stop();

    void UnInit();

    inline pv::SigSession*  GetSession(){
        return _session;
    }

    pv::api::IAppService* GetAppService();

    inline pv::api::McpTransport* get_mcp_transport() {
        return _mcp_transport;
    }

    inline pv::api::WsTransport* get_ws_transport() {
        return _ws_transport;
    }

    /// Set custom MCP and WebSocket port numbers.
    /// Must be called before Start().
    inline void set_api_ports(int mcp_port, int ws_port) {
        _mcp_port = mcp_port;
        _ws_port = ws_port;
    }

    inline int get_mcp_port() const { return _mcp_port; }
    inline int get_ws_port() const { return _ws_port; }

    inline void SetTopWindow(QWidget *w){
        _topWindow = w;
    }

    inline QWidget* GetTopWindow(){
        return _topWindow;
    }

    bool TopWindowIsMaximized();

public:
    std::string        _open_file_name;
    QRect              _screenRect;

private:
    pv::SigSession      *_session;
    QWidget             *_topWindow;

    // API Service Layer
    pv::api::AppService* _app_service = nullptr;
    pv::api::RpcDispatcher* _rpc_dispatcher = nullptr;
    pv::api::WsTransport* _ws_transport = nullptr;
    pv::api::McpTransport* _mcp_transport = nullptr;
    pv::api::DirectTransport* _direct_transport = nullptr;

    // API port numbers (defaults: MCP=10110, WS=10430)
    int _mcp_port = 10110;
    int _ws_port = 10430;
};
