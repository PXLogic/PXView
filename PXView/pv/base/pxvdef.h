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

#include <cstdint>
#include <cstdlib>

// ---------------------------------------------------------------------------
// PXView local definitions — formerly pxvdef.h, modernized to C++20.
//
// This header is the lightweight definition file for PXView-local types,
// enums, macros, and constants that were previously defined in the PXView
// fork libsigrok.h. After migrating to upstream libsigrok 0.6.0, these
// definitions live here so PXView code compiles without the fork.
//
// This header does NOT include <libsigrok/libsigrok.h>. The struct
// sr_dev_inst is only used as a pointer member in ds_device_full_info,
// so a forward declaration suffices.
// ---------------------------------------------------------------------------

// --- Utility macros (C++20 modernized) ---

template <typename T, std::size_t N>
constexpr std::size_t countof(T (&)[N]) noexcept { return N; }

#define begin_element(x) (&x[0])
#define end_element(x)   (&x[countof(x)])

// --- View types ---

enum View_type {
    TIME_VIEW,
    FFT_VIEW,
    ALL_VIEW
};

enum DEVICE_COLLECT_MODE {
    COLLECT_SINGLE = 0,
    COLLECT_REPEAT = 1,
    COLLECT_LOOP   = 2,
};

enum DEVICE_STATUS_TYPE {
    ST_INIT    = 0,
    ST_RUNNING = 1,
    ST_STOPPED = 2,
};

// --- PXView-local device handle types ---

using ds_device_handle = uint64_t;
#define NULL_HANDLE ((ds_device_handle)0)

// Device list entry returned by SigSession::get_device_list().
struct ds_device_base_info {
    ds_device_handle handle;
    char name[150];
};

// --- Glitch filter mode ---
// (Moved from logicsnapshot.h so sigsession.h can forward-declare
// LogicSnapshot instead of including the heavy snapshot header.)
// Spec v2 Task 4: changed to enum class with fixed underlying type so
// filterprocessor.h can forward-declare it without including logicsnapshot.h.
enum class GlitchFilterMode : int {
    Both = 0,
    High = 1,
    Low  = 2
};

// 过渡兼容别名（标记 deprecated，后续移除）
[[deprecated("Use GlitchFilterMode::Both")]]
inline constexpr GlitchFilterMode GLITCH_FILTER_BOTH = GlitchFilterMode::Both;
[[deprecated("Use GlitchFilterMode::High")]]
inline constexpr GlitchFilterMode GLITCH_FILTER_HIGH = GlitchFilterMode::High;
[[deprecated("Use GlitchFilterMode::Low")]]
inline constexpr GlitchFilterMode GLITCH_FILTER_LOW = GlitchFilterMode::Low;

// Forward declaration — the actual definition is in <libsigrok/libsigrok.h>.
struct sr_dev_inst;

// Extended device info used internally by DeviceAgent.
struct ds_device_full_info {
    ds_device_handle handle;
    char name[150];
    char path[256];
    char driver_name[20];
    int  dev_type;
    int  actived_times;
    struct sr_dev_inst *sdi;
};

// Device type classification (mirrors upstream sr_dev_inst_type but kept
// as a PXView-local enum for backward compat with existing code paths).
enum sr_device_type {
    DEV_TYPE_UNKOWN  = 0,
    DEV_TYPE_DEMO,
    DEV_TYPE_FILELOG,
    DEV_TYPE_USB,
    DEV_TYPE_SERIAL,
};

// --- Work mode ---

// LOGIC = logic analyzer, DSO = oscilloscope (deprecated), ANALOG = analog,
// MSO = mixed signal oscilloscope (application-layer mode for devices that
// declare both LOGIC_ANALYZER and OSCILLOSCOPE capabilities).
inline constexpr int LOGIC  = 0;
inline constexpr int DSO    = 1;
inline constexpr int ANALOG = 2;
inline constexpr int MSO    = 3;

// --- Trigger mode ---

inline constexpr int SIMPLE_TRIGGER  = 0;
inline constexpr int ADV_TRIGGER     = 1;
inline constexpr int SERIAL_TRIGGER  = 2;

// --- Fork libsigrok trigger position struct ---
// Binary-compatible with pxlogic driver's pxlogic_trigger_pos. Used by
// SR_DF_TRIGGER payload internally for trigger position tracking.
struct ds_trigger_pos {
    uint16_t check_id;
    uint64_t real_pos;
    uint32_t ram_saddr;
    uint16_t remain_cnt_l;
    uint16_t remain_cnt_h;
    uint8_t  status;
};

// --- Max probe count constants ---

inline constexpr int DS_MAX_DSO_PROBES_NUM    = 16;
inline constexpr int DS_MAX_ANALOG_PROBES_NUM = 16;

// --- min/max helper macros (C++20: prefer std::min/std::max in new code) ---

#define ds_min(a, b)  (((a) < (b)) ? (a) : (b))
#define ds_max(a, b)  (((a) > (b)) ? (a) : (b))

// --- Channel type extensions ---
// PXView-local channel types for non-hardware signal categories.
// Values 10003+ avoid collision with upstream SR_CHANNEL_LOGIC(10000)/
// SR_CHANNEL_DSO(10001)/SR_CHANNEL_ANALOG(10002).

inline constexpr int SR_CHANNEL_DECODER   = 10003;
inline constexpr int SR_CHANNEL_FFT       = 10004;
inline constexpr int SR_CHANNEL_LISSAJOUS = 10005;
inline constexpr int SR_CHANNEL_MATH      = 10006;
inline constexpr int SR_CHANNEL_GROUP     = 10007;

// --- Backward-compat aliases ---

#define sr_input_format  sr_input_module
#define sr_config_info   sr_key_info

// --- DSO display constants ---

inline constexpr int DS_CONF_DSO_VDIVS = 8;
inline constexpr int DS_CONF_DSO_HDIVS = 10;

// --- Hardware operation modes ---

inline constexpr int LO_OP_BUFFER = 0;
inline constexpr int LO_OP_STREAM = 1;
inline constexpr int LO_OP_INTEST = 2;

// --- Sample alignment ---

inline constexpr int SAMPLES_ALIGN = 63;

// --- Trigger constants ---

inline constexpr int TriggerStages       = 4;
inline constexpr int TriggerProbes       = 16;
inline constexpr int DS_MAX_TRIG_PERCENT = 100;

// --- DSO trigger source ---

inline constexpr int DSO_TRIGGER_AUTO   = 0;
inline constexpr int DSO_TRIGGER_CH0    = 1;
inline constexpr int DSO_TRIGGER_CH1    = 2;
inline constexpr int DSO_TRIGGER_CH0A1  = 3;
inline constexpr int DSO_TRIGGER_CH0O1  = 4;

// --- DSO trigger type ---

inline constexpr int DSO_TRIGGER_RISING  = 0;
inline constexpr int DSO_TRIGGER_FALLING = 1;

// --- Coupling ---

inline constexpr int SR_GND_COUPLING = 0;
inline constexpr int SR_DC_COUPLING  = 1;
inline constexpr int SR_AC_COUPLING  = 2;

// --- SI unit multipliers ---

#define SR_Kn(x)  ((uint64_t)(x) * 1000ULL)
#define SR_KB(x)  ((uint64_t)(x) * 1000ULL)
#define SR_Mn(x)  ((uint64_t)(x) * 1000000ULL)
#define SR_GB(x)  ((uint64_t)(x) * 1000000000ULL)

// --- Time unit macros (duration in nanoseconds) ---

#define SR_SEC(x)  ((uint64_t)(x) * 1000000000ULL)
#define SR_MIN(x)  ((uint64_t)(x) * 60ULL * 1000000000ULL)
#define SR_HOUR(x) ((uint64_t)(x) * 3600ULL * 1000000000ULL)
#define SR_DAY(x)  ((uint64_t)(x) * 86400ULL * 1000000000ULL)

// --- Time string formatter ---
// Stub implementation in deviceagent.cpp formats duration (nanoseconds)
// as a human-readable string. Caller must g_free() the returned pointer.
char *sr_time_string(uint64_t duration);

// --- DSO measurement type enum ---

enum DSO_MEASURE_TYPE {
    DSO_MS_BEGIN = 0,
    DSO_MS_FREQ  = 1,
    DSO_MS_PERD  = 2,
    DSO_MS_PDUT  = 3,
    DSO_MS_NDUT  = 4,
    DSO_MS_PCNT  = 5,
    DSO_MS_RISE  = 6,
    DSO_MS_FALL  = 7,
    DSO_MS_PWDT  = 8,
    DSO_MS_NWDT  = 9,
    DSO_MS_BRST  = 10,
    DSO_MS_AMPT  = 11,
    DSO_MS_VHIG  = 12,
    DSO_MS_VLOW  = 13,
    DSO_MS_VRMS  = 14,
    DSO_MS_VMEA  = 15,
    DSO_MS_VP2P  = 16,
    DSO_MS_VMAX  = 17,
    DSO_MS_VMIN  = 18,
    DSO_MS_POVR  = 19,
    DSO_MS_NOVR  = 20,
    DSO_MS_END   = 21,
};

// --- Packet constants ---

inline constexpr int SR_PKT_OK     = 0;
inline constexpr int SR_DF_OVERFLOW = 10009;

// --- Device mode list entry ---

struct sr_dev_mode {
    int mode;
    const char *acronym;
};

extern const sr_dev_mode kDevModeLogic;
extern const sr_dev_mode kDevModeAnalog;
extern const sr_dev_mode kDevModeDso;
extern const sr_dev_mode kDevModeMso;

// --- Object destruction macros (modernized: nullptr → nullptr) ---

#define DESTROY_OBJECT(p)     do { if ((p)) { delete (p); (p) = nullptr; } } while (0)
#define DESTROY_QT_OBJECT(p)  do { if ((p)) { (p)->deleteLater(); (p) = nullptr; } } while (0)
#define DESTROY_QT_LATER(p)   ((p))->deleteLater()

#define RELEASE_ARRAY(a) do { for (auto ptr : (a)) { delete ptr; } (a).clear(); } while (0)

#define ABS_VAL(x) ((x) > 0 ? (x) : -(x))

// --- Format versions ---

inline constexpr int SESSION_FORMAT_VERSION = 3;
inline constexpr int HEADER_FORMAT_VERSION  = 3;

// --- Decoder data format ---

namespace DecoderDataFormat {
    enum _data_format {
        hex   = 0,
        dec   = 1,
        oct   = 2,
        bin   = 3,
        ascii = 4
    };

    int Parse(const char *name);
}

// --- USB link speed constants ---
// Mirror libusb's libusb_speed enum values so Core-layer code can
// interpret sr_dev_inst_usb_speed_get() without including <libusb.h>.

inline constexpr int PXV_USB_SPEED_UNKNOWN    = 0;
inline constexpr int PXV_USB_SPEED_LOW       = 1;  // USB 1.1 (1.5 Mbps)
inline constexpr int PXV_USB_SPEED_FULL      = 2;  // USB 1.1 (12 Mbps)
inline constexpr int PXV_USB_SPEED_HIGH      = 3;  // USB 2.0 (480 Mbps)
inline constexpr int PXV_USB_SPEED_SUPER     = 4;  // USB 3.0 (5 Gbps)
inline constexpr int PXV_USB_SPEED_SUPER_PLUS = 5;  // USB 3.1+ (10+ Gbps)
