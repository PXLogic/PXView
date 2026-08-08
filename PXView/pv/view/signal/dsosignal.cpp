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

#include "pv/view/signal/dsosignal.h"
#include "pv/view/component/dso_trigger_config.h"
#include "pv/view/component/dso_measure.h"
#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include <functional>
#include <cmath>

#include "pv/mainwindow/appcontrol.h"
#include "pv/config/appconfig.h"
#include "pv/data/datasource.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/model/signalmodel.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/ui/langresource.h"
#include "pv/view/view.h"
#include "pv/view/viewport/viewport.h"

using namespace std;

namespace pv {
namespace view {

const QString DsoSignal::vDialUnit[DsoSignal::vDialUnitCount] = {
    "mV",
    "V",
};

const QColor DsoSignal::SignalColours[4] = {
    QColor(238, 178, 17, 255), // dsYellow
    QColor(0, 153, 37, 255),   // dsGreen
    QColor(213, 15, 37, 255),  // dsRed
    QColor(17, 133, 209, 255)  // dsBlue

};

static const char *DsoSignalColorTokens[4] = {"@signal-orange", "@signal-green",
                                              "@signal-red", "@signal-blue"};

QColor DsoSignal::getSignalColor(int index) {
  QColor c =
      AppConfig::Instance().GetThemeColor(DsoSignalColorTokens[index % 4]);
  return c.isValid() ? c : SignalColours[index % 4];
}

// LDO dual-path threshold:
//   spp < threshold → paint_trace (per-sample polyline, continuous)
//   spp >= threshold → paint_per_pixel (per-pixel min/max, 1px rects)
// Set to 4.0: paint_trace uses drawPolyline which is inherently continuous
// (connects points), while paint_per_pixel uses drawRects (independent
// rectangles, no connection). At spp ≈ 1, paint_per_pixel draws 1px rects
// with no connection → appears as scattered dots / broken lines.
// At spp < 4, paint_trace draws < 4000 points (DSO frame = 20K samples,
// visible = width * spp < 4000), which is < 2ms — smooth and continuous.
// At spp >= 4, paint_per_pixel draws 1000 dense rects — no visual gaps.
// Always use paint_per_pixel (drawRects) instead of paint_trace (drawPolyline).
// drawPolyline with alpha=200 on a transparent QPixmap is extremely slow on
// Windows raster engine: 502 points takes ~44ms per DSO channel, vs ~2ms
// for paint_per_pixel with drawRects. The visual difference (stepped vs
// smooth) is negligible for DSO waveforms. Setting threshold to 0 ensures
// paint_per_pixel is always used regardless of zoom level.
const float DsoSignal::EnvelopeThreshold = 0.0f;

DsoSignal::DsoSignal(data::DsoSnapshot *data,
                     std::shared_ptr<data::SignalModel> model,
                     data::DataSource *data_source)
    : Signal(model, data_source), _data(data),
      _cached_hw_offset(model ? model->hw_offset() : 128),
      _hover_point(QPointF(-1, -1)) {
  _vDial = nullptr;
  _period = 0;
  _pcount = 0;
  _scale = 0;
  _en_lock = false;
  _show = true;
  _vDialActive = false;
  _mValid = false;
  _level_valid = false;
  _autoV = false;
  _autoH = false;
  _autoV_over = false;
  _auto_cnt = 0;
  _hover_en = false;
  _hover_index = 0;
  _hover_value = 0;

  _trig_config = std::make_unique<DsoTriggerConfig>(this);
  _measure = std::make_unique<DsoMeasure>(this);

  init_vDial();
  _colour = getSignalColor(model ? model->index() : 0);
  load_settings();
}

DsoSignal::DsoSignal(DsoSignal *s, pv::data::DsoSnapshot *data,
                     std::shared_ptr<data::SignalModel> model,
                     data::DataSource *data_source)
    : Signal(*s, model, data_source), _data(data), _scale(s->_scale),
      _stop_scale(s->_stop_scale), _en_lock(false), _show(s->_show),
      _vDialActive(s->_vDialActive), _acCoupling(s->_acCoupling),
      _bits(s->_bits), _ref_min(s->_ref_min), _ref_max(s->_ref_max),
      _trig_value(s->_trig_value), _trig_delta(s->_trig_delta),
      _zero_offset(s->_zero_offset), _cached_hw_offset(s->_cached_hw_offset),
      _mValid(false), _max(0), _min(0), _period(0), _level_valid(false),
      _high(0), _low(0), _rms(0), _mean(0), _rise_time(0), _fall_time(0),
      _high_time(0), _burst_time(0), _pcount(0), _autoV(false), _autoH(false),
      _autoV_over(false), _auto_cnt(0), _hover_en(false), _hover_index(0),
      _hover_point(QPointF(-1, -1)), _hover_value(0) {
  _vDial = nullptr;

  _trig_config = std::make_unique<DsoTriggerConfig>(this);
  _measure = std::make_unique<DsoMeasure>(this);

  init_vDial(s);
}

DsoSignal *DsoSignal::clone() const {
  DsoSignal *cloned =
      new DsoSignal(const_cast<DsoSignal *>(this), nullptr, _model, _data_source);
  cloned->_local_enabled = _local_enabled;
  cloned->_visible = _visible;
  return cloned;
}

DsoSignal::~DsoSignal() {
// Delegate unique_ptrs auto-destroy here (complete types visible in this TU).
// Spec v2 Task 6: _vDial now managed by unique_ptr, no manual delete.
}

void DsoSignal::set_scale(int height) {
  _scale = height / (_ref_max - _ref_min) * _stop_scale;
}

//============================== Phase G facades ==============================
// The following methods forward to the delegate classes. Public API is
// preserved; only the implementation moved. DsoHardwareConfig was removed
// (DSO mode deprecated); its DSO-key backed methods are now no-op stubs.

// -- DsoHardwareConfig (inlined; DSO-key backed parts stubbed) --
void DsoSignal::set_enable(bool enable) {
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  if (!probe)
    return;

  if (_data_source->device()->is_hardware_logic() && get_index() == 0) {
    return;
  }

  _en_lock = true;
  bool cur_enable = _model->enabled();
  if (cur_enable == enable) {
    // 即使 model 状态没变,也要同步 _local_enabled,因为 enabled()
    // (基类 Signal::enabled()) 返回的是 _local_enabled,不是 _model->enabled()。
    _local_enabled = enable;
    _en_lock = false;
    return;
  }
  // 同步 _local_enabled — set_enable 不经过 Signal::set_enabled(),
  // 而是直接调 _model->set_probe_enabled(),所以 _local_enabled 不会自动同步。
  _local_enabled = enable;

  // DSO 模式: 不要 stop/start capture! 旧代码在 set_enable 内部做
  // stop_capture + start_capture,这会触发 reload() -> rebuild_signals()
  // -> apply_model_properties() 用新 model 覆盖 _local_enabled。但重建是
  // 异步的,重建期间旧 signal 的 _local_enabled 可能被重置为旧值,导致
  // "禁用后无法使能"。demo 驱动是软件生成的,enable 状态在下一帧自然
  // 生效 (demo_send_dso_packet 只为 enabled 通道生成数据),无需重启采集。
  // 同时调用 Signal::set_enabled() 确保基类 _local_enabled 与 model 同步。
  Signal::set_enabled(enable);
  _model->set_probe_enabled(enable, probe);

  if (_view)
    _view->update_hori_res();

  if (_view) {
    _view->set_update(_viewport, true);
    _view->update();
  }
  _en_lock = false;
}

void DsoSignal::set_vDialActive(bool active) {
  if (enabled())
    _vDialActive = active;
}

bool DsoSignal::go_vDialPre(bool manul) {
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;

  if (_autoV && manul)
    autoV_end();

  if (enabled() && !_vDial->isMin()) {
    if (_data_source->is_running_status())
      _data_source->refresh(DsoSignal::RefreshShort);

    const double pre_vdiv = _vDial->get_value();
    _vDial->set_sel(_vDial->get_sel() - 1);

    // Sync new vdiv to driver so that rebuild_signals() -> load_settings()
    // -> get_probe_vdiv() reads the updated value. Without this, the async
    // DeviceOptionsUpdated broadcast (emitted by mouse_press after this call
    // returns) triggers rebuild_signals() which recreates DsoSignal and calls
    // init_vDial()/load_settings() — reading the STALE driver vdiv and
    // resetting _sel back to the old index.
    DeviceAgent *dev = _data_source ? _data_source->device() : nullptr;
    if (dev && probe)
      dev->set_config_uint64(SR_CONF_PROBE_VDIV, _vDial->get_value(), probe);

    if (_data_source->is_stopped_status()) {
      set_stop_scale(_stop_scale * (pre_vdiv / _vDial->get_value()));
      set_scale(get_view_rect().height());
    }
    if (probe)
      _model->set_probe_offset((uint16_t)_zero_offset, probe);

    _view->vDial_updated();
    _view->set_update(_viewport, true);
    _view->update();
    if (_model) {
      _model->set_vdiv((double)_vDial->get_value());
    }
    return true;
  } else {
    if (_autoV && !_autoV_over)
      autoV_end();
    return false;
  }
}

bool DsoSignal::go_vDialNext(bool manul) {
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;

  if (_autoV && manul)
    autoV_end();

  if (enabled() && !_vDial->isMax()) {
    if (_data_source->is_running_status())
      _data_source->refresh(DsoSignal::RefreshShort);

    const double pre_vdiv = _vDial->get_value();
    _vDial->set_sel(_vDial->get_sel() + 1);

    // Sync new vdiv to driver so that rebuild_signals() -> load_settings()
    // -> get_probe_vdiv() reads the updated value. Without this, the async
    // DeviceOptionsUpdated broadcast (emitted by mouse_press after this call
    // returns) triggers rebuild_signals() which recreates DsoSignal and calls
    // init_vDial()/load_settings() — reading the STALE driver vdiv and
    // resetting _sel back to the old index.
    DeviceAgent *dev = _data_source ? _data_source->device() : nullptr;
    if (dev && probe)
      dev->set_config_uint64(SR_CONF_PROBE_VDIV, _vDial->get_value(), probe);

    if (_data_source->is_stopped_status()) {
      set_stop_scale(_stop_scale * (pre_vdiv / _vDial->get_value()));
      set_scale(get_view_rect().height());
    }
    if (probe)
      _model->set_probe_offset((uint16_t)_zero_offset, probe);

    _view->vDial_updated();
    _view->set_update(_viewport, true);
    _view->update();
    if (_model) {
      _model->set_vdiv((double)_vDial->get_value());
    }
    return true;
  } else {
    if (_autoV && !_autoV_over)
      autoV_end();
    return false;
  }
}

void DsoSignal::init_vDial(DsoSignal *src) {
  QVector<QString> vUnit;

  for (uint64_t i = 0; i < DsoSignal::vDialUnitCount; i++) {
    vUnit.append(DsoSignal::vDialUnit[i]);
  }

  _vDial = nullptr;

  QVector<uint64_t> vValue = _data_source->device()->get_probe_vdiv_list();
  if (vValue.isEmpty()) {
    // Device does not expose SR_CONF_PROBE_VDIV. Use a standard 8-step
    // vdiv range (10mV..2V, same as demo driver's dso_vdivs[]) so the dial
    // is rotatable instead of being stuck on a single entry.
    static const uint64_t default_vdivs[] = {
        10, 20, 50, 100, 200, 500, 1000, 2000
    };
    for (uint64_t v : default_vdivs)
      vValue.push_back(v);
  }

_vDial.reset(new dslDial(vValue.count(), DsoSignal::vDialValueStep, vValue, vUnit, false));

  if (src) {
    _vDial->set_sel(src->_vDial->get_sel());
    _vDial->set_factor(src->_vDial->get_factor());
  }
}

bool DsoSignal::load_settings() {
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  int v;
  uint32_t ui32;
  bool ret;

  // dso channel bits
  ret = _data_source->device()->get_unit_bits(v);
  if (ret) {
    _bits = (uint8_t)v;
  } else {
    _bits = DsoSignal::DefaultBits;
    pxv_warn("config_get SR_CONF_UNIT_BITS failed, set to %d (default)",
             DsoSignal::DefaultBits);

    if (_data_source->device()->is_hardware())
      return false;
  }

  ret = _data_source->device()->get_ref_min(ui32);
  if (ret)
    _ref_min = (double)ui32;
  else
    _ref_min = 1;

  ret = _data_source->device()->get_ref_max(ui32);
  if (ret)
    _ref_max = (double)ui32;
  else
    _ref_max = ((1 << _bits) - 1);

  // -- vdiv (DSO-key backed; fall back to model value)
  uint64_t vdiv;
  uint64_t vfactor;
  if (probe) {
    ret = _data_source->device()->get_probe_vdiv(vdiv, probe);
    if (!ret) {
      // SR_CONF_PROBE_VDIV fork stub deleted; fall back to model.
      vdiv = _model ? (uint64_t)_model->vdiv() : 0;
    }

    ret = _data_source->device()->get_probe_factor(vfactor, probe);
    if (!ret) {
      pxv_err("ERROR: config_get SR_CONF_PROBE_FACTOR failed.");
      return false;
    }
  } else {
    vdiv = _model ? _model->vdiv() : 0;
    vfactor = _model ? _model->vfactor() : 1;
  }

  _vDial->set_value(vdiv);
  _vDial->set_factor(vfactor);

  // -- coupling (DSO-key backed; fall back to model value)
  if (probe) {
    ret = _data_source->device()->get_probe_coupling(v, probe);
    if (ret) {
      _acCoupling = uint8_t(v);
    } else {
      // SR_CONF_PROBE_COUPLING fork stub deleted; fall back to model.
      _acCoupling = _model ? (uint8_t)_model->coupling() : 0;
    }
  } else {
    _acCoupling = _model ? (uint8_t)_model->coupling() : 0;
  }

  // -- enable state (sync from driver so CH1 is enabled by default)
  if (probe) {
    bool ch_enabled = false;
    ret = _data_source->device()->get_config_bool(SR_CONF_PROBE_EN, ch_enabled, probe, nullptr);
    if (ret) {
      set_enable(ch_enabled);
    } else {
      // Driver GET failed (e.g. stub) — assume enabled to avoid CH1 waveform disappearing
      set_enable(true);
    }
  }

  // -- vpos (zero offset)
  // Pre-initialize to mid-range so that if get_probe_offset fails, we keep
  // a sensible default (0V at screen center) instead of 0 (top of screen).
  // For 8-bit ADC, mid = 128.
  const int mid_range = (1 << _bits) / 2;
  _zero_offset = mid_range;
  if (probe) {
    ret = _data_source->device()->get_probe_offset(_zero_offset, probe);
    if (!ret) {
      // Driver GET failed — keep the mid-range default. Do NOT fall back to
      // _model->vertical_offset() because the model defaults to 0.0 (unset),
      // which would place 0V at the top of the screen.
      pxv_warn("config_get SR_CONF_PROBE_OFFSET failed, using default %d", _zero_offset);
    }
  }

  // -- hw_offset (hardware DC offset)
  // Query the driver here so _cached_hw_offset is correct even when the
  // device is stopped (get_hw_offset() only refreshes during running status).
  // Without this, _cached_hw_offset stays at the model default (0.0), causing
  // the waveform to shift down by mid_range * _scale pixels from the zero line.
  _cached_hw_offset = mid_range;
  if (probe) {
    int hw_off = mid_range;
    if (_data_source->device()->get_probe_hw_offset(hw_off, probe)) {
      _cached_hw_offset = hw_off;
    } else {
      pxv_warn("config_get SR_CONF_PROBE_HW_OFFSET failed, using default %d", _cached_hw_offset);
    }
  }

  // -- trig_value (trigger level)
  // Pre-initialize to mid-range so the T cursor appears at screen center
  // instead of the top when the driver call fails.
  _trig_value = mid_range;
  if (probe) {
    ret = _data_source->device()->get_trigger_value(_trig_value, probe);
    if (ret) {
      _trig_delta = get_trig_vrate() - get_zero_ratio();
    } else {
      // Driver GET failed — keep the mid-range default. Do NOT fall back to
      // _model->trig_value() because the model defaults to 0.0 (unset),
      // which would place the T cursor at the top of the screen.
      _trig_delta = get_trig_vrate() - get_zero_ratio();
    }
  } else {
    _trig_delta = get_trig_vrate() - get_zero_ratio();
  }

  if (_view) {
    _view->set_update(_viewport, true);
    _view->update();
  }
  return true;
}

int DsoSignal::commit_settings() {
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  if (!probe)
    return 0;

  // -- enable
  _model->set_probe_enabled(enabled(), probe);

  // -- vdiv
  _model->set_vdiv((double)_vDial->get_value());
  _model->set_probe_factor(_vDial->get_factor(), probe);

  // -- coupling
  _model->set_coupling((int)_acCoupling);

  // -- offset
  _model->set_probe_offset((uint16_t)_zero_offset, probe);

  // -- trig_value
  _model->set_trigger_value((double)_trig_value, probe);

  return 1;
}

uint64_t DsoSignal::get_vDialValue() { return _vDial->get_value(); }

uint16_t DsoSignal::get_vDialSel() { return _vDial->get_sel(); }

void DsoSignal::set_acCoupling(uint8_t coupling) {
  pxv_info("[DSO-COUPLING] set_acCoupling(%u) called, enabled=%d",
           (unsigned)coupling, enabled());
  auto model = _model;

  if (enabled()) {
    _acCoupling = coupling;
    if (model) {
      model->set_coupling((int)coupling);
    }
  }
}

int DsoSignal::ratio2value(double ratio) {
  return ratio * (_ref_max - _ref_min) + _ref_min;
}

int DsoSignal::ratio2pos(double ratio) {
  return ratio * get_view_rect().height() + get_view_rect().top();
}

double DsoSignal::value2ratio(int value) {
  // Clamp to [0.0, 1.0]. Previously only `max(0.0, ...)` clipped the lower
  // bound — when the driver-reported _trig_value exceeded _ref_max (driver
  // bug, profile mismatch, stale restored-session value), this returned >1.0
  // and paint_fore() rendered the T pointer below the viewport (off-screen).
  return min(1.0, max(0.0, (value - _ref_min) / (_ref_max - _ref_min)));
}

double DsoSignal::pos2ratio(int pos) {
  return min(max(pos - get_view_rect().top(), 0), get_view_rect().height()) *
         1.0 / get_view_rect().height();
}

int DsoSignal::get_zero_vpos() { return ratio2pos(get_zero_ratio()); }

double DsoSignal::get_zero_ratio() { return value2ratio(_zero_offset); }

int DsoSignal::get_hw_offset() {
  // In running mode, hw_offset doesn't change between frames (it only
  // changes when the user adjusts probe offset). Refresh from driver
  // at most once per second to avoid sr_config_get + log I/O on every
  // paint cycle (was the #2 cause of DSO medium-zoom lag after
  // paint_trace slowness).
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  if (_data_source->is_running_status()) {
    if (probe && _hw_offset_timer.elapsed() >= 1000) {
      int hw_offset = _cached_hw_offset;
      if (_data_source->device()->get_probe_hw_offset(hw_offset, probe)) {
        _cached_hw_offset = hw_offset;
      }
      _hw_offset_timer.restart();
    }
  }
  return _cached_hw_offset;
}

void DsoSignal::set_zero_vpos(int pos) {
  if (enabled()) {
    set_zero_ratio(pos2ratio(pos));
    set_trig_ratio(_trig_delta + get_zero_ratio(), false);
  }
}

void DsoSignal::set_zero_ratio(double ratio) {
  // CRITICAL: Copy _model to a local shared_ptr BEFORE calling set_config_*.
  // set_config_uint16 -> config_changed -> broadcast_async<SampleCountUpdated>
  // is SYNCHRONOUS and can trigger nested reload -> signals_changed -> View
  // AllReplaced rebuild, which DELETES this DsoSignal (and its _model member).
  // After set_config returns, _model may be dangling. The local copy keeps the
  // SignalModel alive even if `this` is deleted mid-method.
  auto model = _model;
  _zero_offset = ratio2value(ratio);
  if (model) {
    model->set_zero_offset((double)_zero_offset);
  }
}

void DsoSignal::set_factor(uint64_t factor) {
  pxv_info("[DSO-FACTOR] set_factor(%llu) called, enabled=%d",
           (unsigned long long)factor, enabled());
  auto model = _model;
  sr_channel *probe = model ? model->sr_channel_handle() : nullptr;

  if (enabled()) {
    uint64_t prefactor = 0;
    bool ret;

    if (probe) {
      ret = _data_source->device()->get_probe_factor(prefactor, probe);
      if (!ret) {
        pxv_err("ERROR: config_get SR_CONF_PROBE_FACTOR failed.");
        return;
      }
    } else {
      prefactor = model ? model->vfactor() : 1;
    }

    if (prefactor != factor) {
      _vDial->set_factor(factor);
      _view->set_update(_viewport, true);
      _view->update();
      if (model) {
        model->set_vfactor((double)factor);
      }

      // 联动 vDial: 把有效 vdiv (base_value × factor) 推送到 driver。
      // dslDial::get_value() 返回基础档位值 (不含 factor), 而 driver 的
      // SR_CONF_PROBE_VDIV 用于 demo_dso_vdiv_scale() 缩放波形振幅。
      // 如果不把 factor 乘进去推送, 切换 x10 后波形振幅不变 (因为
      // dso_vdiv 仍是基础值, demo_dso_vdiv_scale 按基础值缩放)。
      // x10 探头衰减信号 10 倍, 等效于 vdiv 放大 10 倍, 所以推送
      // base × factor 让 demo_dso_vdiv_scale 按放大后的 vdiv 缩放,
      // 波形振幅相应缩小。
      DeviceAgent *dev = _data_source ? _data_source->device() : nullptr;
      if (dev && probe) {
        uint64_t effective_vdiv = _vDial->get_value() * factor;
        dev->set_config_uint64(SR_CONF_PROBE_VDIV, effective_vdiv, probe);
        pxv_info("[DSO-FACTOR] pushed effective_vdiv=%llu (base=%llu × factor=%llu)",
                 (unsigned long long)effective_vdiv,
                 (unsigned long long)_vDial->get_value(),
                 (unsigned long long)factor);
      }

      // 同步 stop_scale (与 go_vDialPre/Next 一致)
      if (_data_source->is_stopped_status()) {
        // factor 变化等效于 vdiv 变化, 调整 stop_scale 保持显示比例
        // 新_vdiv / 旧_vdiv = factor / prefactor
        set_stop_scale(_stop_scale * ((double)prefactor / (double)factor));
        set_scale(get_view_rect().height());
      }

      _view->vDial_updated();
    }
  }
}

uint64_t DsoSignal::get_factor() {
  // PERFORMANCE FIX: Use cached _vDial factor instead of querying the driver
  // on every paint cycle. load_settings() already syncs the driver value into
  // _vDial via get_probe_factor(), so reading the cached value is equivalent
  // but avoids 974+ sr_config_get calls per session (the main zoom lag cause).
  if (_vDial) {
    uint64_t f = _vDial->get_factor();
    return f > 0 ? f : 1;
  }
  return _model ? _model->vfactor() : 1;
}

// -- DsoTriggerConfig --
double DsoSignal::get_trig_vrate() { return _trig_config->get_trig_vrate(); }
void DsoSignal::set_trig_vpos(int pos, bool delta_change) { _trig_config->set_trig_vpos(pos, delta_change); }
void DsoSignal::set_trig_ratio(double ratio, bool delta_change) { _trig_config->set_trig_ratio(ratio, delta_change); }

// -- DsoMeasure --
QString DsoSignal::get_measure(enum DSO_MEASURE_TYPE type) { return _measure->get_measure(type); }
bool DsoSignal::measure(const QPointF &p) { return _measure->measure(p); }
bool DsoSignal::get_hover(uint64_t &index, QPointF &p, double &value) { return _measure->get_hover(index, p, value); }
QPointF DsoSignal::get_point(uint64_t index, float &value) { return _measure->get_point(index, value); }
double DsoSignal::get_voltage(uint64_t index) { return _measure->get_voltage(index); }
QString DsoSignal::get_voltage(double v, int p, bool scaled) { return _measure->get_voltage(v, p, scaled); }
QString DsoSignal::get_time(double t) { return _measure->get_time(t); }
void DsoSignal::auto_set() { _measure->auto_set(); }
void DsoSignal::autoV_end() { _measure->autoV_end(); }
void DsoSignal::autoH_end() { _measure->autoH_end(); }
void DsoSignal::auto_end() { _measure->auto_end(); }
void DsoSignal::auto_start() { _measure->auto_start(); }

//============================== Paint methods ==============================
// Phase 5: Paint methods (get_view_rect, paint_prepare, paint_back,
// paint_mid, paint_fore, get_trig_rect, paint_trace, paint_envelope,
// paint_per_pixel, paint_type_options, paint_hover_measure) have been
// extracted to dsosignal_paint.cpp for better file modularity.
//============================== Interaction ==============================

bool DsoSignal::mouse_press(int right, const QPoint pt) {
  int y = get_y();
  const QRectF vDial_rect = get_rect(DSO_VDIAL, y, right);
  const QRectF chEn_rect = get_rect(DSO_CHEN, y, right);
  const QRectF acdc_rect = get_rect(DSO_ACDC, y, right);
  const QRectF auto_rect = get_rect(DSO_AUTO, y, right);
  const QRectF x1_rect = get_rect(DSO_X1, y, right);
  const QRectF x10_rect = get_rect(DSO_X10, y, right);
  const QRectF x100_rect = get_rect(DSO_X100, y, right);

  // 诊断日志:确认点击位置和各 rect 命中情况。
  pxv_info("[DSO-MOUSE] press pt=(%d,%d) y=%d enabled=%d local_en=%d "
           "model_en=%d chEn=%s acdc=%s x1=%s x10=%s x100=%s vDial=%s",
           pt.x(), pt.y(), y, enabled(), _local_enabled,
           _model ? _model->enabled() : -1,
           chEn_rect.contains(pt) ? "Y" : "n",
           acdc_rect.contains(pt) ? "Y" : "n",
           x1_rect.contains(pt) ? "Y" : "n",
           x10_rect.contains(pt) ? "Y" : "n",
           x100_rect.contains(pt) ? "Y" : "n",
           vDial_rect.contains(pt) ? "Y" : "n");

  if (chEn_rect.contains(pt)) {
    if (_data_source->device()->is_file() == false && !_en_lock) {
      set_enable(!enabled());
    }
    return true;
  } else if (enabled()) {
    if (vDial_rect.contains(pt) && pt.x() > vDial_rect.center().x()) {
      if (pt.y() > vDial_rect.center().y())
        go_vDialNext(true);
      else
        go_vDialPre(true);
    } else if (_data_source->device()->is_file() == false &&
               acdc_rect.contains(pt)) {
      if (_data_source->device()->is_hardware_logic())
        set_acCoupling((get_acCoupling() + 1) % 3);
      else
        set_acCoupling((get_acCoupling() + 1) % 3);
    } else if (auto_rect.contains(pt)) {
      if (_data_source->device()->is_hardware())
        auto_start();
    } else if (x1_rect.contains(pt)) {
      set_factor(1);
      _view->dso_factor_updated();
    } else if (x10_rect.contains(pt)) {
      set_factor(10);
      _view->dso_factor_updated();
    } else if (x100_rect.contains(pt)) {
      set_factor(100);
      _view->dso_factor_updated();
    } else {
      return false;
    }

    // User interaction changed device options (vDial/acdc/factor). Broadcast
    // DsoViewOptionChanged (NOT DeviceOptionsUpdated) so MCP/WS clients receive
    // a push notification AND dock panels refresh, WITHOUT triggering
    // reload()/rebuild_signals() which would drop View-only state
    // (_stop_scale resets to 1 in path-B full rebuild → waveform no longer
    // scales with vdiv). The individual setters (set_factor/set_acCoupling/
    // go_vDial*) deliberately do NOT broadcast because they are also called
    // from JSON restore paths (rebuild loop risk); mouse_press is the
    // user-interaction entry point per AGENTS.md.
    _view->session().broadcast_async<interface::DsoViewOptionChanged>(
        interface::DsoViewOptionChanged{get_index()});
    return true;
  }
  return false;
}

bool DsoSignal::mouse_wheel(int right, const QPoint pt, const int shift) {
  int y = get_y();
  const QRectF vDial_rect = get_rect(DSO_VDIAL, y, right);

  if (vDial_rect.contains(pt)) {
    if (shift > 0.5)
      go_vDialPre(true);
    else if (shift < -0.5)
      go_vDialNext(true);
    // Same rationale as mouse_press: notify docks/MCP without rebuild.
    _view->session().broadcast_async<interface::DsoViewOptionChanged>(
        interface::DsoViewOptionChanged{get_index()});
    return true;
  } else {
    return false;
  }
}

QRectF DsoSignal::get_rect(DsoSetRegions type, int y, int right) {
  (void)right;

  if (type == DSO_VDIAL)
    return QRectF(get_leftWidth() + SquareWidth * 0.5 + Margin,
                  y - SquareWidth * SquareNum + SquareWidth * 3,
                  SquareWidth * (SquareNum - 1), SquareWidth * (SquareNum - 1));
  else if (type == DSO_X1)
    return QRectF(get_leftWidth() + SquareWidth * 0.5,
                  y - SquareWidth * 2 - SquareWidth * (SquareNum - 2) * 1 +
                      SquareWidth * 3,
                  SquareWidth * 1.75, SquareWidth);
  else if (type == DSO_X10)
    return QRectF(get_leftWidth() + SquareWidth * 0.5,
                  y - SquareWidth * 2 - SquareWidth * (SquareNum - 2) * 0.5 +
                      SquareWidth * 3,
                  SquareWidth * 1.75, SquareWidth);
  else if (type == DSO_X100)
    return QRectF(get_leftWidth() + SquareWidth * 0.5,
                  y - SquareWidth * 2 - SquareWidth * (SquareNum - 2) * 0 +
                      SquareWidth * 3,
                  SquareWidth * 1.75, SquareWidth);
  else if (type == DSO_CHEN)
    return QRectF(2, y - SquareWidth / 2 + SquareWidth * 3, SquareWidth * 1.75,
                  SquareWidth);
  else if (type == DSO_ACDC)
    return QRectF(2 + SquareWidth * 1.75 + Margin,
                  y - SquareWidth / 2 + SquareWidth * 3, SquareWidth * 1.75,
                  SquareWidth);
  else if (type == DSO_AUTO)
    return QRectF(2 + SquareWidth * 3.5 + Margin * 2,
                  y - SquareWidth / 2 + SquareWidth * 3, SquareWidth * 1.75,
                  SquareWidth);
  else
    return QRectF(0, 0, 0, 0);
}

void DsoSignal::set_data(data::DsoSnapshot *data) { _data = data; _data_ref.reset(); }

void DsoSignal::set_data_from_source(data::DataSource *source) {
  _data_ref = source ? source->get_dso_snapshot_shared() : nullptr;
  _data = source ? source->get_dso_snapshot() : nullptr;
}

void DsoSignal::clear_data() { _data = nullptr; _data_ref.reset(); }

} // namespace view
} // namespace pv
