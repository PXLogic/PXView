/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
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
 */

#pragma once

#include "iapp_service.h"

#include <functional>
#include <map>
#include <vector>

class AppControl;
class DeviceAgent;

namespace pv {

class SigSession;

namespace api {

class SessionService;

class AppService : public IAppService {
public:
    explicit AppService(AppControl* app_control);
    ~AppService() override;

    // 1. Lifecycle
    Result<void> initialize() override;
    Result<void> shutdown() override;

    // 2. Device management
    std::vector<DeviceInfo> get_device_list() const override;
    Result<DeviceInfo> get_active_device() const override;
    Result<void> connect_device(const std::string& device_id) override;
    Result<void> disconnect_device(const std::string& device_id) override;
    bool is_scanning_devices() const override;

    // 3. Session management
    Result<int> create_session(
        const std::string& device_id = "",
        const std::string& file_path = "") override;
    Result<void> destroy_session(int session_id) override;
    ISessionService* get_session(int session_id) override;
    std::vector<int> get_session_ids() const override;
    int get_active_session_id() const override;
    Result<void> set_active_session(int session_id) override;
    ISessionService* get_active_session() override;
    int get_session_count() const override;

    // 4. Global event subscription
    void add_event_listener(IServiceEventListener* listener) override;
    void remove_event_listener(IServiceEventListener* listener) override;

    // 5. Global config
    std::string get_app_data_dir() const override;
    std::string get_decoder_search_path() const override;

    // 6. GUI integration callback
    // Set by MainWindow to allow API layer to request new tabs
    void set_new_tab_callback(std::function<void()> callback);

private:
    DeviceInfo translate_device_info(void* base_info,
                                     int index) const;
    void notify_event(ServiceEvent event,
                      const std::map<std::string, std::string>& params = {});

    AppControl* _app_control;
    std::map<int, SessionService*> _sessions;
    int _active_session_id;
    int _next_session_id;
    std::vector<IServiceEventListener*> _event_listeners;
    std::function<void()> _on_new_tab_requested;
};

} // namespace api
} // namespace pv
