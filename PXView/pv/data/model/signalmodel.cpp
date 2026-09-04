/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

#include "pv/data/model/signalmodel.h"
#include "pv/data/snapshot/snapshot.h"
#include "pv/data/idevice_config_port.h"

#include "pv/base/log.h"
#include "pv/base/pxvdef.h"

#include <cassert>

namespace pv {
namespace data {

SignalModel::SignalModel()
    : _index(0)
    , _type(SR_CHANNEL_LOGIC)
    , _enabled(false)
    , _vdiv(0.0)
    , _coupling(0)
    , _vfactor(1.0)
    , _map_default(true)
    , _trig_type(NONTRIG)
    , _trig_value(0.0)
    , _vertical_offset(0.0)
    , _zero_offset(0.0)
    , _hw_offset(0.0)
    , _glitch_filter_enabled(false)
    , _glitch_filter_width(0)
    , _signal_invert_enabled(false)
    , _snapshot(nullptr)
{
}

SignalModel::~SignalModel()
{
}

void SignalModel::set_index(int index) { _index = index; }

void SignalModel::set_name(const std::string &name) {
    if (_name != name) {
        _name = name;
        // Write back to the underlying sr_channel struct so libsigrok and
        // any UI that reads from sr_channel directly stay in sync. The
        // sr_channel owns its name string via g_strdup; free the old one
        // and replace with a g_strdup of the new value.
        if (_sr_channel) {
            if (_sr_channel->name) {
                g_free(_sr_channel->name);
                _sr_channel->name = nullptr;
            }
            _sr_channel->name = g_strdup(name.c_str());
        }
        emit appearance_changed();
    }
}

void SignalModel::set_type(int type) {
    assert(type == SR_CHANNEL_LOGIC || type == SR_CHANNEL_DSO ||
           type == SR_CHANNEL_ANALOG || type == SR_CHANNEL_DECODER ||
           type == SR_CHANNEL_FFT || type == SR_CHANNEL_LISSAJOUS ||
           type == SR_CHANNEL_MATH || type == SR_CHANNEL_GROUP);
    _type = type;
    // Mirror to sr_channel.type — set_name()/set_enabled() already do this for
    // their fields, but type was missed, so a model-side type change never
    // reached libsigrok. libsigrok dispatches on ch->type in several places:
    //   - session_driver.c:137/155/173/286/380/842/872/901 — demo driver routes
    //     LOGIC / ANALOG / DSO feeds and builds the enabled-channel set by type
    //   - soft-trigger.c:37 — trigger matching filters channels by type
    //   - device.c:217 — channel comparison
    // Without the write-back the driver keeps classifying the channel with its
    // old type, so e.g. a channel switched to ANALOG still delivers logic data.
    if (_sr_channel) {
        _sr_channel->type = type;
    }
}

void SignalModel::set_enabled(bool enabled, bool notify) {
    if (_enabled != enabled) {
        _enabled = enabled;
        // Mirror to sr_channel.enabled so libsigrok reflects the new state.
        if (_sr_channel) {
            _sr_channel->enabled = enabled ? TRUE : FALSE;
        }
        if (notify)
            emit visibility_changed();
    }
}

void SignalModel::set_color(const std::string &color) {
    if (_color != color) {
        _color = color;
        emit appearance_changed();
    }
}

void SignalModel::set_vdiv(double vdiv) {
    if (_vdiv != vdiv) {
        _vdiv = vdiv;
        emit appearance_changed();
    }
}

void SignalModel::set_coupling(int coupling) {
    if (_coupling != coupling) {
        _coupling = coupling;
        // SR_CONF_PROBE_COUPLING is int32 (matches DsoSignal::commit_settings
        // and demo driver's dso_couplings[] _Static_assert). Without pushing
        // to the driver here, set_acCoupling() only updates the model field;
        // the next load_settings() reads the STALE driver value and resets
        // the UI back to the old coupling (e.g. GND→DC after start_capture).
        if (_sr_channel && _device_port) {
            IDeviceConfigPort *device = _device_port;
            if (device && device->have_instance()) {
                device->set_config_int32(SR_CONF_PROBE_COUPLING, coupling,
                                         _sr_channel, nullptr);
            }
        }
        emit appearance_changed();
    }
}

void SignalModel::set_vfactor(double vfactor) {
    if (_vfactor != vfactor) {
        _vfactor = vfactor;
        // SR_CONF_PROBE_FACTOR is uint64 (matches DsoSignal::commit_settings).
        if (_sr_channel && _device_port) {
            IDeviceConfigPort *device = _device_port;
            if (device && device->have_instance()) {
                device->set_config_uint64(SR_CONF_PROBE_FACTOR,
                                          (uint64_t)vfactor, _sr_channel, nullptr);
            }
        }
        emit appearance_changed();
    }
}

void SignalModel::set_map_default(bool map_default) {
    if (_map_default != map_default) {
        _map_default = map_default;
        if (_sr_channel && _device_port) {
            IDeviceConfigPort *device = _device_port;
            if (device && device->have_instance()) {
                device->set_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default,
                                        _sr_channel, nullptr);
            }
        }
        emit appearance_changed();
    }
}

// ---- Probe configuration (explicit sr_channel override) ----
// Pattern: follow set_vdiv — use the explicit |probe| if non-nullptr, else
// fall back to the model's _sr_channel. In headless mode (no sr_channel and
// no session), only update the model field without touching libsigrok.

void SignalModel::set_probe_enabled(bool enabled, struct sr_channel *probe) {
    if (_enabled != enabled) {
        _enabled = enabled;
        struct sr_channel *ch = probe ? probe : _sr_channel;
        if (ch && _device_port) {
            IDeviceConfigPort *device = _device_port;
            if (device && device->have_instance()) {
                device->set_config_bool(SR_CONF_PROBE_EN, enabled, ch, nullptr);
            }
        }
        if (_sr_channel) {
            _sr_channel->enabled = enabled ? TRUE : FALSE;
        }
        emit visibility_changed();
    }
}

void SignalModel::set_probe_offset(uint16_t offset, struct sr_channel *probe) {
    struct sr_channel *ch = probe ? probe : _sr_channel;
    if (ch && _device_port) {
        IDeviceConfigPort *device = _device_port;
        if (device && device->have_instance()) {
            device->set_config_uint16(SR_CONF_PROBE_OFFSET, (int)offset,
                                      ch, nullptr);
        }
    }
    // Fork libsigrok's sr_channel had a `zero_offset` field; upstream
    // libsigrok does not. The hardware sync above (via set_config_uint16)
    // is sufficient — model state is tracked in _zero_offset via
    // set_zero_offset().
}

void SignalModel::set_probe_factor(uint64_t factor, struct sr_channel *probe) {
    struct sr_channel *ch = probe ? probe : _sr_channel;
    if (ch && _device_port) {
        IDeviceConfigPort *device = _device_port;
        if (device && device->have_instance()) {
            device->set_config_uint64(SR_CONF_PROBE_FACTOR, factor,
                                      ch, nullptr);
        }
    }
    // Fork libsigrok's sr_channel had a `vfactor` field; upstream
    // libsigrok does not. Hardware sync above is sufficient — model state
    // is tracked in _vfactor via set_vfactor().
    emit appearance_changed();
}

void SignalModel::set_trig_type(int trig_type) {
    if (_trig_type != trig_type) {
        _trig_type = trig_type;
        emit trig_type_changed(_trig_type);
    }
}

void SignalModel::set_trig_value(double v) {
    if (_trig_value == v) return;
    _trig_value = v;
    // Note: existing behavior — set_trig_value did not emit appearance_changed
    // and DsoSignal::set_trig_vrate does its own view refresh, so we keep the
    // same no-emit contract here.
}

void SignalModel::set_trigger_value(double value, struct sr_channel *probe) {
    (void)probe;
    // Update the model field regardless of probe override.
    _trig_value = value;
    /* Send the trigger level to the driver so that trigger detection
     * (e.g. demo_send_dso_packet edge crossing) uses the correct threshold.
     * Without this, the driver keeps the default trigger value and the
     * waveform does not respond to cursor movement. */
    struct sr_channel *ch = probe ? probe : _sr_channel;
    if (ch && _device_port) {
        IDeviceConfigPort *device = _device_port;
        if (device && device->have_instance()) {
            device->set_config_int32(SR_CONF_TRIGGER_VALUE, (int)value, ch, nullptr);
        }
    }
}

bool SignalModel::commit_trig()
{
    // Fork libsigrok's ds_trigger_probe_set/ds_trigger_set_en are gone.
    // The trigger state is stored in _trig_type and synced to upstream
    // libsigrok via SessionStateContext::sync_trigger_to_libsigrok() at
    // capture start, which reads trig_type() and builds an sr_trigger.
    // This method is retained for backward compat with TriggerDock callers.
    return _trig_type != NONTRIG;
}

void SignalModel::set_vertical_offset(double offset) {
    if (_vertical_offset == offset) return;
    _vertical_offset = offset;
    // Fork libsigrok's sr_channel had an `offset` field (uint16, vertical
    // position). Upstream libsigrok does not expose this — model state is
    // tracked in _vertical_offset. Driver-side vertical position is set via
    // SR_CONF_PROBE_OFFSET (see set_zero_offset).
}

void SignalModel::set_zero_offset(double offset) {
    if (_zero_offset == offset) return;
    _zero_offset = offset;
    // SR_CONF_PROBE_OFFSET is uint16 (matches DsoSignal::commit_settings).
    if (_sr_channel && _device_port) {
        IDeviceConfigPort *device = _device_port;
        if (device && device->have_instance()) {
            device->set_config_uint16(SR_CONF_PROBE_OFFSET, (int)offset,
                                      _sr_channel, nullptr);
        }
    }
}

void SignalModel::set_hw_offset(double offset) {
    if (_hw_offset == offset) return;
    _hw_offset = offset;
    // SR_CONF_PROBE_HW_OFFSET is uint16 (matches DsoSignal::get_hw_offset
    // which calls get_config_uint16).
    if (_sr_channel && _device_port) {
        IDeviceConfigPort *device = _device_port;
        if (device && device->have_instance()) {
            device->set_config_uint16(SR_CONF_PROBE_HW_OFFSET, (int)offset,
                                      _sr_channel, nullptr);
        }
    }
}

void SignalModel::set_glitch_filter_enabled(bool enabled) { _glitch_filter_enabled = enabled; }
void SignalModel::set_glitch_filter_width(int width) { _glitch_filter_width = width; }
void SignalModel::set_signal_invert_enabled(bool enabled) { _signal_invert_enabled = enabled; }
void SignalModel::set_snapshot(std::shared_ptr<Snapshot> snapshot) { _snapshot = snapshot; }

void SignalModel::commit_to_device()
{
    // Headless-mode no-op: without a sr_channel there is nothing to sync to.
    if (_sr_channel == nullptr) return;

    // ---- Direct sr_channel struct fields (upstream-compatible only) ----
    // name: free old g_strdup'd string and replace.
    if (_sr_channel->name) {
        g_free(_sr_channel->name);
        _sr_channel->name = nullptr;
    }
    _sr_channel->name = g_strdup(_name.c_str());

    _sr_channel->enabled = _enabled ? TRUE : FALSE;

    // type must be mirrored too: libsigrok dispatches on ch->type when routing
    // the data feed (session_driver.c) and matching triggers (soft-trigger.c),
    // so a model-side type change that never reaches the struct leaves the
    // driver classifying the channel with its old type.
    _sr_channel->type = _type;

    // Fork libsigrok's sr_channel had extra fields (offset, zero_offset,
    // hw_offset, vdiv, vfactor, coupling, trig_value) that upstream
    // libsigrok does not expose. Those are synced to the driver via the
    // DeviceAgent set_config_* calls below — model state lives in the
    // _vertical_offset / _zero_offset / _hw_offset / _vdiv / _vfactor /
    // _coupling / _trig_value fields on this object.

    // ---- Hardware-relevant fields via DeviceAgent set_config_* ----
    // Same keys/types as DsoSignal::commit_settings() so the driver receives
    // the same updates it would get from the View layer.
    if (_device_port == nullptr) return;
    IDeviceConfigPort *device = _device_port;
    if (device == nullptr || !device->have_instance()) return;

    device->set_config_bool(SR_CONF_PROBE_EN, _enabled, _sr_channel, nullptr);
    device->set_config_uint64(SR_CONF_PROBE_FACTOR, (uint64_t)_vfactor,
                              _sr_channel, nullptr);
    device->set_config_uint16(SR_CONF_PROBE_OFFSET, (int)_zero_offset,
                              _sr_channel, nullptr);
    device->set_config_uint16(SR_CONF_PROBE_HW_OFFSET, (int)_hw_offset,
                              _sr_channel, nullptr);
    device->set_config_bool(SR_CONF_PROBE_MAP_DEFAULT, _map_default,
                            _sr_channel, nullptr);
}

} // namespace data
} // namespace pv
