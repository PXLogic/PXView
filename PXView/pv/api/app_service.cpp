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

#include "app_service.h"
#include "session_service.h"
#include "../appcontrol.h"
#include "../sigsession.h"
#include "../deviceagent.h"
#include "../config/appconfig.h"

#include <libsigrok.h>
#include <algorithm>

namespace pv {
namespace api {

AppService::AppService(AppControl* app_control)
    : _app_control(app_control)
    , _active_session_id(-1)
    , _next_session_id(0)
{
}

AppService::~AppService()
{
    shutdown();
}

// ---- 1. Lifecycle ----

Result<void> AppService::initialize()
{
    // AppControl is already initialized by the time AppService is created.
    // Auto-register the existing SigSession so that MCP tools can use it
    // immediately without requiring an explicit create_session() call.
    // The GUI already has a SigSession with a device connected, so we just
    // need to wrap it in a SessionService and register it.
    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (session) {
        int session_id = _next_session_id++;
        auto* svc = new SessionService(session, session->get_device());
        _sessions[session_id] = svc;
        _active_session_id = session_id;
    }
    return Result<void>::Success();
}

Result<void> AppService::shutdown()
{
    // Destroy all sessions
    for (auto& pair : _sessions) {
        delete pair.second;
    }
    _sessions.clear();
    _active_session_id = -1;
    return Result<void>::Success();
}

// ---- 2. Device management ----

DeviceInfo AppService::translate_device_info(void* base_info_v,
                                                     int index) const
{
    (void)index;
    auto* base_info = static_cast<::ds_device_base_info*>(base_info_v);
    DeviceInfo info;
    if (!base_info)
        return info;

    // Use the handle value as a string id
    info.id = std::to_string(static_cast<uint64_t>(base_info->handle));
    info.display_name = base_info->name;

    // Get richer info from the DeviceAgent if available
    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (session) {
        ::DeviceAgent* dev = session->get_device();
        if (dev && dev->have_instance()) {
            info.driver_name = dev->driver_name().toStdString();
            info.path = dev->path().toStdString();
            info.is_hardware = dev->is_hardware();
            info.is_demo = dev->is_demo();
            info.is_file = dev->is_file();
            info.is_virtual = dev->is_virtual();
            info.is_hardware_logic = dev->is_hardware_logic();
            info.is_hardware_dso = dev->is_hardware_dso();
            info.is_dsl_device = dev->is_dsl_device();
            info.is_compat_device = dev->is_compat_device();
        }
    }

    return info;
}

std::vector<DeviceInfo> AppService::get_device_list() const
{
    std::vector<DeviceInfo> result;

    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (!session)
        return result;

    int count = 0;
    int actived_index = -1;
    ::ds_device_base_info* array = session->get_device_list(count, actived_index);

    if (!array || count <= 0)
        return result;

    for (int i = 0; i < count; i++) {
        DeviceInfo info;
        ::ds_device_base_info& item = array[i];

        // Use handle as string id
        info.id = std::to_string(static_cast<uint64_t>(item.handle));
        info.display_name = item.name;

        // Determine device type flags from the name convention.
        std::string name(item.name);
        if (name.find("Demo") != std::string::npos) {
            info.is_demo = true;
            info.is_virtual = true;
        } else {
            info.is_hardware = true;
        }

        result.push_back(info);
    }

    free(array);
    return result;
}

Result<DeviceInfo> AppService::get_active_device() const
{
    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (!session)
        return Result<DeviceInfo>::Fail(ErrorCode::MissingDevice,
                                        "No session available");

    ::DeviceAgent* dev = session->get_device();
    if (!dev || !dev->have_instance())
        return Result<DeviceInfo>::Fail(ErrorCode::MissingDevice,
                                        "No active device");

    DeviceInfo info;
    info.id = std::to_string(static_cast<uint64_t>(dev->handle()));
    info.display_name = dev->name().toStdString();
    info.driver_name = dev->driver_name().toStdString();
    info.path = dev->path().toStdString();
    info.is_hardware = dev->is_hardware();
    info.is_demo = dev->is_demo();
    info.is_file = dev->is_file();
    info.is_virtual = dev->is_virtual();
    info.is_hardware_logic = dev->is_hardware_logic();
    info.is_hardware_dso = dev->is_hardware_dso();
    info.is_dsl_device = dev->is_dsl_device();
    info.is_compat_device = dev->is_compat_device();

    return Result<DeviceInfo>::Success(info);
}

Result<void> AppService::connect_device(const std::string& device_id)
{
    // device_id is the string representation of the ds_device_handle
    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (!session)
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No session available");

    if (device_id.empty())
        return Result<void>::Fail(ErrorCode::InvalidRequest,
                                  "Device id is empty");

    ds_device_handle handle = reinterpret_cast<ds_device_handle>(
        std::stoull(device_id));

    if (!session->set_device(handle))
        return Result<void>::Fail(ErrorCode::DeviceError,
                                  "Failed to connect device");

    notify_event(ServiceEvent::DeviceConfigChanged);
    return Result<void>::Success();
}

Result<void> AppService::disconnect_device(const std::string& device_id)
{
    (void)device_id;
    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (!session)
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No session available");

    ::DeviceAgent* dev = session->get_device();
    if (!dev || !dev->have_instance())
        return Result<void>::Fail(ErrorCode::DeviceDisconnected,
                                  "No device connected");

    dev->release();
    notify_event(ServiceEvent::DeviceDetached);
    return Result<void>::Success();
}

bool AppService::is_scanning_devices() const
{
    // The current architecture does not expose an async scanning state.
    return false;
}

// ---- 3. Session management ----

Result<int> AppService::create_session(
    const std::string& device_id,
    const std::string& file_path)
{
    SigSession* session = _app_control ? _app_control->GetSession() : nullptr;
    if (!session)
        return Result<int>::Fail(ErrorCode::InternalError,
                                 "No SigSession available");

    // Simplified: map the current single SigSession as session_id.
    // If a default session already exists, return it.
    if (!_sessions.empty()) {
        // Return existing session id (the first one)
        int existing_id = _sessions.begin()->first;
        return Result<int>::Success(existing_id);
    }

    // If a file path is provided, open the file
    if (!file_path.empty()) {
        if (!session->set_file(QString::fromStdString(file_path)))
            return Result<int>::Fail(ErrorCode::LoadFailed,
                                     "Failed to open file");
    }

    // If a device_id is specified, connect to that device.
    // However, if the device is already the active device in SigSession,
    // skip set_device() to avoid triggering DSV_MSG_CURRENT_DEVICE_CHANGED
    // which causes massive UI rebuilds that can crash/hang when invoked
    // from the MCP context.
    if (!device_id.empty() && file_path.empty()) {
        bool device_already_active = false;
        if (session->get_device() && session->get_device()->have_instance()) {
            ds_device_handle current_handle = session->get_device()->handle();
            ds_device_handle requested_handle = reinterpret_cast<ds_device_handle>(
                std::stoull(device_id));
            device_already_active = (current_handle == requested_handle);
        }

        if (!device_already_active) {
            ds_device_handle handle = reinterpret_cast<ds_device_handle>(
                std::stoull(device_id));
            if (!session->set_device(handle))
                return Result<int>::Fail(ErrorCode::DeviceError,
                                         "Failed to connect device");
        }
    }

    int session_id = _next_session_id++;
    auto* svc = new SessionService(session, session->get_device());
    _sessions[session_id] = svc;
    _active_session_id = session_id;

    // Note: Do NOT call _on_new_tab_requested() here.
    // set_device() above already triggers DSV_MSG_CURRENT_DEVICE_CHANGED
    // which rebuilds signals for the current tab. Creating a new tab
    // from the MCP context can cause crashes due to UI callback conflicts.

    return Result<int>::Success(session_id);
}

Result<void> AppService::destroy_session(int session_id)
{
    auto it = _sessions.find(session_id);
    if (it == _sessions.end())
        return Result<void>::Fail(ErrorCode::InvalidRequest,
                                  "Session not found");

    delete it->second;
    _sessions.erase(it);

    if (_active_session_id == session_id) {
        _active_session_id = _sessions.empty() ? -1 : _sessions.begin()->first;
    }

    return Result<void>::Success();
}

ISessionService* AppService::get_session(int session_id)
{
    auto it = _sessions.find(session_id);
    if (it == _sessions.end())
        return nullptr;
    return it->second;
}

std::vector<int> AppService::get_session_ids() const
{
    std::vector<int> ids;
    ids.reserve(_sessions.size());
    for (const auto& pair : _sessions) {
        ids.push_back(pair.first);
    }
    return ids;
}

int AppService::get_active_session_id() const
{
    return _active_session_id;
}

Result<void> AppService::set_active_session(int session_id)
{
    auto it = _sessions.find(session_id);
    if (it == _sessions.end())
        return Result<void>::Fail(ErrorCode::InvalidRequest,
                                  "Session not found");

    _active_session_id = session_id;
    return Result<void>::Success();
}

ISessionService* AppService::get_active_session()
{
    if (_active_session_id < 0)
        return nullptr;
    return get_session(_active_session_id);
}

int AppService::get_session_count() const
{
    return static_cast<int>(_sessions.size());
}

// ---- 4. Global event subscription ----

void AppService::add_event_listener(IServiceEventListener* listener)
{
    if (listener) {
        auto it = std::find(_event_listeners.begin(),
                            _event_listeners.end(), listener);
        if (it == _event_listeners.end())
            _event_listeners.push_back(listener);
    }
}

void AppService::remove_event_listener(IServiceEventListener* listener)
{
    auto it = std::find(_event_listeners.begin(),
                        _event_listeners.end(), listener);
    if (it != _event_listeners.end())
        _event_listeners.erase(it);
}

// ---- 5. Global config ----

std::string AppService::get_app_data_dir() const
{
    return GetAppDataDir().toStdString();
}

std::string AppService::get_decoder_search_path() const
{
    return GetDecodeScriptDir().toStdString();
}

// ---- 6. GUI integration callback ----

void AppService::set_new_tab_callback(std::function<void()> callback)
{
    _on_new_tab_requested = std::move(callback);
}

// ---- Internal helpers ----

void AppService::notify_event(
    ServiceEvent event,
    const std::map<std::string, std::string>& params)
{
    ServiceEventData data;
    data.event = event;
    data.params = params;

    for (auto* listener : _event_listeners) {
        listener->on_service_event(data);
    }
}

} // namespace api
} // namespace pv
