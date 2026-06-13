#pragma once
#include "types.h"
#include "isession_service.h"

namespace pv::api {

class IAppService {
public:
    virtual ~IAppService() = default;

    // 1. Lifecycle
    virtual Result<void> initialize() = 0;
    virtual Result<void> shutdown() = 0;

    // 2. Device management
    virtual std::vector<DeviceInfo> get_device_list() const = 0;
    virtual Result<DeviceInfo> get_active_device() const = 0;
    virtual Result<void> connect_device(const std::string& device_id) = 0;
    virtual Result<void> disconnect_device(const std::string& device_id) = 0;
    virtual bool is_scanning_devices() const = 0;

    // 3. Session management
    virtual Result<int> create_session(
        const std::string& device_id = "",
        const std::string& file_path = "") = 0;
    virtual Result<void> destroy_session(int session_id) = 0;
    virtual ISessionService* get_session(int session_id) = 0;
    virtual std::vector<int> get_session_ids() const = 0;
    virtual int get_active_session_id() const = 0;
    virtual Result<void> set_active_session(int session_id) = 0;
    virtual ISessionService* get_active_session() = 0;
    virtual int get_session_count() const = 0;

    // 4. Global event subscription
    virtual void add_event_listener(IServiceEventListener* listener) = 0;
    virtual void remove_event_listener(IServiceEventListener* listener) = 0;

    // 5. Global config
    virtual std::string get_app_data_dir() const = 0;
    virtual std::string get_decoder_search_path() const = 0;
};

} // namespace pv::api
