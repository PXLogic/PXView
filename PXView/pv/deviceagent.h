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

#ifndef DEVICE_AGENT_H
#define DEVICE_AGENT_H

#include <glib.h>
#include <stdint.h>
#include <libsigrok/libsigrok.h>
#include <QString>
#include <QVector>
#include <thread>
#include <vector>

#include "dsvdef.h"

class IDeviceAgentCallback
{
    public:
        virtual void DeviceConfigChanged()=0;
        // Called from the worker thread AFTER sr_session_run() returns,
        // i.e. the libsigrok session has fully stopped. This is the upstream
        // equivalent of fork libsigrok's DS_EV_COLLECT_TASK_END event — the
        // reliable "session really stopped" signal that SR_DF_END cannot
        // provide (at SR_DF_END time the main loop is still running).
        // Implementations must marshal onto the main thread before touching
        // UI state (use broadcast_async<TypedEvent>).
        virtual void DeviceSessionStopped() {}
};

/**
 * DeviceAgent — manages the active libsigrok device instance and acquisition
 * session.
 *
 * After the fork libsigrok removal (Task 5-10), DeviceAgent is the sole owner
 * of the sr_session and the active sr_dev_inst. It tracks scanned devices
 * (from sr_driver_scan) and file-loaded devices (from sr_input_scan_file),
 * and exposes handle-based access for SigSession's device-list API.
 *
 * Lifecycle:
 *   1. SigSession::get_device_list() calls sr_driver_scan for all drivers,
 *      registers the scanned SDIs via set_scanned_devices().
 *   2. SigSession::set_device(handle) calls open_by_handle(handle, sr_ctx),
 *      which finds the SDI by handle (index+1), opens it via sr_dev_open,
 *      creates sr_session, adds the device, and registers the datafeed
 *      callback.
 *   3. CaptureManager::exec_capture() calls start() → sr_session_start +
 *      spawns _session_thread running sr_session_run() (blocks until the
 *      session stops). This mirrors the fork libsigrok collect_thread.
 *      Upstream libsigrok 0.6.0's sr_session_start() registers event
 *      sources but does NOT pump the GLib main context — without a thread
 *      running g_main_loop, source callbacks (e.g. demo's prepare_data
 *      timer) never fire and no SR_DF_LOGIC packets are emitted.
 *   4. CaptureManager::action_stop_capture() calls stop() → sr_session_stop
 *      (signals main loop to quit) + joins _session_thread.
 *   5. SigSession::set_device() calls release() before opening a new device,
 *      which stops the thread, destroys the session and closes the device.
 *
 * The datafeed callback is injected via set_datafeed_callback() by
 * SessionStateContext (which owns the DataFeedParser).
 */
class DeviceAgent
{
public:
    DeviceAgent();
    ~DeviceAgent();

    // --- Device list management ---
    // Called by SigSession::get_device_list(). Registers scanned SDIs.
    // Handle = index+1 (0 reserved for NULL_HANDLE).
    void set_scanned_devices(const std::vector<struct sr_dev_inst*> &sdis);

    // Returns the cached scanned SDI list (populated by set_scanned_devices).
    // SigSession::get_device_list() uses this to avoid repeated sr_driver_scan.
    const std::vector<struct sr_dev_inst*> &scanned_sdi() const { return _scanned_sdi; }

    // Called by SigSession::set_file(). Registers a file-loaded SDI.
    ds_device_handle set_file_device(struct sr_dev_inst *sdi, const QString &name);

    // Called by SigSession::close_file(). Removes a device by handle.
    void remove_device(ds_device_handle handle);

    // Called by SigSession::get_device_list(). Returns file-loaded devices.
    std::vector<struct sr_dev_inst*> &file_devices() { return _file_sdi; }

    // --- Lifecycle ---
    // Opens the device by handle. Creates sr_session, adds device, registers
    // datafeed callback. Returns false on failure.
    bool open_by_handle(ds_device_handle handle, struct sr_context *ctx);

    // Releases the active device: destroys session, closes device.
    void release();

    // Refresh device info (name/driver/type) from the active SDI.
    void update();

    // --- Datafeed callback registration ---
    void set_datafeed_callback(sr_datafeed_callback cb, void *user_data);

    // --- Accessors ---
    inline bool have_instance() const { return _dev_handle != NULL_HANDLE; }
    inline QString name() const { return _dev_name; }
    inline QString path() const { return _path; }
    inline QString driver_name() const { return _driver_name; }
    inline ds_device_handle handle() const { return _dev_handle; }

    struct sr_dev_inst* inst();
    struct sr_session* sr_session() { return _sr_session; }

    inline bool is_file() const { return _dev_type == DEV_TYPE_FILELOG; }
    inline bool is_demo() const { return _dev_type == DEV_TYPE_DEMO; }
    inline bool is_hardware() const { return _dev_type == DEV_TYPE_USB; }
    inline bool is_virtual() const { return is_file() || is_demo(); }

    inline bool is_hardware_logic() const {
        return is_hardware() && (_driver_name == "DSLogic" ||
                                 _driver_name.startsWith("px", Qt::CaseInsensitive));
    }
    inline bool is_hardware_dso() const {
        return is_hardware() && _driver_name == "DSCope";
    }
    inline bool is_dsl_device() const {
        return is_hardware() && (_driver_name == "DSLogic" ||
                                 _driver_name == "DSCope" ||
                                 _driver_name.startsWith("px", Qt::CaseInsensitive));
    }
    inline bool is_compat_device() const {
        return is_hardware() && !is_dsl_device();
    }

    inline void set_callback(IDeviceAgentCallback *callback) {
        _callback = callback;
    }

    // --- Channel operations ---
    bool enable_probe(const sr_channel *probe, bool enable);
    bool enable_probe(int probe_index, bool enable);
    bool set_channel_name(int ch_index, const char *name);
    bool channel_is_enable(int index);
    GSList* get_channels();
    int get_channel_count();
    bool have_enabled_channel();

    // --- Sample config ---
    // get_sample_limit() 返回用户选择的采样深度对应的样本数（停止条件）。
    // 在 stream 模式下若驱动 limit_samples=0（持续流），返回应用层默认值，
    // 不再返回 ring buffer 大小。
    uint64_t get_sample_limit();
    // get_ring_sample_count() 返回 ring buffer 大小（用于 mmap 内存分配）。
    // stream 模式下基于 _app_stream_mem_buff(GB) 计算；非 stream 模式与
    // get_sample_limit() 等价。
    uint64_t get_ring_sample_count();
    // get_hw_depth() 返回硬件存储深度（每通道样本数）。Buffer 模式下为 FPGA
    // DRAM 容量 / 通道数；用于 SamplingBar 构建采样深度下拉框上限，防止用户
    // 选择超过硬件存储能力的深度。驱动不支持 SR_CONF_HW_DEPTH 时返回 0。
    uint64_t get_hw_depth();
    uint64_t get_sample_rate();
    uint64_t get_time_base();
    double get_sample_time();

    // --- Mode ---
    int get_work_mode();
    void set_work_mode(int mode);
    const GSList* get_device_mode_list();
    int get_hardware_operation_mode();
    bool is_stream_mode();
    bool detect_stream_mode();
    QString get_demo_operation_mode();
    bool check_firmware_version();

    // --- Trigger ---
    bool is_trigger_enabled();

    // --- Acquisition ---
    bool start();
    bool stop();
    bool is_collecting();
    inline bool is_new_device() const { return _is_new_device; }

    // --- Config (via sdi->driver->config_get/set/list) ---
    GVariant* get_config_list(const sr_channel_group *group, int key);
    GVariant* get_config(int key, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config(int key, GVariant *data, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool have_config(int key, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_string(int key, QString &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_string(int key, const char *value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_bool(int key, bool &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_bool(int key, bool value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_uint64(int key, uint64_t &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_uint64(int key, uint64_t value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_uint16(int key, int &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_uint16(int key, int value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_uint32(int key, uint32_t &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_uint32(int key, uint32_t value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_int16(int key, int &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_int16(int key, int value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_int32(int key, int &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_int32(int key, int value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_byte(int key, int &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_byte(int key, int value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    bool get_config_double(int key, double &value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);
    bool set_config_double(int key, double value, const sr_channel *ch = NULL, const sr_channel_group *cg = NULL);

    // --- sr_config create/free (fork libsigrok API stub) ---
    // Fork libsigrok exposed ds_new_config / ds_free_config for building
    // sr_config entries to attach to sr_datafeed_meta packets (used by
    // StoreSession export). Upstream libsigrok does not provide these —
    // implement inline as simple g_new0/g_free wrappers.
    struct sr_config *new_config(int key, GVariant *data);
    void free_config(struct sr_config *src);

    // --- option_value_to_code (fork libsigrok API replacement) ---
    // Fork libsigrok exposed ds_option_value_to_code for converting config
    // option string values to integer codes. Upstream libsigrok does not
    // provide this directly — implemented here by querying the driver's
    // config_list for `key` (as a GVariant string array) and returning the
    // index of the entry matching `value`. Returns -1 if no match is found
    // or the list is unavailable (caller falls back to default value 0).
    // `mode` is retained for API compatibility but not applied — the
    // driver's config_list handler already returns mode-appropriate options.
    int option_value_to_code(int mode, int key, const char *value);

    // --- Typed wrappers (View-layer convenience over get_config_*) ---
    bool is_roll_mode(bool &roll);
    bool get_unit_bits(int &v);
    bool get_ref_min(uint32_t &v);
    bool get_ref_max(uint32_t &v);
    bool get_probe_vdiv(uint64_t &v, sr_channel *probe);
    bool get_probe_factor(uint64_t &v, sr_channel *probe);
    bool get_probe_coupling(int &v, sr_channel *probe);
    bool get_probe_offset(int &v, sr_channel *probe);
    bool get_probe_hw_offset(int &v, sr_channel *probe);
    bool get_probe_map_default(bool &v, sr_channel *probe);
    bool get_trigger_value(int &v, sr_channel *probe);
    // PXView-local: real trigger sample position (uint64, read-only). Only
    // the PXLogic driver exposes it via SR_CONF_TRIGGER_POS. Returns 0 for
    // devices that don't support it (start-of-capture fallback). Safe to
    // call when no device is connected (get_config handles the null case).
    uint64_t get_trigger_pos() const;
    QVector<uint64_t> get_probe_vdiv_list();

    // --- USB link info (replaces deleted SR_CONF_USB_SPEED/USB30_SUPPORT keys) ---
    // Returns PXV_USB_SPEED_* (LOW=1/FULL=2/HIGH=3/SUPER=4/SUPER_PLUS=5),
    // defined in dsvdef.h. PXV_USB_SPEED_UNKNOWN (0) for non-USB devices or
    // when speed cannot be determined. Values mirror libusb's enum so View
    // code may compare against LIBUSB_SPEED_* interchangeably. Reads
    // libusb_get_device_speed via libsigrok's sr_dev_inst_usb_speed_get() —
    // no driver config_get needed.
    int get_usb_speed();
    // Convenience: true when get_usb_speed() == PXV_USB_SPEED_SUPER or
    // PXV_USB_SPEED_SUPER_PLUS. Returns false for UNKNOWN (treats as USB 2.0).
    bool is_usb30();

    // Returns the active device's underlying libusb_device* (as void* to
    // avoid pulling libusb.h into the header). NULL when no device is open
    // or the device is not USB. Intended for pointer-identity comparison
    // against hotplug DETACH device_handle values — comparing two pointer
    // values is safe even if one has been freed by libusb (no dereference).
    void *get_libusb_device();

    // Reads the active device's USB VID/PID from the libusb device
    // descriptor. Returns false on failure (non-USB device, no device, or
    // descriptor unreadable). Used by hotplug ATTACH rebind to match a
    // re-enumerated device by VID/PID.
    bool get_vid_pid(uint16_t &vid, uint16_t &pid);

    // --- Config info ---
    const struct sr_key_info* get_config_info(int key);

    // Find sdi by handle — needed by SigSession::set_default_device() to
    // match last-used device by driver name + connection ID.
    struct sr_dev_inst* find_sdi_by_handle(ds_device_handle handle);

private:
    void config_changed();
    void stop_session_thread(); // join _session_thread if joinable

    ds_device_handle _dev_handle = NULL_HANDLE;
    int         _dev_type = 0;
    QString     _dev_name;
    QString     _driver_name;
    QString     _path;
    bool        _is_new_device = false;
    struct sr_dev_inst  *_di = nullptr;
    struct sr_session   *_sr_session = nullptr;
    struct sr_context   *_sr_ctx = nullptr;
    IDeviceAgentCallback *_callback = nullptr;

    // Datafeed callback (injected by SessionStateContext)
    sr_datafeed_callback _datafeed_cb = nullptr;
    void *_datafeed_cb_data = nullptr;

    // Session run thread — pumps the GLib main loop so that event sources
    // registered by sr_session_source_add() (e.g. demo's 100ms prepare_data
    // timer) actually fire. Fork libsigrok did this internally via
    // collect_thread/g_main_loop_run; upstream libsigrok 0.6.0 leaves it
    // to the caller.
    std::thread _session_thread;

    // Tracked devices: scanned (from sr_driver_scan) + file-loaded.
    std::vector<struct sr_dev_inst*> _scanned_sdi;
    std::vector<struct sr_dev_inst*> _file_sdi;

    // --- App-layer config state (C-class keys, not driver-backed) ---
    // These keys (DISK_CACHE_ENABLE/PATH, STREAM_BUFF/STREAM_MEM_BUFF) are
    // application-layer concepts — disk cache is implemented by
    // LogicSnapshotDiskCacheWriter + MmapAllocator, not by any driver.
    // They are stored here so non-DSL devices (fx2lafw, etc.) get sensible
    // defaults without the driver needing to implement config_get/set for
    // these keys. DSL/PXLogic devices still forward to the driver so the
    // existing pxlogic config_get/set path works unchanged.
    bool    _app_disk_cache_enable = false;
    QString _app_disk_cache_path;
    double  _app_stream_buff = 16.0;       // GB, disk cache total depth
    double  _app_stream_mem_buff = 16.0;   // GB, in-memory ring buffer

    // App-layer stream mode override (SR_CONF_STREAM). Lazy-initialized on
    // first is_stream_mode() call from the driver's reported capability
    // (SR_CONF_CONTINUOUS for upstream drivers, SR_CONF_OPERATION_MODE for
    // DSL/PXLogic). User can toggle it via the deviceoptions panel — the
    // cached value is then returned by is_stream_mode() so samplingbar's
    // loop-mode button and sample-count list reflect the user's choice.
    bool    _app_stream_mode = false;
    bool    _app_stream_mode_init = false;

    // App-layer work mode cache (for non-DSL devices that don't implement
    // SR_CONF_DEVICE_MODE). Stores the current work mode so the DevMode
    // toolbar button can switch modes (LOGIC/ANALOG/DSO/MSO) without driver
    // support. DSL/PXLogic devices still use SR_CONF_DEVICE_MODE via the
    // driver, but this cache is always updated so get_work_mode() is
    // consistent.
    int     _app_work_mode = LOGIC;

    // Cached mode list built by get_device_mode_list() based on the device's
    // channel capabilities (logic/analog/DSO). Owned by DeviceAgent — freed
    // in release() and rebuilt on each open_by_handle().
    GSList  *_mode_list_cache = nullptr;
};

#endif
