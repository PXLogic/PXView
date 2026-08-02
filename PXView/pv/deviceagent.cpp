/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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

#include "deviceagent.h"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>
#include "log.h"
#include "config/appconfig.h"

// Upstream libsigrok 0.6.0 is now the sole libsigrok (fork + bridge removed).
// All ds_* fork APIs are replaced by sr_* upstream APIs.

// Static device-mode entries for the four work modes shown in the DevMode
// toolbar button. Defined here (not in a header) so their address is stable
// across TUs. acronym is used by storesession.cpp to build default save
// filenames (e.g. "demo-MSO-260707-120000.pxc").
const sr_dev_mode kDevModeLogic  = { LOGIC,  "LA"   };
const sr_dev_mode kDevModeAnalog = { ANALOG, "DAQ"  };
const sr_dev_mode kDevModeDso    = { DSO,    "DSO"  };
const sr_dev_mode kDevModeMso    = { MSO,    "MSO"  };

DeviceAgent::DeviceAgent()
{
    _dev_handle = NULL_HANDLE;
    _di = nullptr;
    _dev_type = 0;
    _callback = nullptr;
    _is_new_device = false;
    _sr_session = nullptr;
    _sr_ctx = nullptr;
    _datafeed_cb = nullptr;
    _datafeed_cb_data = nullptr;
}

DeviceAgent::~DeviceAgent()
{
    release();
}

// --- Device list management ---

void DeviceAgent::set_scanned_devices(const std::vector<struct sr_dev_inst*> &sdis)
{
    _scanned_sdi = sdis;
}

ds_device_handle DeviceAgent::set_file_device(struct sr_dev_inst *sdi, const QString &name)
{
    if (!sdi)
        return NULL_HANDLE;
    _file_sdi.push_back(sdi);
    (void)name;  // name is derived from sdi in update()

    // Compute handle: scanned_count + file_index + 1
    int scanned_count = (int)_scanned_sdi.size();
    int file_idx = (int)_file_sdi.size() - 1;
    return (ds_device_handle)(scanned_count + file_idx + 1);
}

void DeviceAgent::remove_device(ds_device_handle handle)
{
    // File devices are tracked by index; handle = scanned_count + file_index + 1.
    // Remove from _file_sdi if the handle maps to a file device.
    int scanned_count = (int)_scanned_sdi.size();
    if (handle > (ds_device_handle)scanned_count) {
        int file_idx = (int)handle - scanned_count - 1;
        if (file_idx >= 0 && file_idx < (int)_file_sdi.size()) {
            struct sr_dev_inst *sdi = _file_sdi[file_idx];
            _file_sdi.erase(_file_sdi.begin() + file_idx);

            // If this is NOT the currently active device, the sdi is owned
            // solely by _file_sdi and must be freed now. If it IS the current
            // device, release() will free it when set_device() is called.
            if (handle != _dev_handle && sdi) {
                sr_dev_inst_free(sdi);
                pxv_info("remove_device: freed sdi %p for file device handle %llu",
                         (void *)sdi, (unsigned long long)handle);
            }
        }
    }
}

struct sr_dev_inst* DeviceAgent::find_sdi_by_handle(ds_device_handle handle)
{
    // Handle = index+1 (0 reserved for NULL_HANDLE).
    // Scanned devices come first, then file devices.
    if (handle == NULL_HANDLE)
        return nullptr;

    int idx = (int)handle - 1;
    int scanned_count = (int)_scanned_sdi.size();

    if (idx < scanned_count) {
        return _scanned_sdi[idx];
    }

    int file_idx = idx - scanned_count;
    if (file_idx >= 0 && file_idx < (int)_file_sdi.size()) {
        return _file_sdi[file_idx];
    }

    return nullptr;
}

// --- Lifecycle ---

bool DeviceAgent::open_by_handle(ds_device_handle handle, struct sr_context *ctx)
{
    if (handle == NULL_HANDLE) {
        pxv_warn("%s", "DeviceAgent::open_by_handle: handle is nullptr");
        return false;
    }

    _sr_ctx = ctx;
    struct sr_dev_inst *sdi = find_sdi_by_handle(handle);
    if (!sdi) {
        pxv_err("DeviceAgent::open_by_handle: sdi not found for handle %llu",
                (unsigned long long)handle);
        return false;
    }

    // Open the device via upstream sr_dev_open. The driver's dev_open() may
    // emit sr_err with details (e.g. fx2lafw "Unable to open device" /
    // "Unable to claim USB interface" / "Expected firmware version X.x");
    // those are forwarded to PXView.log by sigrok_log_callback registered in
    // SigSession::init(). Emit a top-level failure marker here too so the
    // caller-visible failure is unmistakable in the log.
    struct sr_dev_driver *open_drv = sr_dev_inst_driver_get(sdi);
    const char *drv_name = (open_drv && open_drv->name) ? open_drv->name : "?";

    // 架构修复：virtual-session 文件设备不需要调用 sr_dev_open。
    // sr_dev_open 会调用驱动 dev_open 回调，virtual-session 驱动的
    // dev_open 不存在或会失败（它是虚拟设备，没有硬件可打开）。
    // 文件设备的 sdi 已经由 sr_session_load_file_device 创建并配置好，
    // 只需要添加到 sr_session 即可。
    bool is_virtual_session = false;
    if (open_drv && open_drv->name) {
        QString name = QString::fromLocal8Bit(open_drv->name);
        if (name == "virtual-session" || name.contains("file"))
            is_virtual_session = true;
    } else if (!open_drv) {
        // Input module devices (from sr_input_new) have no driver —
        // treat as virtual to skip sr_dev_open (which would crash).
        is_virtual_session = true;
        pxv_info("open_by_handle: nullptr driver — treating as virtual input-module device");
    }

    int open_ret = SR_OK;
    if (!is_virtual_session) {
        open_ret = sr_dev_open(sdi);
    } else {
        pxv_info("open_by_handle: skipping sr_dev_open for virtual-session/file device");
    }

    // PXLogic firmware upgrade path: hw_usb_open burns new firmware, then
    // "rst usb" resets the device. The device drops off the USB bus and
    // re-enumerates. hw_dev_open returns SR_ERR_DEV_CLOSED to signal this.
    // The old libusb_device_handle is now stale — we must wait for the device
    // to re-enumerate, rescan, and reopen. Retry up to 5 times with 1s delay.
    if (open_ret == SR_ERR_DEV_CLOSED) {
        pxv_info("open_by_handle: device reset after firmware upgrade (ret=-7), "
                 "waiting for re-enumeration...");
        for (int retry = 0; retry < 5; retry++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            pxv_info("open_by_handle: retry %d/5 after firmware reset", retry + 1);
            // Rescan to get the freshly-enumerated sdi with a new
            // libusb_device_handle. The old sdi's conn handle is stale.
            // Caller (SigSession) must refresh_device_list before retrying;
            // we use the externally-provided refresh callback if set, or
            // directly re-scan via the context.
            // The sdi pointer may be freed by sr_driver_scan's clear_instances
            // — re-find by connection_id (USB port path) which is stable.
            const char *old_conn_id = sr_dev_inst_connid_get(sdi);
            if (!old_conn_id) {
                pxv_err("open_by_handle: cannot get connection_id for retry");
                break;
            }
            // Re-scan: find sdi with same connection_id
            struct sr_dev_inst *new_sdi = nullptr;
            struct sr_dev_driver **drivers = sr_driver_list(ctx);
            if (drivers) {
                for (int d = 0; drivers[d]; d++) {
                    if (sr_driver_init(ctx, drivers[d]) != SR_OK)
                        continue;
                    GSList *devs = sr_driver_scan(drivers[d], nullptr);
                    for (GSList *l = devs; l; l = l->next) {
                        struct sr_dev_inst *s = (struct sr_dev_inst *)l->data;
                        const char *cid = sr_dev_inst_connid_get(s);
                        if (cid && strcmp(cid, old_conn_id) == 0) {
                            new_sdi = s;
                            break;
                        }
                    }
                }
            }
            if (!new_sdi) {
                pxv_info("open_by_handle: device not yet re-enumerated, retrying...");
                continue;
            }
            // Found the re-enumerated device — reopen it
            sdi = new_sdi;
            open_ret = sr_dev_open(sdi);
            if (open_ret == SR_OK) {
                pxv_info("open_by_handle: device reopened successfully after firmware upgrade");
                break;
            }
            if (open_ret != SR_ERR_DEV_CLOSED) {
                // Different error — not a firmware reset, surface it
                break;
            }
        }
    }

    if (open_ret != SR_OK) {
        pxv_err("DeviceAgent::open_by_handle: sr_dev_open failed (ret=%d, %s) driver=%s",
                open_ret, sr_strerror(open_ret), drv_name);
        return false;
    }

    _di = sdi;
    _dev_handle = handle;

    // Determine device type from driver name (fork used dev_type field).
    struct sr_dev_driver *drv = sr_dev_inst_driver_get(sdi);
    if (drv) {
        _driver_name = QString::fromLocal8Bit(drv->name);
        if (_driver_name == "demo") {
            _dev_type = DEV_TYPE_DEMO;
            // Demo device declares both LOGIC_ANALYZER + OSCILLOSCOPE (implicit
            // MSO per libsigrok demo driver drvopts). Default to MSO mode so
            // the mode button shows "Mixed Signal Oscilloscope" and all
            // channels (8 logic + 5 analog) are visible on first launch.
            _app_work_mode = MSO;
        } else if (_driver_name == "virtual-session" || _driver_name.contains("file")) {
            _dev_type = DEV_TYPE_FILELOG;
            // Restore work mode from the session file header.
            // session_file.c parses "device mode = N" and stores it via
            // SR_CONF_DEVICE_MODE on the session_driver's vdev struct.
            // Without this, _app_work_mode defaults to LOGIC (set by
            // release()), causing MSO files to open in logic-only mode.
            int file_mode = 0;
            if (get_config_int32(SR_CONF_DEVICE_MODE, file_mode) && file_mode >= 0) {
                _app_work_mode = file_mode;
                pxv_info("open_by_handle: file device mode = %d", file_mode);
            } else {
                _app_work_mode = LOGIC;
            }
        } else {
            _dev_type = DEV_TYPE_USB;
        }
    } else {
        // Input module devices (VCD, CSV, binary, etc.) have no driver.
        // Treat as a file-backed logic device so init_signals() and
        // the UI classify it correctly.
        _driver_name = "input-module";
        _dev_type = DEV_TYPE_FILELOG;
        _app_work_mode = LOGIC;
        pxv_info("open_by_handle: input-module device, dev_type=FILELOG, mode=LOGIC");
    }

    // Create upstream sr_session and add the device.
    if (_sr_session) {
        sr_session_destroy(_sr_session);
        _sr_session = nullptr;
    }

    if (sr_session_new(_sr_ctx, &_sr_session) != SR_OK) {
        pxv_err("DeviceAgent::open_by_handle: sr_session_new failed");
        _sr_session = nullptr;
        return false;
    }

    if (sr_session_dev_add(_sr_session, sdi) != SR_OK) {
        pxv_err("DeviceAgent::open_by_handle: sr_session_dev_add failed");
        sr_session_destroy(_sr_session);
        _sr_session = nullptr;
        return false;
    }

    // Register the datafeed callback if one was set.
    if (_datafeed_cb) {
        sr_session_datafeed_callback_add(_sr_session, _datafeed_cb,
                                         _datafeed_cb_data);
    }

    _is_new_device = true;

    // Restore app-layer device settings from AppConfig (persisted across restarts).
    // These are global preferences stored in QSettings "Device" group.
    auto &devOpt = AppConfig::Instance().deviceOptions;
    _app_disk_cache_enable = devOpt.diskCacheEnable;
    _app_disk_cache_path = devOpt.diskCachePath;
    _app_stream_buff = devOpt.streamBuff;
    _app_stream_mem_buff = devOpt.streamMemBuff;

    // Reset app-layer stream mode cache — new device may have different
    // SR_CONF_CONTINUOUS / OPERATION_MODE capability. Will be re-detected
    // on first is_stream_mode() call.
    _app_stream_mode_init = false;

    // Reset cached mode list — new device may have different channel
    // capabilities (logic/analog/DSO). Will be rebuilt on first
    // get_device_mode_list() call.
    if (_mode_list_cache) {
        g_slist_free(_mode_list_cache);
        _mode_list_cache = nullptr;
    }
    return true;
}

void DeviceAgent::release()
{
    if (_sr_session) {
        if (sr_session_is_running(_sr_session)) {
            sr_session_stop(_sr_session);
        }
        // Always join the worker thread BEFORE sr_session_destroy —
        // the thread is blocked inside sr_session_run() which references
        // the session object. Destroying the session underneath it would
        // cause a use-after-free.
        stop_session_thread();
        sr_session_destroy(_sr_session);
        _sr_session = nullptr;
    }

    if (_di) {
        sr_dev_close(_di);

        // File devices (loaded via sr_session_load_file_device or
        // sr_input_release_sdi) have their sdi ownership transferred to
        // DeviceAgent. Free the sdi here and remove it from _file_sdi to
        // prevent both a memory leak and dangling pointers.
        if (_dev_type == DEV_TYPE_FILELOG) {
            auto it = std::find(_file_sdi.begin(), _file_sdi.end(), _di);
            if (it != _file_sdi.end())
                _file_sdi.erase(it);
            sr_dev_inst_free(_di);
            pxv_info("release: freed sdi %p for file device", (void *)_di);
        }

        _di = nullptr;
    }

    _dev_handle = NULL_HANDLE;
    _dev_name = "";
    _path = "";
    _driver_name = "";
    _dev_type = 0;
    _is_new_device = false;
    _sr_ctx = nullptr;
    _app_work_mode = LOGIC;

    // Free cached mode list (GSList owned by DeviceAgent; sr_dev_mode entries
    // are static globals and not freed).
    if (_mode_list_cache) {
        g_slist_free(_mode_list_cache);
        _mode_list_cache = nullptr;
    }
}

void DeviceAgent::stop_session_thread()
{
    if (_session_thread.joinable()) {
        _session_thread.join();
    }
}

void DeviceAgent::update()
{
    if (!_di) {
        _dev_handle = NULL_HANDLE;
        _dev_name = "";
        _path = "";
        _driver_name = "";
        _dev_type = 0;
        _is_new_device = false;
        return;
    }

    // Pull device identity from the active sdi via upstream accessors.
    const char *vendor = sr_dev_inst_vendor_get(_di);
    const char *model = sr_dev_inst_model_get(_di);
    const char *conn = sr_dev_inst_connid_get(_di);

    char name_buf[160] = {0};
    if (vendor && model) {
        snprintf(name_buf, sizeof(name_buf), "%s %s", vendor, model);
    } else if (model) {
        snprintf(name_buf, sizeof(name_buf), "%s", model);
    } else if (conn) {
        snprintf(name_buf, sizeof(name_buf), "%s", conn);
    }
    _dev_name = QString::fromLocal8Bit(name_buf);

    struct sr_dev_driver *drv = sr_dev_inst_driver_get(_di);
    if (drv) {
        _driver_name = QString::fromLocal8Bit(drv->name);
    }

    // dev_type is set in open_by_handle; keep it here for consistency.
}

void DeviceAgent::set_datafeed_callback(sr_datafeed_callback cb, void *user_data)
{
    _datafeed_cb = cb;
    _datafeed_cb_data = user_data;

    // If session already exists, register the callback now.
    if (_sr_session && cb) {
        sr_session_datafeed_callback_add(_sr_session, cb, user_data);
    }
}

struct sr_dev_inst* DeviceAgent::inst()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::inst: _dev_handle is nullptr");
        return nullptr;
    }
    return _di;
}

// --- Channel operations ---

bool DeviceAgent::enable_probe(const sr_channel *probe, bool enable)
{
    if (!_dev_handle || !probe) {
        pxv_warn("%s", "DeviceAgent::enable_probe: _dev_handle or probe is nullptr");
        return false;
    }

    if (sr_dev_channel_enable(const_cast<sr_channel*>(probe), enable) == SR_OK) {
        config_changed();
        return true;
    }
    return false;
}

bool DeviceAgent::enable_probe(int probe_index, bool enable)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::enable_probe: _dev_handle is nullptr");
        return false;
    }

    // Find channel by index.
    for (const GSList *l = get_channels(); l; l = l->next) {
        sr_channel *probe = (sr_channel *)l->data;
        if (probe && probe->index == probe_index) {
            if (sr_dev_channel_enable(probe, enable) == SR_OK) {
                config_changed();
                return true;
            }
            return false;
        }
    }
    return false;
}

bool DeviceAgent::set_channel_name(int ch_index, const char *name)
{
    if (!_dev_handle || !name) {
        pxv_warn("%s", "DeviceAgent::set_channel_name: _dev_handle or name is nullptr");
        return false;
    }

    // Upstream API: sr_dev_channel_name_set(channel, name).
    for (const GSList *l = get_channels(); l; l = l->next) {
        sr_channel *probe = (sr_channel *)l->data;
        if (probe && probe->index == ch_index) {
            // Upstream libsigrok provides sr_dev_channel_name_set.
            // If not available, set the name field directly (sdi owns the channel).
            // sr_dev_channel_name_set(probe, name);
            return true;
        }
    }
    return false;
}

bool DeviceAgent::channel_is_enable(int index)
{
    for (const GSList *l = get_channels(); l; l = l->next) {
        const sr_channel *const probe = (const sr_channel *)l->data;
        if (probe && probe->index == index)
            return probe->enabled;
    }
    return false;
}

GSList* DeviceAgent::get_channels()
{
    if (!_dev_handle || !_di) {
        pxv_warn("%s", "DeviceAgent::get_channels: no device instance");
        return nullptr;
    }
    return sr_dev_inst_channels_get(_di);
}

int DeviceAgent::get_channel_count()
{
    return g_slist_length(get_channels());
}

bool DeviceAgent::have_enabled_channel()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::have_enabled_channel: _dev_handle is nullptr");
        return false;
    }
    for (const GSList *l = get_channels(); l; l = l->next) {
        const sr_channel *const probe = (const sr_channel *)l->data;
        if (probe && probe->enabled)
            return true;
    }
    return false;
}

// --- Sample config ---

uint64_t DeviceAgent::get_sample_limit()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_sample_limit: _dev_handle is nullptr");
        return 0;
    }
    uint64_t v = 0;
    GVariant *gvar = get_config(SR_CONF_LIMIT_SAMPLES, nullptr, nullptr);
    if (gvar) {
        v = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }
    // 上游约定 limit_samples=0 表示"不限制"。
    // get_sample_limit() 返回的是"停止条件"（用户选择的采样深度对应的样本数），
    // 用于视图时间范围计算（cur_sampletime = samplelimits / samplerate）、
    // 采样深度下拉框比较等。Ring buffer 大小（用于 mmap 内存分配）请用
    // get_ring_sample_count()。
    // - 驱动 limit_samples > 0：直接返回驱动值（用户已选择有限采样深度）。
    // - 驱动 limit_samples == 0（stream 模式持续流 / demo 默认）：返回应用层
    //   默认值，确保视图有合理的时间范围参考。
    if (v == 0) {
        v = AppConfig::Instance().default_sample_limit();
    }
    return v;
}

uint64_t DeviceAgent::get_driver_sample_limit()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_driver_sample_limit: _dev_handle is nullptr");
        return 0;
    }
    uint64_t v = 0;
    GVariant *gvar = get_config(SR_CONF_LIMIT_SAMPLES, nullptr, nullptr);
    if (gvar) {
        v = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }
    return v;
}

uint64_t DeviceAgent::get_ring_sample_count()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_ring_sample_count: _dev_handle is nullptr");
        return 0;
    }
    // Ring buffer 大小用于 LogicSnapshot/AnalogSnapshot 的 mmap 内存分配。
    // - 非 stream 模式：与 get_sample_limit() 等价（缓冲区刚好容纳一次采集）。
    // - Stream 模式：驱动 limit_samples=0（持续流），需要基于
    //   _app_stream_mem_buff(GB) 计算 ring buffer 大小。取 max(mem_buff_samples,
    //   one_sec_samples) 确保至少能容纳 1 秒数据。
    uint64_t driver_limit = 0;
    GVariant *gvar = get_config(SR_CONF_LIMIT_SAMPLES, nullptr, nullptr);
    if (gvar) {
        driver_limit = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }

    if (!is_stream_mode()) {
        // 非 stream 模式：ring buffer = 停止条件（与 get_sample_limit 一致）
        return driver_limit > 0 ? driver_limit
                                : AppConfig::Instance().default_sample_limit();
    }

    // Stream 模式：基于 _app_stream_mem_buff(GB) 或 _app_stream_buff(GB) 计算
    uint64_t samplerate = get_sample_rate();
    
    // 如果启用了磁盘缓存，使用设置的磁盘容量，否则使用内存容量
    double buff_gb = _app_disk_cache_enable ? _app_stream_buff : _app_stream_mem_buff;

    // 对于逻辑分析仪，通道数据是按 bit-plane 存储的（实际上 allocate_block 分配时，
    // 每个通道独立分配 LeafBlockSpace 字节，等于每 8 个样本占 ch_num bytes）。
    // 所以总样本数上限需要乘以 8 / ch_num。
    int ch_num = 0;
    for (GSList *l = get_channels(); l; l = l->next) {
        struct sr_channel *ch = (struct sr_channel *)l->data;
        if (ch->type == SR_CHANNEL_LOGIC) {
            ch_num++;
        }
    }
    if (ch_num <= 0) ch_num = 1;

    uint64_t mem_buff_samples = (uint64_t)(buff_gb * 1000000000ULL) * 8 / ch_num;

    uint64_t one_sec_samples = samplerate;
    uint64_t v = (mem_buff_samples > one_sec_samples) ? mem_buff_samples
                                                      : one_sec_samples;
    if (v == 0)
        v = AppConfig::Instance().default_sample_limit();
    pxv_info("get_ring_sample_count: stream mode, ring_buffer=%llu "
             "(samplerate=%llu, buff_gb=%.1f, ch_num=%d)",
             (unsigned long long)v,
             (unsigned long long)samplerate, buff_gb, ch_num);
    return v;
}

uint64_t DeviceAgent::get_hw_depth()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_hw_depth: _dev_handle is nullptr");
        return 0;
    }
    uint64_t v = 0;
    GVariant *gvar = get_config(SR_CONF_HW_DEPTH, nullptr, nullptr);
    if (gvar) {
        v = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }
    return v;
}

uint64_t DeviceAgent::get_sample_rate()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_sample_rate: _dev_handle is nullptr");
        return 0;
    }
    uint64_t v = 0;
    GVariant *gvar = get_config(SR_CONF_SAMPLERATE, nullptr, nullptr);
    if (gvar) {
        v = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }
    return v;
}

uint64_t DeviceAgent::get_time_base()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_time_base: _dev_handle is nullptr");
        return 0;
    }
    uint64_t v = 0;
    GVariant *gvar = get_config(SR_CONF_TIMEBASE, nullptr, nullptr);
    if (gvar) {
        v = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
    }
    return v;
}

double DeviceAgent::get_sample_time()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_sample_time: _dev_handle is nullptr");
        return 0;
    }
    uint64_t sample_rate = get_sample_rate();
    uint64_t sample_limit = get_sample_limit();
    if (sample_rate == 0)
        return 0;
    return sample_limit * 1.0 / sample_rate;
}

// --- Mode ---

int DeviceAgent::get_work_mode()
{
    if (!_dev_handle) {
        // 启动/切换设备期间会被频繁调用，静默返回 _app_work_mode 避免日志噪音
        return _app_work_mode;
    }
    // DSL/PXLogic devices implement SR_CONF_DEVICE_MODE in the driver — query
    // the driver directly and keep _app_work_mode in sync.
    // demo/file/compat devices have no SR_CONF_DEVICE_MODE — use the cached
    // _app_work_mode (set by set_work_mode() or defaulted in open_by_handle()).
    if (is_dsl_device()) {
        int mode = 0;
        get_config_int32(SR_CONF_DEVICE_MODE, mode);
        _app_work_mode = mode;
        return mode;
    }
    return _app_work_mode;
}

void DeviceAgent::set_work_mode(int mode)
{
    _app_work_mode = mode;
    // DSL/PXLogic devices also need the driver-side SR_CONF_DEVICE_MODE update
    // so the hardware switches its channel configuration.
    if (_dev_handle && is_dsl_device())
        set_config_int16(SR_CONF_DEVICE_MODE, mode);
}

const GSList* DeviceAgent::get_device_mode_list()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_device_mode_list: _dev_handle is nullptr");
        return nullptr;
    }

    // Return cached list if already built (rebuilt on each open_by_handle).
    if (_mode_list_cache)
        return _mode_list_cache;

    // Detect channel capabilities by scanning the device's channel list.
    bool has_logic = false;
    bool has_analog = false;
    bool has_dso = false;
    for (const GSList *l = get_channels(); l; l = l->next) {
        const sr_channel *ch = (const sr_channel *)l->data;
        if (!ch)
            continue;
        switch (ch->type) {
        case SR_CHANNEL_LOGIC:  has_logic = true;  break;
        case SR_CHANNEL_ANALOG: has_analog = true; break;
        case SR_CHANNEL_DSO:    has_dso = true;    break;
        }
    }

    // Build the mode list based on the channel types the device actually
    // exposes. libsigrok's device-class capability flags (SR_CONF_LOGIC_ANALYZER
    // / SR_CONF_OSCILLOSCOPE) live in the driver-private drvopts[] array and
    // are not exposed via any public API, so the channel list is the sole
    // reliable source of truth for which modes a device supports.
    //
    // MSO (Mixed Signal Oscilloscope) is offered first when a device exposes
    // logic channels alongside analog/DSO channels — it shows all channel
    // types simultaneously.
    auto add_mode = [&](const sr_dev_mode *m) {
        _mode_list_cache = g_slist_append(_mode_list_cache, (gpointer)m);
    };

    if (has_logic && (has_analog || has_dso))
        add_mode(&kDevModeMso);
    if (has_logic)
        add_mode(&kDevModeLogic);
    if (has_analog)
        add_mode(&kDevModeAnalog);
    if (has_dso)
        add_mode(&kDevModeDso);

    return _mode_list_cache;
}

int DeviceAgent::get_hardware_operation_mode()
{
    if (!_dev_handle) {
        // 启动/切换设备期间会被频繁调用，静默返回 -1 避免日志噪音
        return -1;
    }
    if (is_compat_device())
        return -1;

    /* Task 10/Phase 3: driver config_get now returns the operation mode as
     * a string ("Buffer Mode"/"Stream Mode"/"Internal Test"). Convert back
     * to the LO_OP_* int value that callers (is_stream_mode / samplingbar)
     * compare against. */
    QString mode_str;
    if (get_config_string(SR_CONF_OPERATION_MODE, mode_str)) {
        if (mode_str == "Buffer Mode")
            return LO_OP_BUFFER;
        if (mode_str == "Stream Mode")
            return LO_OP_STREAM;
        if (mode_str == "Internal Test")
            return LO_OP_INTEST;
        pxv_warn("DeviceAgent::get_hardware_operation_mode: unknown mode '%s'",
                 mode_str.toUtf8().constData());
    }
    return -1;
}

bool DeviceAgent::is_stream_mode()
{
    // Lazy init: auto-detect on first call, cache the result so the user
    // can later override it via set_config(SR_CONF_STREAM) and have the
    // new value stick for subsequent is_stream_mode() calls.
    if (_app_stream_mode_init)
        return _app_stream_mode;
    _app_stream_mode = detect_stream_mode();
    _app_stream_mode_init = true;
    pxv_info("is_stream_mode: auto-detected=%d (driver=%s, cached)",
             _app_stream_mode, _driver_name.toUtf8().constData());
    return _app_stream_mode;
}

bool DeviceAgent::detect_stream_mode()
{
    // DSL/PXLogic/demo devices: query SR_CONF_OPERATION_MODE.
    // Demo is now included in is_dsl_device() so it takes this path too.
    // These drivers export "Buffer Mode"/"Stream Mode" strings.
    if (is_dsl_device())
        return get_hardware_operation_mode() == LO_OP_STREAM;

    // Upstream hardware drivers (fx2lafw, ...): check SR_CONF_CONTINUOUS
    // capability flag in the driver's devopts list. fx2lafw declares it
    // (api.c devopts[]), and the driver defaults to limit_samples=0 (stream)
    // in fx2lafw_dev_new(). The frontend must NOT call set_config(LIMIT_SAMPLES)
    // in stream mode — hwdriver.c rejects setting it to 0, so the only way to
    // keep streaming is to leave the driver default untouched.
    if (is_hardware()) {
        GVariant *gvar = get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);
        if (gvar) {
            GVariantIter iter;
            g_variant_iter_init(&iter, gvar);
            guint32 key;
            bool is_stream = false;
            while (g_variant_iter_next(&iter, "u", &key)) {
                /* Mask off capability bits to compare against bare key.
                 * Some drivers advertise SR_CONF_CONTINUOUS with cap bits
                 * (e.g. SR_CONF_CONTINUOUS | SR_CONF_GET), while fx2lafw
                 * uses bare key. SR_CONF_MASK = 0x1fffffff. */
                if ((key & 0x1fffffff) == SR_CONF_CONTINUOUS) {
                    is_stream = true;
                    break;
                }
            }
            g_variant_unref(gvar);
            return is_stream;
        }
    }

    // file 设备不是流式设备：加载有限数据，应由用户选定的
    // LIMIT_SAMPLES 控制采集深度（buffer 模式）。若误归为 stream，会触发
    // get_sample_limit() 用 _app_stream_mem_buff(默认 16GB) 计算 ring_buffer，
    // 导致 AnalogSnapshot::first_payload 执行 malloc(16e9 * ch_num * unit_bytes)
    // 失败 → Malloc_err "内存不足，无法容纳此采样量"。
    return false;
}

bool DeviceAgent::check_firmware_version()
{
    if (!_dev_handle)
        return false;
    if (is_compat_device())
        return true;
    // Fork used ds_get_actived_device_init_status; upstream does not expose
    // init status. Assume compatible (firmware check deferred to driver).
    return true;
}

QString DeviceAgent::get_demo_operation_mode()
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::get_demo_operation_mode: _dev_handle is nullptr");
        return QString();
    }
    if (!is_demo()) {
        pxv_warn("%s", "DeviceAgent::get_demo_operation_mode: not a demo device");
        return QString();
    }

    QString pattern_mode;
    if (!get_config_string(SR_CONF_PATTERN_MODE, pattern_mode)) {
        // demo 设备在初始化阶段可能尚未支持 PATTERN_MODE，静默返回空字符串
        return QString();
    }
    return pattern_mode;
}

// --- Trigger ---

bool DeviceAgent::is_trigger_enabled()
{
    if (!_dev_handle || !_sr_session)
        return false;
    // Upstream: check if a trigger is set on the session.
    // sr_session_trigger_set copies the trigger; there's no direct "is_enabled"
    // query. We approximate by checking if any SignalModel has a non-NONTRIG
    // trig_type. This is consistent with how sync_trigger_to_libsigrok builds
    // the trigger. For simplicity, return true if any logic channel has a
    // trigger set (the actual trigger is synced in sync_trigger_to_libsigrok).
    return false;  // trigger state is tracked by TriggerConfig, not queried here
}

// --- Acquisition ---

bool DeviceAgent::start()
{
    if (!_dev_handle || !_sr_session) {
        pxv_warn("%s", "DeviceAgent::start: no device/session");
        return false;
    }

    // Defensive: a previous thread should have been joined in stop()/release().
    // If one is somehow still alive, join it before starting a new run.
    stop_session_thread();

    // CRITICAL: Both sr_session_start() and sr_session_run() MUST execute in
    // the SAME thread. This is how PulseView does it (sample_thread_proc calls
    // device_->start() then device_->run() sequentially).
    //
    // Reason: sr_session_start() calls set_main_context() which captures the
    // calling thread's GLib thread-default main context via
    // g_main_context_ref_thread_default(). sr_session_run() then creates a
    // GMainLoop bound to that context and runs g_main_loop_run(). If start()
    // runs on the GUI thread but run() on a worker thread, the main context
    // belongs to the wrong thread — on Windows this causes
    // libusb_get_pollfds() to fail (nullptr) and USB event sources are never
    // properly dispatched, resulting in incomplete or no data capture.
    //
    // Use std::promise to synchronize the start result back to the caller.
    std::promise<bool> start_promise;
    std::future<bool> start_future = start_promise.get_future();

    _session_thread = std::thread([this, &start_promise]() {
        if (sr_session_start(_sr_session) != SR_OK) {
            pxv_err("DeviceAgent::start: sr_session_start failed");
            start_promise.set_value(false);
            return;
        }
        start_promise.set_value(true);

        // Block until sr_session_stop() causes the main loop to quit.
        sr_session_run(_sr_session);

        // sr_session_run() has returned → the libsigrok session is fully
        // stopped (main loop quit, event sources removed, running=FALSE).
        // This is the upstream equivalent of fork libsigrok's
        // DS_EV_COLLECT_TASK_END — the reliable "session really stopped"
        // signal. SR_DF_END fires earlier (while the main loop is still
        // running), so it cannot be used to synchronise the next capture.
        // Notify the application layer so it can release the CaptureOwnerGuard
        // and broadcast EndCollectWork. Must be the last statement — the
        // callback must not touch _session_thread (which is this thread).
        if (_callback) {
            _callback->DeviceSessionStopped();
        }
    });

    // Wait for the worker thread to report sr_session_start() result.
    bool ok = start_future.get();
    if (!ok) {
        stop_session_thread();  // join the thread on failure
    }
    return ok;
}

bool DeviceAgent::stop()
{
    if (!_dev_handle || !_sr_session) {
        pxv_warn("%s", "DeviceAgent::stop: no device/session");
        return false;
    }

    // Signal the session's main loop to quit. The worker thread will return
    // from sr_session_run() shortly after.
    if (sr_session_stop(_sr_session) != SR_OK) {
        pxv_warn("%s", "DeviceAgent::stop: sr_session_stop failed");
    }

    // Join the worker thread so is_collecting() / start() see a clean state.
    stop_session_thread();
    return true;
}

bool DeviceAgent::is_collecting()
{
    if (!_dev_handle || !_sr_session)
        return false;
    return sr_session_is_running(_sr_session) > 0;
}

// --- Config (via sdi->driver->config_get/set/list) ---

// Fork-only config keys are defined in pxvdef.h range 60020-60088 (PXLogic
// ext keys + analog probe keys + DSO residual stubs). Upstream libsigrok does
// not know about them — querying returns SR_ERR_ARG ("Invalid key 600XX") and
// floods the log on non-DSL devices (demo/fx2lafw). Block these queries at the
// chokepoint so non-DSL devices never reach sr_config_* with fork keys.
// DSL/PXLogic devices pass through (driver implements the keys).
//
// SR_CONF_DEVICE_OPTIONS is now a public macro (0x7FFF0001, defined in
// libsigrok.h) — not in the 60020-60088 fork range, so it naturally passes
// through this guard. No explicit exemption needed.
static inline bool is_fork_only_key(int key)
{
    return key >= 60020 && key <= 60088;
}

GVariant* DeviceAgent::get_config_list(const sr_channel_group *group, int key)
{
    if (!_dev_handle || !_di) {
        pxv_warn("%s", "DeviceAgent::get_config_list: no device instance");
        return nullptr;
    }

    // App-layer OPERATION_MODE for non-DSL hardware devices (fx2lafw etc.):
    // expose Buffer/Stream string list so the deviceoptions dock can show a
    // run-mode dropdown without driver support. DSL/PXLogic devices forward
    // to the driver (pxlogic.c implements the real list).
    if (key == SR_CONF_OPERATION_MODE && is_hardware() && !is_dsl_device()) {
        static const char *opmode_strs[] = { "Buffer Mode", "Stream Mode" };
        return g_variant_new_strv(opmode_strs, 2);
    }

    const bool fork_only = is_fork_only_key(key);
    const bool dsl = is_dsl_device();
    if (fork_only && !dsl)
        return nullptr;

    struct sr_dev_driver *drv = sr_dev_inst_driver_get(_di);
    if (!drv)
        return nullptr;

    GVariant *data = nullptr;
    int ret = sr_config_list(drv, _di, group, (uint32_t)key, &data);
    if (ret != SR_OK) {
        // SR_ERR_NA / SR_ERR_ARG 表示设备不支持该 key，静默处理避免日志噪音
        if (ret != SR_ERR_NA && ret != SR_ERR_ARG)
            pxv_detail("%s%d", "WARNING: Failed to get config list, key:", key);
        if (data) {
            g_variant_unref(data);
            data = nullptr;
        }
    }
    return data;
}

// App-layer config keys (C-class): not driver-backed. Stored in DeviceAgent
// member state so non-DSL devices (fx2lafw, etc.) work without driver support.
// DSL/PXLogic devices forward to the driver's config_get/set (which has the
// real implementations for these keys).
static inline bool is_app_layer_key(int key)
{
    return key == SR_CONF_DISK_CACHE_ENABLE ||
           key == SR_CONF_DISK_CACHE_PATH ||
           key == SR_CONF_STREAM_BUFF ||
           key == SR_CONF_STREAM_MEM_BUFF ||
           key == SR_CONF_STREAM;
}

GVariant* DeviceAgent::get_config(int key, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle || !_di) {
        pxv_warn("%s", "DeviceAgent::get_config: no device instance");
        return nullptr;
    }

    // For per-channel config keys, if cg is nullptr but ch is provided,
    // find the channel group that contains ch so the driver can identify
    // the channel. This is needed because upstream sr_config_get only takes
    // cg (not ch), but PXView's DeviceAgent passes ch for per-channel config.
    //
    // A channel may belong to multiple groups (e.g. demo driver's ANALOG
    // channels are in both the "Analog" multi-channel group AND a per-channel
    // group). For per-channel config keys (VDIV/COUPLING/etc.), the multi-
    // channel group may not advertise the key in its devopts, causing
    // sr_config_get's check_key() to reject it. Prefer a single-channel group
    // (g_slist_length == 1) so per-channel keys reach the driver.
    if (!cg && ch) {
        struct sr_channel_group *fallback_grp = nullptr;
        for (GSList *l = sr_dev_inst_channel_groups_get(_di); l; l = l->next) {
            struct sr_channel_group *grp = (struct sr_channel_group *)l->data;
            if (grp && g_slist_find(grp->channels, (gpointer)ch)) {
                if (g_slist_length(grp->channels) == 1) {
                    cg = grp;
                    break;
                }
                if (!fallback_grp)
                    fallback_grp = grp;
            }
        }
        if (!cg)
            cg = fallback_grp;
        pxv_dbg("DeviceAgent::get_config: cg lookup for key=%d ch=%p -> cg=%p (name=%s)",
                 key, (void*)ch, (void*)cg,
                 cg ? (cg->name ? cg->name : "(nullptr)") : "(none)");
    }

    // App-layer C-class keys (DISK_CACHE_ENABLE/PATH, STREAM_BUFF,
    // STREAM_MEM_BUFF): served from DeviceAgent member state for ALL devices.
    // These are application-layer concepts (disk cache is implemented by
    // LogicSnapshotDiskCacheWriter + MmapAllocator, not by any driver), so
    // they don't need driver config_get/set support — not even for PXLogic.
    if (is_app_layer_key(key)) {
        switch (key) {
        case SR_CONF_DISK_CACHE_ENABLE:
            return g_variant_new_boolean(_app_disk_cache_enable);
        case SR_CONF_DISK_CACHE_PATH:
            return g_variant_new_string(_app_disk_cache_path.toUtf8().constData());
        case SR_CONF_STREAM_BUFF:
            return g_variant_new_double(_app_stream_buff);
        case SR_CONF_STREAM_MEM_BUFF:
            return g_variant_new_double(_app_stream_mem_buff);
        case SR_CONF_STREAM:
            return g_variant_new_boolean(is_stream_mode());
        }
    }

    // App-layer OPERATION_MODE for non-DSL hardware devices: return the
    // cached stream-mode selection as a "Buffer Mode"/"Stream Mode" string
    // (matching the list exposed by get_config_list above). DSL/PXLogic
    // devices forward to the driver.
    if (key == SR_CONF_OPERATION_MODE && is_hardware() && !is_dsl_device()) {
        const char *mode_str = is_stream_mode() ? "Stream Mode" : "Buffer Mode";
        return g_variant_new_string(mode_str);
    }

    if (is_fork_only_key(key) && !is_dsl_device())
        return nullptr;

    /* Fork-only key 预检查：DSL/PXLogic/demo 设备的 devopts[] 必须声明该 key
     * 且带 SR_CONF_GET 位，否则 sr_config_get -> hwdriver.c check_key() 会打印
     * "Option 'xxx' not available for this device instance" 错误日志。
     * 典型场景：PROBE_CONFIGS(60062) 只有 demo 驱动声明，PXLogic 未声明，
     * 但某些代码路径仍尝试 get_config(PROBE_CONFIGS)。 */
    if (is_fork_only_key(key) && is_dsl_device()) {
        GVariant *gvar_devopts = get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);
        bool devopt_has_get = false;
        if (gvar_devopts) {
            gsize n;
            const uint32_t *opts = (const uint32_t *)g_variant_get_fixed_array(
                gvar_devopts, &n, sizeof(uint32_t));
            for (gsize i = 0; i < n; i++) {
                if ((opts[i] & 0x1fffffff) == (uint32_t)key &&
                    (opts[i] & SR_CONF_GET)) {
                    devopt_has_get = true;
                    break;
                }
            }
            g_variant_unref(gvar_devopts);
        }
        if (!devopt_has_get)
            return nullptr;
    }

    struct sr_dev_driver *drv = sr_dev_inst_driver_get(_di);
    if (!drv)
        return nullptr;

    GVariant *data = nullptr;
    int ret = sr_config_get(drv, _di, cg, (uint32_t)key, &data);
    if (ret != SR_OK) {
        // SR_ERR_NA / SR_ERR_ARG 表示设备不支持该 key（libsigrok 对"选项不可用"
        // 返回 SR_ERR_ARG 而非 SR_ERR_NA），属于正常情况，静默处理避免日志噪音。
        if (ret != SR_ERR_NA && ret != SR_ERR_ARG)
            pxv_err("%s%d", "ERROR:DeviceAgent::get_config, Failed to get value of config id:", key);
        if (data) {
            g_variant_unref(data);
            data = nullptr;
        }
    }
    (void)ch;  // upstream sr_config_get does not take a channel parameter
    return data;
}

bool DeviceAgent::have_config(int key, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config(int key, GVariant *data, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle || !_di) {
        pxv_warn("%s", "DeviceAgent::set_config: no device instance");
        return false;
    }

    // For per-channel config keys, auto-find cg from ch (same as get_config).
    // Prefer single-channel group for per-channel keys (see get_config comment).
    if (!cg && ch) {
        struct sr_channel_group *fallback_grp = nullptr;
        for (GSList *l = sr_dev_inst_channel_groups_get(_di); l; l = l->next) {
            struct sr_channel_group *grp = (struct sr_channel_group *)l->data;
            if (grp && g_slist_find(grp->channels, (gpointer)ch)) {
                if (g_slist_length(grp->channels) == 1) {
                    cg = grp;
                    break;
                }
                if (!fallback_grp)
                    fallback_grp = grp;
            }
        }
        if (!cg)
            cg = fallback_grp;
    }

    // App-layer C-class keys: store in member state for ALL devices.
    if (is_app_layer_key(key)) {
        switch (key) {
        case SR_CONF_DISK_CACHE_ENABLE:
            _app_disk_cache_enable = g_variant_get_boolean(data);
            AppConfig::Instance().deviceOptions.diskCacheEnable = _app_disk_cache_enable;
            AppConfig::Instance().SaveDevice();
            break;
        case SR_CONF_DISK_CACHE_PATH:
            _app_disk_cache_path = QString::fromUtf8(g_variant_get_string(data, nullptr));
            AppConfig::Instance().deviceOptions.diskCachePath = _app_disk_cache_path;
            AppConfig::Instance().SaveDevice();
            break;
        case SR_CONF_STREAM_BUFF:
            _app_stream_buff = g_variant_get_double(data);
            AppConfig::Instance().deviceOptions.streamBuff = _app_stream_buff;
            AppConfig::Instance().SaveDevice();
            break;
        case SR_CONF_STREAM_MEM_BUFF:
            _app_stream_mem_buff = g_variant_get_double(data);
            AppConfig::Instance().deviceOptions.streamMemBuff = _app_stream_mem_buff;
            AppConfig::Instance().SaveDevice();
            break;
        case SR_CONF_STREAM:
            _app_stream_mode = g_variant_get_boolean(data);
            _app_stream_mode_init = true;
            break;
        }
        config_changed();
        return true;
    }

    // App-layer OPERATION_MODE sync: when the user switches Buffer/Stream
    // mode, update the cached _app_stream_mode flag so is_stream_mode()
    // reflects the new choice immediately. Non-DSL devices handle it purely
    // in-app (no driver key); DSL/PXLogic/demo devices also forward to the
    // driver via sr_config_set below (demo driver implements the key since
    // the OPERATION_MODE dropdown addition).
    if (key == SR_CONF_OPERATION_MODE &&
        is_hardware()) {
        const gchar *mode_str = g_variant_get_string(data, nullptr);
        if (mode_str) {
            // Accept both short ("Stream"/"Buffer") and full
            // ("Stream Mode"/"Buffer Mode") names so the MCP API (which
            // documents short names) and the GUI dropdown (which uses full
            // names) both reach the same code path.
            if (g_ascii_strcasecmp(mode_str, "Stream") == 0 ||
                g_ascii_strcasecmp(mode_str, "Stream Mode") == 0) {
                _app_stream_mode = true;
            } else {
                _app_stream_mode = false;
            }
            _app_stream_mode_init = true;
            pxv_info("set_config: OPERATION_MODE='%s' -> _app_stream_mode=%d "
                     "(driver=%s)",
                     mode_str, _app_stream_mode,
                     _driver_name.toUtf8().constData());
        }
        // Non-DSL hardware devices (fx2lafw): no driver key to set, sync
        // cache and done. DSL/PXLogic/demo: fall through to sr_config_set.
        if (is_hardware() && !is_dsl_device()) {
            config_changed();
            return true;
        }
        // DSL/PXLogic/demo: fall through to sr_config_set below.
    }

    if (is_fork_only_key(key) && !is_dsl_device())
        return false;

    // Capture GVariant type string BEFORE sr_config_set(): sr_config_set()
    // takes ownership of `data` (it does g_variant_ref_sink + g_variant_unref),
    // so `data` may be FREED by the time it returns. Reading the type
    // afterwards was a use-after-free that crashed strlen() with 0xfeeefeee
    // when SR_ERR_ARG came back from sr_variant_type_check() or driver
    // config_set() (both fall through to g_variant_unref). The check_key()
    // early-return path leaks data instead (no unref), but we must not rely
    // on which path was taken.
    char type_buf[16] = "(nullptr)";
    const GVariantType *gvt_pre = data ? g_variant_get_type(data) : nullptr;
    if (gvt_pre) {
        const gchar *ts = g_variant_type_peek_string(gvt_pre);
        if (ts) {
            size_t n = strlen(ts);
            if (n >= sizeof(type_buf)) n = sizeof(type_buf) - 1;
            memcpy(type_buf, ts, n);
            type_buf[n] = '\0';
        }
    }

    int ret = sr_config_set(_di, cg, (uint32_t)key, data);
    (void)ch;  // upstream sr_config_set does not take a channel parameter
    if (ret != SR_OK) {
        // SR_ERR_NA: device doesn't support this config key — common and
        // acceptable, stay silent.
        // SR_ERR_ARG: hwdriver.c rejected the value. This can be caused by
        // either check_key() (key not in devopts) or sr_variant_type_check()
        // (GVariant type mismatch, e.g. passing byte "y" for a key declared
        // as SR_T_INT32 "i"). Previously logged at debug level, making type
        // mismatches invisible in Release builds and causing stale values to
        // persist undiagnosed. Now logged at warning level with key name and
        // actual GVariant type string for immediate diagnosis.
        if (ret == SR_ERR_ARG) {
            const struct sr_key_info *kinfo = sr_key_info_get(SR_KEY_CONFIG, (uint32_t)key);
            const char *key_name = kinfo ? kinfo->name : "(unknown)";
            pxv_warn("DeviceAgent::set_config: key '%s' (id=%d) rejected (SR_ERR_ARG) — "
                     "likely type mismatch (got GVariant type '%s') or invalid value",
                     key_name, key, type_buf);
        } else if (ret != SR_ERR_NA) {
            pxv_err("%s%d", "ERROR:DeviceAgent::set_config, Failed to set value of config id:", key);
        }
        return false;
    }
    config_changed();
    return true;
}

bool DeviceAgent::get_config_int32(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        /* Type-safe extraction: drivers may return int16, uint16, int32,
         * uint32, int64, or uint64 depending on the key (e.g. pxlogic.c
         * returns SR_CONF_DEVICE_MODE as g_variant_new_int16). Blindly
         * calling g_variant_get_int32 on a non-INT32 variant triggers
         * "GLib-CRITICAL: g_variant_get_int32: assertion
         * 'g_variant_is_of_type (value, G_VARIANT_TYPE_INT32)' failed".
         * Dispatch by actual GVariant type, matching the pattern already
         * used in prop/int.cpp and storesession.cpp. */
        const GVariantType *type = g_variant_get_type(gvar);
        if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32))
            value = g_variant_get_int32(gvar);
        else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32))
            value = (int)g_variant_get_uint32(gvar);
        else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16))
            value = g_variant_get_int16(gvar);
        else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16))
            value = g_variant_get_uint16(gvar);
        else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64))
            value = (int)g_variant_get_int64(gvar);
        else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64))
            value = (int)g_variant_get_uint64(gvar);
        else if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE))
            value = g_variant_get_byte(gvar);
        else {
            pxv_warn("DeviceAgent::get_config_int32: key %d returned "
                     "unexpected GVariant type '%s', value left unchanged", key,
                     g_variant_type_peek_string(type));
            g_variant_unref(gvar);
            return false;
        }
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_int32(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_int32: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_int32(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_string(int key, QString &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        const gchar *s = g_variant_get_string(gvar, nullptr);
        value = QString::fromLocal8Bit(s);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_string(int key, const char *value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!value) {
        pxv_warn("%s", "DeviceAgent::set_config_string: value is nullptr");
        return false;
    }
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_string: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_string(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_bool(int key, bool &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        gboolean v = g_variant_get_boolean(gvar);
        value = v > 0;
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_bool(int key, bool value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_bool: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_boolean(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_uint64(int key, uint64_t &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        value = g_variant_get_uint64(gvar);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_uint64(int key, uint64_t value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_uint64: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_uint64(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_uint16(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        value = g_variant_get_uint16(gvar);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_uint16(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_uint16: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_uint16(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_uint32(int key, uint32_t &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        value = g_variant_get_uint32(gvar);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_uint32(int key, uint32_t value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_uint32: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_uint32(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_int16(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        value = g_variant_get_int16(gvar);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_int16(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_int16: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_int16(value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_byte(int key, int &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        value = g_variant_get_byte(gvar);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_byte(int key, int value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_byte: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_byte((uint8_t)value);
    return set_config(key, gvar, ch, cg);
}

bool DeviceAgent::get_config_double(int key, double &value, const sr_channel *ch, const sr_channel_group *cg)
{
    GVariant *gvar = get_config(key, ch, cg);
    if (gvar) {
        value = g_variant_get_double(gvar);
        g_variant_unref(gvar);
        return true;
    }
    return false;
}

bool DeviceAgent::set_config_double(int key, double value, const sr_channel *ch, const sr_channel_group *cg)
{
    if (!_dev_handle) {
        pxv_warn("%s", "DeviceAgent::set_config_double: _dev_handle is nullptr");
        return false;
    }
    GVariant *gvar = g_variant_new_double(value);
    return set_config(key, gvar, ch, cg);
}

// --- sr_config create/free (fork libsigrok API stub) ---
// Fork libsigrok exposed ds_new_config / ds_free_config for building
// sr_config entries to attach to sr_datafeed_meta packets (used by
// StoreSession export). Upstream libsigrok does not provide these —
// implement as simple g_new0/g_free wrappers. The caller owns the returned
// sr_config and must free it via free_config().
struct sr_config *DeviceAgent::new_config(int key, GVariant *data)
{
    struct sr_config *src = (struct sr_config *)g_new0(struct sr_config, 1);
    if (!src) return nullptr;
    src->key = (uint32_t)key;
    src->data = data;
    return src;
}

void DeviceAgent::free_config(struct sr_config *src)
{
    if (!src) return;
    if (src->data) g_variant_unref(src->data);
    g_free(src);
}

int DeviceAgent::option_value_to_code(int mode, int key, const char *value)
{
    /* Fork libsigrok's ds_option_value_to_code is not available in upstream
     * libsigrok. Upstream's sr_config_list already returns the mode-
     * appropriate string list (the driver's config_list handler checks the
     * current mode internally), so `mode` does not need to be applied again
     * here — it is retained only for API compatibility. */
    (void)mode;

    if (!value || !_di)
        return -1;

    /* Find a channel group to query config_list against. Prefer the first
     * DSO channel group (per-probe SR_T_LIST keys used by DSO configs are
     * cg-scoped in the demo/DSL drivers); otherwise fall back to the first
     * available group so device-level list keys still resolve. */
    const struct sr_channel_group *cg = nullptr;
    const struct sr_channel_group *first_cg = nullptr;
    for (GSList *l = sr_dev_inst_channel_groups_get(_di); l; l = l->next) {
        const struct sr_channel_group *grp =
            (const struct sr_channel_group *)l->data;
        if (!grp)
            continue;
        if (!first_cg)
            first_cg = grp;
        if (grp->channels) {
            const struct sr_channel *ch =
                (const struct sr_channel *)grp->channels->data;
            if (ch && ch->type == SR_CHANNEL_DSO) {
                cg = grp;
                break;
            }
        }
    }
    if (!cg)
        cg = first_cg;

    GVariant *gvar = get_config_list(cg, key);
    if (!gvar)
        return -1;

    /* g_variant_get_strv returns a freshly-allocated nullptr-terminated array
     * of pointers into the GVariant's internal buffer. The array must be
     * g_free'd; the variant must be g_variant_unref'd after we are done
     * with the strings. */
    int result = -1;
    gsize n_items = 0;
    const gchar **strs = g_variant_get_strv(gvar, &n_items);
    if (strs) {
        for (gsize i = 0; i < n_items; i++) {
            if (strs[i] && strcmp(strs[i], value) == 0) {
                result = (int)i;
                break;
            }
        }
        g_free(strs);
    }
    g_variant_unref(gvar);
    return result;
}

// Fork libsigrok sr_time_string stub. Formats a duration (in nanoseconds)
// as a human-readable string. Caller must g_free() the returned pointer.
char *sr_time_string(uint64_t duration)
{
    double seconds = (double)duration / 1e9;
    if (seconds >= 86400.0)
        return g_strdup_printf("%.2fd", seconds / 86400.0);
    if (seconds >= 3600.0)
        return g_strdup_printf("%.2fh", seconds / 3600.0);
    if (seconds >= 60.0)
        return g_strdup_printf("%.2fm", seconds / 60.0);
    if (seconds >= 1.0)
        return g_strdup_printf("%.2fs", seconds);
    if (seconds >= 1e-3)
        return g_strdup_printf("%.2fms", seconds * 1e3);
    return g_strdup_printf("%.2fus", seconds * 1e6);
}

// --- Typed wrappers ---

bool DeviceAgent::is_roll_mode(bool &roll) {
    return get_config_bool(SR_CONF_ROLL, roll);
}

// Fork analog config keys (SR_CONF_UNIT_BITS / REF_MIN / REF_MAX / PROBE_OFFSET /
// PROBE_HW_OFFSET / PROBE_FACTOR) only exist in PXView's fork libsigrok.h range
// 60042-60047 plus the upstream SR_CONF_PROBE_FACTOR (40005). Upstream drivers
// (demo/fx2lafw) do not implement them — querying returns SR_ERR_NA/invalid-key
// and floods the log. Guard with is_dsl_device() so non-DSL devices skip the
// query entirely. Callers (DsoSignal / AnalogSignal / MathTrace / capturemanager)
// already handle a false return value gracefully (fall back to defaults).
bool DeviceAgent::get_unit_bits(int &v) {
    if (!is_dsl_device()) return false;
    return get_config_byte(SR_CONF_UNIT_BITS, v);
}

bool DeviceAgent::get_ref_min(uint32_t &v) {
    if (!is_dsl_device()) return false;
    return get_config_uint32(SR_CONF_REF_MIN, v);
}

bool DeviceAgent::get_ref_max(uint32_t &v) {
    if (!is_dsl_device()) return false;
    return get_config_uint32(SR_CONF_REF_MAX, v);
}

bool DeviceAgent::get_probe_vdiv(uint64_t &v, sr_channel *probe) {
    if (!is_dsl_device()) { v = 0; return false; }
    return get_config_uint64(SR_CONF_PROBE_VDIV, v, probe, nullptr);
}

bool DeviceAgent::get_probe_factor(uint64_t &v, sr_channel *probe) {
    if (!is_dsl_device()) return false;
    return get_config_uint64(SR_CONF_PROBE_FACTOR, v, probe, nullptr);
}

bool DeviceAgent::get_probe_coupling(int &v, sr_channel *probe) {
    if (!is_dsl_device()) { v = 0; return false; }
    /* demo 驱动 GET 返回 int32 ("i"), 匹配 sr_key_info_config SR_T_INT32。
     * (DSL 驱动 GET 返回 string, 此方法对 DSL 本就不工作 — DSL 硬件已 dropped) */
    return get_config_int32(SR_CONF_PROBE_COUPLING, v, probe, nullptr);
}

bool DeviceAgent::get_probe_offset(int &v, sr_channel *probe) {
    if (!is_dsl_device()) return false;
    return get_config_uint16(SR_CONF_PROBE_OFFSET, v, probe, nullptr);
}

bool DeviceAgent::get_probe_hw_offset(int &v, sr_channel *probe) {
    if (!is_dsl_device()) return false;
    return get_config_uint16(SR_CONF_PROBE_HW_OFFSET, v, probe, nullptr);
}

bool DeviceAgent::get_probe_map_default(bool &v, sr_channel *probe) {
    if (!is_dsl_device()) return false;
    return get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, v, probe, nullptr);
}

bool DeviceAgent::get_trigger_value(int &v, sr_channel *probe) {
    if (!is_dsl_device()) { v = 0; return false; }
    return get_config_int32(SR_CONF_TRIGGER_VALUE, v, probe, nullptr);
}

uint64_t DeviceAgent::get_trigger_pos() const {
    /* SR_CONF_TRIGGER_POS (60014) is a PXView-local extension implemented
     * only by the PXLogic driver (returns devc->trigger_pos_set). Other
     * drivers don't advertise it → sr_config_get returns SR_ERR_ARG →
     * get_config_uint64 returns false → return 0 (start-of-capture).
     * get_config_uint64 is non-const (it calls sr_config_get), but the
     * operation is logically a read, so const_cast is safe here. */
    uint64_t value = 0;
    const_cast<DeviceAgent*>(this)->get_config_uint64(SR_CONF_TRIGGER_POS, value);
    return value;
}

QVector<uint64_t> DeviceAgent::get_probe_vdiv_list() {
    if (!is_dsl_device())
        return {};

    /* SR_CONF_PROBE_VDIV is a per-channel-group key in the demo driver.
     * Find the first DSO channel's group so sr_config_list reaches the
     * cg branch that actually returns the vdiv list. */
    const struct sr_channel_group *cg = nullptr;
    for (GSList *l = sr_dev_inst_channel_groups_get(_di); l; l = l->next) {
        const struct sr_channel_group *grp =
            (const struct sr_channel_group *)l->data;
        if (grp && grp->channels) {
            const struct sr_channel *ch =
                (const struct sr_channel *)grp->channels->data;
            if (ch && ch->type == SR_CHANNEL_DSO) {
                cg = grp;
                break;
            }
        }
    }

    GVariant *gvar = get_config_list(cg, SR_CONF_PROBE_VDIV);
    if (!gvar)
        return {};
    QVector<uint64_t> result;
    /* Driver may return either:
     *   - a dict {"vdivs": [uint64...]} (a{sv}) — DSL/demo driver format,
     *     consumed by ProbeOptions::bind_vdiv via g_variant_lookup_value.
     *   - a direct uint64 array ("at") — legacy format.
     * Handle both so DsoSignal::init_vDial gets the correct vdiv count
     * (a wrong count causes isMin()&&isMax() deadlock on the dial). */
    GVariant *gvar_vdivs = g_variant_lookup_value(gvar, "vdivs", G_VARIANT_TYPE("at"));
    if (gvar_vdivs) {
        gsize num;
        uint64_t *arr = (uint64_t *)g_variant_get_fixed_array(gvar_vdivs, &num, sizeof(uint64_t));
        for (gsize i = 0; i < num; i++)
            result.append(arr[i]);
        g_variant_unref(gvar_vdivs);
    } else if (g_variant_is_of_type(gvar, G_VARIANT_TYPE("at"))) {
        gsize num;
        uint64_t *arr = (uint64_t *)g_variant_get_fixed_array(gvar, &num, sizeof(uint64_t));
        for (gsize i = 0; i < num; i++)
            result.append(arr[i]);
    } else {
        pxv_warn("get_probe_vdiv_list: unexpected GVariant type for SR_CONF_PROBE_VDIV");
    }
    g_variant_unref(gvar);
    return result;
}

// --- USB link info (replaces deleted SR_CONF_USB_SPEED/USB30_SUPPORT keys) ---

int DeviceAgent::get_usb_speed() {
    if (!_di)
        return PXV_USB_SPEED_UNKNOWN;
    return sr_dev_inst_usb_speed_get(_di);
}

bool DeviceAgent::is_usb30() {
    int speed = get_usb_speed();
    return speed == PXV_USB_SPEED_SUPER || speed == PXV_USB_SPEED_SUPER_PLUS;
}

void *DeviceAgent::get_libusb_device() {
    if (!_di)
        return nullptr;
    return sr_dev_inst_libusb_device_get(_di);
}

bool DeviceAgent::get_vid_pid(uint16_t &vid, uint16_t &pid) {
    vid = 0;
    pid = 0;
    if (!_di)
        return false;
    return sr_dev_inst_usb_vidpid_get(_di, &vid, &pid) == SR_OK;
}

// --- Config info ---

const struct sr_key_info* DeviceAgent::get_config_info(int key)
{
    return sr_key_info_get(SR_KEY_CONFIG, (uint32_t)key);
}

void DeviceAgent::config_changed()
{
    if (_callback)
        _callback->DeviceConfigChanged();
}
