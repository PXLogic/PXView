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

#include <stdint.h>

#define countof(x) (sizeof(x)/sizeof(x[0]))

#define begin_element(x) (&x[0])
#define end_element(x) (&x[countof(x)])

enum View_type {
    TIME_VIEW,
    FFT_VIEW,
    ALL_VIEW
};

enum DEVICE_COLLECT_MODE {
    COLLECT_SINGLE = 0,
    COLLECT_REPEAT = 1,
    COLLECT_LOOP = 2,
};

enum DEVICE_STATUS_TYPE {
  ST_INIT = 0,
  ST_RUNNING = 1,
  ST_STOPPED = 2,
};

// --- PXView-local device handle types ---
// These were previously defined in PXView's fork libsigrok.h. After migrating
// to upstream libsigrok 0.6.0 (libsigrok), they are defined locally here.
// ds_device_handle is an opaque token used by SigSession's device-list API.
// The actual device identity is carried by struct sr_dev_inst* in DeviceAgent.
typedef uint64_t ds_device_handle;
#define NULL_HANDLE ((ds_device_handle)0)

// Device list entry returned by SigSession::get_device_list().
// The handle is an opaque token; name is a UTF-8 display string.
struct ds_device_base_info {
    ds_device_handle handle;
    char name[150];
};

// Extended device info used internally by DeviceAgent.
struct ds_device_full_info {
    ds_device_handle handle;
    char name[150];
    char path[256];
    char driver_name[20];
    int dev_type;
    int actived_times;
    struct sr_dev_inst *sdi;
};

// Device type classification (mirrors upstream sr_dev_inst_type but kept
// as a PXView-local enum for backward compat with existing code paths).
enum sr_device_type {
    DEV_TYPE_UNKOWN = 0,
    DEV_TYPE_DEMO,
    DEV_TYPE_FILELOG,
    DEV_TYPE_USB,
    DEV_TYPE_SERIAL,
};

// Work mode (logic analyzer / oscilloscope / analog / mixed signal).
// Note: DSO mode is deprecated (DSCope hardware dropped), but the enum
// value is retained for TriggerConfig/UI compatibility.
// MSO is an application-layer mode used by the mode button (DevMode widget)
// for mixed-signal devices (e.g. demo) that declare both LOGIC_ANALYZER and
// OSCILLOSCOPE capabilities — it shows both logic and analog channels.
enum {
    LOGIC = 0,
    DSO = 1,
    ANALOG = 2,
    MSO = 3,            // Mixed Signal Oscilloscope — show logic + analog channels
};

// Trigger mode (UI retained for Adv/Serial; only Simple is synced to driver).
enum {
    SIMPLE_TRIGGER = 0,
    ADV_TRIGGER = 1,
    SERIAL_TRIGGER = 2,
};

// Fork libsigrok trigger position struct (binary-compatible with pxlogic
// driver's pxlogic_trigger_pos). Used by SR_DF_TRIGGER payload in the fork
// data feed. Upstream SR_DF_TRIGGER has NO payload, so this is only used
// internally for trigger position tracking.
struct ds_trigger_pos {
    uint16_t check_id;
    uint64_t real_pos;
    uint32_t ram_saddr;
    uint16_t remain_cnt_l;
    uint16_t remain_cnt_h;
    uint8_t status;
};

// Fork libsigrok DSO status struct deleted — DSO mode is deprecated and all
// DSO status code paths have been removed (dso_measure / storesession /
// datasource / sessionstatecontext).

// sr_datafeed_dso is now defined in upstream libsigrok.h.

// Fork libsigrok max probe count constants (used to size fixed envelope arrays).
// DSO mode is deprecated; values kept conservative so legacy fixed-size arrays
// in DsoSnapshot/AnalogSnapshot compile without modification.
#define DS_MAX_DSO_PROBES_NUM     16
#define DS_MAX_ANALOG_PROBES_NUM  16

// Fork libsigrok ds_min / ds_max helper macros. Upstream libsigrok does not
// provide these; alias to standard min/max so DSO envelope code compiles.
#define ds_min(a, b)  (((a) < (b)) ? (a) : (b))
#define ds_max(a, b)  (((a) > (b)) ? (a) : (b))

// Fork libsigrok channel type extensions (upstream only has LOGIC/ANALOG).
// These are PXView-local channel types used by SignalModel for non-hardware
// signal categories (decoder/FFT/Lissajous/Math/Group). Values 10003+ avoid
// collision with upstream SR_CHANNEL_LOGIC(10000)/SR_CHANNEL_DSO(10001, fork)
// /SR_CHANNEL_ANALOG(10002).
#define SR_CHANNEL_DECODER    10003
#define SR_CHANNEL_FFT        10004
#define SR_CHANNEL_LISSAJOUS  10005
#define SR_CHANNEL_MATH       10006
#define SR_CHANNEL_GROUP      10007

// SR_CONF_PROBE_EN (60049) is now defined in libsigrok.h enum
// (DSL Driver Extension Keys block). Was a fork-only stub here previously.

// Backward-compat alias: upstream libsigrok renamed sr_input_format to
// sr_input_module. Code referencing the old name is updated gradually.
#define sr_input_format sr_input_module

// Fork libsigrok DSO vertical-division count (used by DSO/Analog/Math/Spectrum
// scaling). Standard oscilloscope displays have 8 vertical divisions.
// DSO mode is deprecated; value retained for stub compatibility.
#define DS_CONF_DSO_VDIVS  8

// Fork libsigrok DSO horizontal-division count. Standard oscilloscope
// displays have 10 horizontal divisions. Used by DSO time-base math
// (sigsession.cpp / samplingbar.cpp / dsosignal.cpp).
#define DS_CONF_DSO_HDIVS  10

// Fork libsigrok hardware operation modes (returned by
// SR_CONF_OPERATION_MODE). LO_OP_BUFFER = buffered capture, LO_OP_STREAM =
// streaming capture. PXLogic hardware uses these to select capture mode.
#define LO_OP_BUFFER  0
#define LO_OP_STREAM  1
#define LO_OP_INTEST  2

// Fork libsigrok sample alignment mask. Used by samplingbar.cpp to align
// sample counts to DMA buffer boundaries (64-byte alignment).
#define SAMPLES_ALIGN  63

// Fork libsigrok trigger constants. Used by triggerdock.cpp for trigger stage
// count and probe count. TriggerStages = max advanced trigger stages (4).
// TriggerProbes = max trigger probe count (16). DS_MAX_TRIG_PERCENT = max
// trigger position percentage (100).
#define TriggerStages       4
#define TriggerProbes       16
#define DS_MAX_TRIG_PERCENT 100

// Task 10.8: struct sr_list_item deleted — config_list now returns standard
// GVariant string arrays via g_variant_new_strv. View layer reads via
// g_variant_get_strv instead of casting uint64 to sr_list_item*.

// Fork libsigrok DSO trigger source flag. DSO_TRIGGER_AUTO = auto-trigger
// (no external trigger). Used by capturemanager.cpp / viewport_painter.cpp /
// dsotriggerdock.cpp. DSO mode is deprecated.
#define DSO_TRIGGER_AUTO  0

// SR_CONF_TRIGGER_CHANNEL (60078) / SR_CONF_TRIGGER_HOLDOFF (60079) /
// SR_CONF_TRIGGER_MARGIN (60080) are now defined in libsigrok.h enum
// (DSL Driver Extension Keys block). Values changed from old fork 30012/
// 30015/30016 to avoid conflict with upstream SR_CONF_VDIV/SAMPLE_INTERVAL/
// NUM_HDIV (which occupied those 30000-range slots in upstream 0.6.0).

// Fork libsigrok time-unit conversion macro. SR_SEC(x) converts x seconds to
// the internal time-base unit (nanoseconds). Used by sigsession.cpp /
// samplingbar.cpp for time-base math: total_time = timebase * HDIVS / SR_SEC(1).
#define SR_SEC(x)  ((uint64_t)(x) * 1000000000ULL)

// SR_CONF_WAIT_UPLOAD (60050) / SR_CONF_ACTUAL_SAMPLES (60056) /
// SR_CONF_FILE_VERSION (60058) are now defined in libsigrok.h enum
// (DSL Driver Extension Keys block). Values preserved from old fork stubs.

// Fork libsigrok analog probe mapping config keys were previously defined
// here as 60059/60060/60061. They are now enum members of sr_config_keys
// in upstream libsigrok.h (same numeric values). No #define stubs needed.

// SR_CONF_CLOCK_TYPE (60081) / SR_CONF_BANDWIDTH_LIMIT (60082) /
// SR_CONF_BANDWIDTH (60083) are now defined in libsigrok.h enum
// (DSL Driver Extension Keys block).

// Fork libsigrok DSO trigger source/type enum values. Used by dsotriggerdock.cpp
// (QButtonGroup IDs), dsosignal.cpp (slope comparison), capturemanager.cpp,
// viewport_painter.cpp. DSO mode is deprecated; values are button-group IDs
// so actual integers only need to be distinct within each group.
// Source group (DSO_TRIGGER_*): AUTO=0, CH0=1, CH1=2, CH0A1=3, CH0O1=4.
// Type group: RISING=0, FALLING=1.
#define DSO_TRIGGER_CH0    1
#define DSO_TRIGGER_CH1    2
#define DSO_TRIGGER_CH0A1  3
#define DSO_TRIGGER_CH0O1  4
#define DSO_TRIGGER_RISING   0
#define DSO_TRIGGER_FALLING  1

// Fork libsigrok coupling enum values. Upstream libsigrok only exposes
// SR_CONF_COUPLING as an integer config key (no enum). Used by probeoptions.cpp
// and dsosignal.cpp to decode the coupling value returned by
// get_config_int16(SR_CONF_PROBE_COUPLING). Typical oscilloscope coupling modes.
#define SR_GND_COUPLING  0
#define SR_DC_COUPLING   1
#define SR_AC_COUPLING   2

// Fork libsigrok SI unit multiplier macros. SR_NS/SR_US/SR_MS are now
// defined in upstream libsigrok.h (same values); the remaining multipliers
// (SR_Kn/SR_KB/SR_Mn/SR_GB) are still PXView-local.
#define SR_Kn(x)  ((uint64_t)(x) * 1000ULL)
#define SR_KB(x)  ((uint64_t)(x) * 1000ULL)
#define SR_Mn(x)  ((uint64_t)(x) * 1000000ULL)
#define SR_GB(x)  ((uint64_t)(x) * 1000000000ULL)

// Fork libsigrok time unit macros (duration in nanoseconds). Used by
// samplingbar.cpp for time-base calculation. SR_SEC already defined above
// as ((uint64_t)(x) * 1000000000ULL).
#define SR_MIN(x)  ((uint64_t)(x) * 60ULL * 1000000000ULL)
#define SR_HOUR(x) ((uint64_t)(x) * 3600ULL * 1000000000ULL)
#define SR_DAY(x)  ((uint64_t)(x) * 86400ULL * 1000000000ULL)

// Fork libsigrok time string formatter. Upstream libsigrok provides
// sr_samplerate_string / sr_voltage_string but not sr_time_string. Stub
// implementation in deviceagent.cpp formats duration (nanoseconds) as a
// human-readable string. Caller must g_free() the returned pointer.
char *sr_time_string(uint64_t duration);

// SR_T_UINT8 (10012) / SR_T_INT16 (10013) / SR_T_CHAR (10014) /
// SR_T_LIST (10015) / SR_T_UINT16 (10016) are now defined in libsigrok.h
// enum sr_datatype (DSL Driver Extension types block).

// Fork libsigrok DSO measurement type enum. Used by DsoMeasure / DsoSignal /
// ViewStatus for DSO auto-measurements. DSO mode is deprecated; values
// retained so the DSO UI code compiles until full removal.
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

// Fork libsigrok packet status code.
#define SR_PKT_OK 0

// Fork libsigrok packet types not in upstream.
// SR_DF_DSO is now in upstream libsigrok enum (value 10008).
#define SR_DF_OVERFLOW 10009

// Backward-compat alias: upstream libsigrok renamed sr_config_info to
// sr_key_info. Callers that reference sr_config_info are updated gradually.
#define sr_config_info sr_key_info

// Fork libsigrok device-mode list entry (fork sr_dev_mode). Upstream
// libsigrok does not expose a device-mode list (SR_CONF_DEVICE_MODE returns
// a GVariant, not a GSList of sr_dev_mode). DeviceAgent::get_device_mode_list()
// returns a static list built from these entries, based on the connected
// device's capabilities (logic channels / analog channels / DSO channels).
struct sr_dev_mode {
    int mode;
    const char *acronym;
};

// Static device-mode entries for the four work modes shown in the DevMode
// toolbar button. acronym is used by storesession.cpp to build default save
// filenames (e.g. "demo-MSO-260707-120000.pxc").
extern const sr_dev_mode kDevModeLogic;
extern const sr_dev_mode kDevModeAnalog;
extern const sr_dev_mode kDevModeDso;
extern const sr_dev_mode kDevModeMso;

// --- Fork Analog SR_CONF_* keys (now defined in upstream libsigrok.h) ---
// SR_CONF_PROBE_OFFSET / SR_CONF_PROBE_HW_OFFSET / SR_CONF_PROBE_MAP_DEFAULT
// / SR_CONF_REF_MIN / SR_CONF_REF_MAX / SR_CONF_UNIT_BITS / SR_CONF_PROBE_FACTOR
// / SR_CONF_PROBE_MAP_UNIT / SR_CONF_PROBE_MAP_MIN / SR_CONF_PROBE_MAP_MAX
// are now enum members of sr_config_keys in libsigrok.h (ported from the
// PXView fork). No #define stubs needed here anymore.
//
// SR_CONF_PROBE_MAP_UNIT/MIN/MAX were previously defined here as 60059/60060/
// 60061 — those #define stubs have been removed; the libsigrok.h enum uses the
// same numeric values so PXView code behavior is unchanged.

#define DESTROY_OBJECT(p) if((p)){delete (p); p = NULL;} 
#define DESTROY_QT_OBJECT(p) if((p)){((p))->deleteLater(); p = NULL;}
#define DESTROY_QT_LATER(p) ((p))->deleteLater();

#define RELEASE_ARRAY(a)   for (auto ptr : (a)){delete ptr;} (a).clear();

#define ABS_VAL(x) ((x) > 0 ? (x) : -(x))

#define SESSION_FORMAT_VERSION      3
#define HEADER_FORMAT_VERSION       3

namespace DecoderDataFormat
{
    enum _data_format
    {
        hex=0,
        dec=1,       
        oct=2,
        bin=3,
        ascii=4
    };

    int Parse(const char *name);
}

// --- USB link speed constants ---
// Mirror libusb's libusb_speed enum values so Core-layer code (deviceagent.cpp)
// can interpret sr_dev_inst_usb_speed_get() return values without including
// <libusb-1.0/libusb.h> directly (Core must not depend on libusb headers).
// View-layer files that already include libusb.h may use LIBUSB_SPEED_*
// interchangeably — the numeric values are identical.
#define PXV_USB_SPEED_UNKNOWN    0
#define PXV_USB_SPEED_LOW        1   // USB 1.1 (1.5 Mbps)
#define PXV_USB_SPEED_FULL       2   // USB 1.1 (12 Mbps)
#define PXV_USB_SPEED_HIGH       3   // USB 2.0 (480 Mbps)
#define PXV_USB_SPEED_SUPER      4   // USB 3.0 (5 Gbps)
#define PXV_USB_SPEED_SUPER_PLUS 5   // USB 3.1+ (10+ Gbps)

