/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/session/devicemanager.h"

#include <algorithm>

#include <libsigrok/libsigrok.h>  // sr_dev_inst_free

#include "pv/base/log.h"

namespace pv {

ds_device_handle DeviceManager::register_file_device(struct sr_dev_inst *sdi)
{
    if (!sdi)
        return NULL_HANDLE;
    _file_sdi.push_back(sdi);
    const int scanned_count = (int)_scanned_sdi.size();
    const ds_device_handle handle =
        (ds_device_handle)(scanned_count + (++_next_file_handle));
    _file_handles[handle] = sdi;
    pxv_info("DeviceManager: registered file device sdi=%p handle=%llu",
             (void *)sdi, (unsigned long long)handle);
    return handle;
}

bool DeviceManager::unregister_file_device(ds_device_handle handle,
                                           bool free_sdi)
{
    // handle <= scanned_count 属扫描设备区间，不归注册表管。
    const int scanned_count = (int)_scanned_sdi.size();
    if (handle == NULL_HANDLE || handle <= (ds_device_handle)scanned_count)
        return false;
    auto it = _file_handles.find(handle);
    if (it == _file_handles.end())
        return false;
    struct sr_dev_inst *sdi = it->second;
    _file_handles.erase(it);
    auto vit = std::find(_file_sdi.begin(), _file_sdi.end(), sdi);
    if (vit != _file_sdi.end())
        _file_sdi.erase(vit);
    if (free_sdi && sdi) {
        sr_dev_inst_free(sdi);
        pxv_info("DeviceManager: freed sdi %p for file device handle %llu",
                 (void *)sdi, (unsigned long long)handle);
    }
    return true;
}

bool DeviceManager::detach_sdi(struct sr_dev_inst *sdi, bool free_sdi)
{
    if (!sdi)
        return false;
    for (auto it = _file_handles.begin(); it != _file_handles.end(); ++it) {
        if (it->second == sdi) {
            const ds_device_handle handle = it->first;
            _file_handles.erase(it);
            auto vit = std::find(_file_sdi.begin(), _file_sdi.end(), sdi);
            if (vit != _file_sdi.end())
                _file_sdi.erase(vit);
            if (free_sdi) {
                sr_dev_inst_free(sdi);
                pxv_info("DeviceManager: detached+freed sdi %p (handle %llu)",
                         (void *)sdi, (unsigned long long)handle);
            }
            return true;
        }
    }
    return false;
}

struct sr_dev_inst *DeviceManager::find_sdi_by_handle(
    ds_device_handle handle) const
{
    if (handle == NULL_HANDLE)
        return nullptr;
    const int idx = (int)handle - 1;
    const int scanned_count = (int)_scanned_sdi.size();
    if (idx < scanned_count)
        return _scanned_sdi[idx];
    auto fit = _file_handles.find(handle);
    if (fit != _file_handles.end())
        return fit->second;
    // Legacy position-based fallback（注册表出现前的旧 handle）。
    const int file_idx = idx - scanned_count;
    if (file_idx >= 0 && file_idx < (int)_file_sdi.size())
        return _file_sdi[file_idx];
    return nullptr;
}

ds_device_handle DeviceManager::handle_of_sdi(struct sr_dev_inst *sdi) const
{
    if (!sdi)
        return NULL_HANDLE;
    const int scanned_count = (int)_scanned_sdi.size();
    for (int i = 0; i < scanned_count; i++) {
        if (_scanned_sdi[i] == sdi)
            return (ds_device_handle)(i + 1);
    }
    for (const auto &kv : _file_handles) {
        if (kv.second == sdi)
            return kv.first;
    }
    return NULL_HANDLE;
}

} // namespace pv
