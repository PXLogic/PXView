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

#include "pv/view/component/dso_trigger_config.h"

#include "pv/data/datasource.h"
#include "pv/data/model/signalmodel.h"
#include "pv/session/deviceagent.h"
#include "pv/base/log.h"
#include "pv/view/signal/dsosignal.h"

using namespace std;

namespace pv {
namespace view {

DsoTriggerConfig::DsoTriggerConfig(DsoSignal *signal) : _signal(signal) {}

DsoTriggerConfig::~DsoTriggerConfig() {}

double DsoTriggerConfig::get_trig_vrate() {
  if (_signal->_data_source->device()->is_hardware_logic()) {
    /* Compute trigger cursor position as zero_ratio + delta_ratio.
     * delta_ratio = (_trig_value - mid_value) / (ref_max - ref_min).
     * This can be negative (cursor above zero line) — do NOT use
     * value2ratio() which clamps to [0,1] and prevents the cursor
     * from going above 0V (positive voltage area). */
    const double mid = _signal->ratio2value(0.5);
    const double range = (double)_signal->_ref_max - (double)_signal->_ref_min;
    if (range <= 0)
      return _signal->get_zero_ratio();
    const double delta_ratio = ((double)_signal->_trig_value - mid) / range;
    return delta_ratio + _signal->get_zero_ratio();
  } else {
    return _signal->value2ratio(_signal->_trig_value);
  }
}

void DsoTriggerConfig::set_trig_vpos(int pos, bool delta_change) {
  assert(_signal->_view);
  if (_signal->enabled()) {
    set_trig_ratio(_signal->pos2ratio(pos), delta_change);
  }
}

void DsoTriggerConfig::set_trig_ratio(double ratio, bool delta_change) {
  // Same nested-broadcast guard as set_zero_ratio: set_config_byte triggers
  // synchronous config_changed -> broadcast_async<>, which may delete this DsoSignal.
  auto model = _signal->_model;
  double delta = ratio;

  if (_signal->_data_source->device()->is_hardware_logic()) {
    delta = delta - _signal->get_zero_ratio();
    delta = min(delta, 0.5);
    delta = max(delta, -0.5);
    _signal->_trig_value = _signal->ratio2value(delta + 0.5);
  } else {
    if (delta < 0.06f)
      delta = 0.06f;
    if (delta > 0.945f)
      delta = 0.945f;

    _signal->_trig_value = _signal->ratio2value(delta);
  }

  if (delta_change)
    _signal->_trig_delta = _signal->get_trig_vrate() - _signal->get_zero_ratio();
  // Task 7.2: 写回 Core SignalModel。不广播：本方法亦被 mainwindow JSON
  // 恢复路径 (mainwindow.cpp restore_session) 调用，广播会触发 rebuild 循环。
  if (model) {
    model->set_trig_value((double)_signal->_trig_value);
  }
  /* Send the trigger level to the driver immediately so that real-time
   * trigger detection (e.g. demo_send_dso_packet) uses the updated
   * threshold. Without this, the driver keeps the old value until
   * commit_settings() is called, causing the waveform to not respond
   * to cursor movement during capture. */
  {
    sr_channel *probe = model ? model->sr_channel_handle() : nullptr;
    if (probe) {
      DeviceAgent *device = _signal->_data_source->device();
      if (device && device->have_instance()) {
        device->set_config_int32(SR_CONF_TRIGGER_VALUE,
                                 (int)_signal->_trig_value, probe, nullptr);
      }
    }
  }
}

} // namespace view
} // namespace pv
