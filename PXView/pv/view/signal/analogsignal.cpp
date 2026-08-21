/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "pv/view/signal/analogsignal.h"
#include "pv/config/appconfig.h"
#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/datasource.h"
#include "pv/data/model/signalmodel.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/view/view.h"
#include "pv/view/renderer/rasterize.h"
#include <cmath>
#include <algorithm>

using namespace std;

#define byte(x) uint##x##_t

namespace pv {
namespace view {

const QColor AnalogSignal::SignalColours[4] = {
    QColor(238, 178, 17, 255), // dsYellow
    QColor(0, 153, 37, 255),   // dsGreen
    QColor(213, 15, 37, 255),  // dsRed
    QColor(17, 133, 209, 255)  // dsBlue
};

static const char *SignalColorTokens[4] = {"@signal-orange", "@signal-green",
                                           "@signal-red", "@signal-blue"};

QColor AnalogSignal::getSignalColor(int index) {
  QColor c = AppConfig::Instance().GetThemeColor(SignalColorTokens[index % 4]);
  return c.isValid() ? c : SignalColours[index % 4];
}

// LDO (Low-Density Optimization) 双路径阈值，参考逻辑信号
// LogicSignal::paint_mid_align (logicsignal.cpp:200-224) 的密度分界:
//   低密度 (_cur_edges.size() < max_togs) → 连接线 (连续外观)
//   高密度 (>= max_togs)                  → 每像素条 (条重叠成连续外观)
//
// modernize-thread-model Task 2: the analog waveform rasterization lives in
// the pure function rasterize_analog_channel() (pv/view/renderer/rasterize.h).
// Its dual-mode body (interpolation for spp<1.0, min/max for spp>=1.0) is the
// single rendering path; the former paint_trace / paint_envelope / paint_per_pixel
// members are removed. Note: drawPolyline with alpha on a transparent QPixmap
// is extremely slow on the Windows raster engine, so the rasterizer draws
// opaque polylines when zoomed in and opaque min/max rects when zoomed out.
AnalogSignal::AnalogSignal(data::AnalogSnapshot *data,
                           std::shared_ptr<data::SignalModel> model,
                           data::DataSource *data_source)
    : Signal(model, data_source), _data(data),
      _cached_hw_offset(model ? model->hw_offset() : 128), _hover_en(false),
      _hover_index(0), _hover_point(QPointF(-1, -1)), _hover_value(0),
      _float_scale(1.0f) {
  _typeWidth = 5;
  _colour = getSignalColor(model ? model->index() : 0);

  uint32_t ui32;

  // channel bits
  bool ret = _data_source->device()->get_config_byte(SR_CONF_UNIT_BITS, _bits);
  if (!ret) {
    _bits = DefaultBits;
    // demo/file 设备不支持 SR_CONF_UNIT_BITS，静默使用默认值避免日志噪音
    if (_data_source->device()->is_hardware())
      pxv_warn("config_get SR_CONF_UNIT_BITS failed, set to %d (default)",
               DefaultBits);
  }

  ret = _data_source->device()->get_config_uint32(SR_CONF_REF_MIN, ui32);
  if (ret)
    _ref_min = (double)ui32;
  else
    _ref_min = 1;

  ret = _data_source->device()->get_config_uint32(SR_CONF_REF_MAX, ui32);
  if (ret)
    _ref_max = (double)ui32;
  else
    _ref_max = ((1 << _bits) - 1);

  // -- vpos (read from SignalModel if available, otherwise from device)
  // FIX: 旧代码用 _model->vertical_offset() (UI 布局 y 像素位置，如 345) 设置
  // _zero_offset (ADC 偏移值，范围 [_ref_min, _ref_max]，通常 [1,255])。这导致
  // value2ratio(_zero_offset) 算出 1.354（远超 [0,1]），ratio2pos 又算出
  // 365.5（超出信号区域 [333,357]），最终 float 波形只显示 3.5 像素高度
  // （"迷你波形"问题）。
  // 正确做法：使用 _model->zero_offset()（ADC 值），它是 SignalModel 中独立
  // 于 UI 布局的字段；fallback 到设备 SR_CONF_PROBE_OFFSET。
  if (_model) {
    _zero_offset = (int)_model->zero_offset();
  } else {
    ret = _data_source->device()->get_config_uint16(SR_CONF_PROBE_OFFSET,
                                                    _zero_offset, nullptr, nullptr);
    if (!ret) {
      pxv_err("ERROR: config_get SR_CONF_PROBE_OFFSET failed.");
    }
  }
}

AnalogSignal::AnalogSignal(view::AnalogSignal *s,
                           pv::data::AnalogSnapshot *data,
                           std::shared_ptr<data::SignalModel> model,
                           data::DataSource *data_source)
    : Signal(*s, model, data_source), _data(data),
      _cached_hw_offset(s->_cached_hw_offset), _hover_en(false),
      _hover_index(0), _hover_point(QPointF(-1, -1)), _hover_value(0),
      _float_scale(s->_float_scale) {
  _typeWidth = 5;
  _bits = s->get_bits();
  _ref_min = s->get_ref_min();
  _ref_max = s->get_ref_max();
  _zero_offset = s->get_zero_offset();

  _scale = s->get_scale();
}

AnalogSignal *AnalogSignal::clone() const {
  AnalogSignal *cloned = new AnalogSignal(const_cast<AnalogSignal *>(this),
                                          nullptr, _model, _data_source);
  cloned->_local_enabled = _local_enabled;
  cloned->_visible = _visible;
  return cloned;
}

AnalogSignal::~AnalogSignal() {
}

int AnalogSignal::get_hw_offset() {
  if (_data_source->is_running_status()) {
    int hw_offset = _cached_hw_offset;
    sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
    if (probe && _data_source->device()->get_config_uint16(
                     SR_CONF_PROBE_HW_OFFSET, hw_offset, probe, nullptr)) {
      _cached_hw_offset = hw_offset;
    }
  }
  return _cached_hw_offset;
}

int AnalogSignal::commit_settings() {
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  if (!probe)
    return 0;

  // -- enable
  _model->set_probe_enabled(enabled(), probe);

  // -- vdiv
  _model->set_vdiv(_model ? _model->vdiv() : 0);

  // -- coupling
  _model->set_coupling(_model ? _model->coupling() : 0);

  // -- offset
  _model->set_probe_offset(_model ? (uint16_t)_model->vertical_offset() : 0,
                           probe);

  // -- trig_value
  _model->set_trigger_value(_model ? _model->trig_value() : 0, probe);

  return 1;
}

bool AnalogSignal::measure(const QPointF &p) {
  _hover_en = false;
  if (!enabled())
    return false;

  if (_data_source->is_stopped_status() == false)
    return false;

  const QRectF window = get_view_rect();
  if (!window.contains(p))
    return false;

  if (!_data || _data->have_data() == false)
    return false;

  const double scale = _view->scale();
  if (scale <= 0)
    return false;
  const int64_t pixels_offset = _view->offset();
  const double samplerate = _data_source->cur_snap_samplerate();
  const double samples_per_pixel = samplerate * scale;

  _hover_index = floor((p.x() + pixels_offset) * samples_per_pixel + 0.5);
  if (_hover_index >= _data->get_sample_count())
    return false;

  _hover_point = get_point(_hover_index, _hover_value);
  _hover_en = true;
  return true;
}

bool AnalogSignal::get_hover(uint64_t &index, QPointF &p, double &value) {
  if (_hover_en) {
    index = _hover_index;
    p = _hover_point;
    value = _hover_value;
    return true;
  }
  return false;
}

QPointF AnalogSignal::get_point(uint64_t index, float &value) {
  QPointF pt = QPointF(-1, -1);

  if (!enabled() || !_data)
    return pt;

  const int order = _data->get_ch_order(get_index());
  if (order == -1)
    return pt;

  const double scale = _view->scale();
  if (scale <= 0)
    return pt;
  const int64_t pixels_offset = _view->offset();
  const double samplerate = _data_source->cur_snap_samplerate();
  const double samples_per_pixel = samplerate * scale;

  if (index >= _data->get_sample_count())
    return pt;

  const uint64_t ring_index =
      (uint64_t)(_data->get_ring_start() + floor(index)) %
      _data->get_sample_count();
  const uint8_t unit_bytes = _data->get_unit_bytes();
  const uint8_t *samples = _data->get_samples(ring_index);
  // get_samples(ring_index) 已返回该样本组起始地址，只需通道内 order 偏移
  const uint64_t sample_offs = static_cast<uint64_t>(order) * unit_bytes;

  const int height = get_totalHeight();
  const float top = get_y() - height * 0.5;
  const float bottom = get_y() + height * 0.5;
  const int hw_offset = get_hw_offset();
  const float x = (index / samples_per_pixel - pixels_offset);

  float y;
  if (_data->is_float() && unit_bytes == sizeof(float)) {
    value = *reinterpret_cast<const float*>(samples + sample_offs);
    y = min(max(top, get_zero_vpos() - value * _float_scale), bottom);
  } else {
    value = *(samples + sample_offs);
    for (uint8_t i = 1; i < unit_bytes; i++) {
      value += (samples[sample_offs + i] << i * 8);
    }
    y = min(max(top, get_zero_vpos() + (value - hw_offset) * _scale), bottom);
  }
  pt = QPointF(x, y);

  return pt;
}

/**
 * Probe options
 **/
uint64_t AnalogSignal::get_vdiv() {
  return _model ? (uint64_t)_model->vdiv() : 0;
}

uint8_t AnalogSignal::get_acCoupling() {
  return _model ? (uint8_t)_model->coupling() : 0;
}

bool AnalogSignal::get_mapDefault() {
  return _model ? _model->map_default() : true;
}

QString AnalogSignal::get_mapUnit() {
  QString unit;
  // Fork analog probe-map keys (60059/60060/60061) only exist on DSL/PXLogic
  // devices. Skipping the query on non-DSL devices avoids flooding the log with
  // "Invalid key 600XX" errors during every paint cycle.
  if (!_data_source || !_data_source->device() ||
      !_data_source->device()->is_dsl_device()) {
    return unit;
  }
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  _data_source->device()->get_config_string(SR_CONF_PROBE_MAP_UNIT, unit, probe,
                                            nullptr);
  return unit;
}

double AnalogSignal::get_mapMin() {
  double min = -1;
  if (!_data_source || !_data_source->device() ||
      !_data_source->device()->is_dsl_device()) {
    return min;
  }
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  _data_source->device()->get_config_double(SR_CONF_PROBE_MAP_MIN, min, probe,
                                            nullptr);
  return min;
}

double AnalogSignal::get_mapMax() {
  double max = 1;
  if (!_data_source || !_data_source->device() ||
      !_data_source->device()->is_dsl_device()) {
    return max;
  }
  sr_channel *probe = _model ? _model->sr_channel_handle() : nullptr;
  _data_source->device()->get_config_double(SR_CONF_PROBE_MAP_MAX, max, probe,
                                            nullptr);
  return max;
}

uint64_t AnalogSignal::get_factor() {
  return _model ? (uint64_t)_model->vfactor() : 1;
}

int AnalogSignal::ratio2value(double ratio) {
  return ratio * (_ref_max - _ref_min) + _ref_min;
}

int AnalogSignal::ratio2pos(double ratio) {
  const int height = get_totalHeight();
  const int top = get_y() - height * 0.5;
  return ratio * height + top;
}

double AnalogSignal::value2ratio(int value) {
  return max(0.0, (value - _ref_min) / (_ref_max - _ref_min));
}

double AnalogSignal::pos2ratio(int pos) {
  const int height = get_totalHeight();
  const int top = get_y() - height / 2;
  return min(max(pos - top, 0), height) * 1.0 / height;
}

void AnalogSignal::set_zero_vpos(int pos) {
  if (enabled()) {
    set_zero_ratio(pos2ratio(pos));
  }
}

int AnalogSignal::get_zero_vpos() { return ratio2pos(get_zero_ratio()); }

void AnalogSignal::set_zero_ratio(double ratio) {
  if (_data_source->is_running_status())
    return;

  // Same nested-broadcast guard as DsoSignal::set_zero_ratio: set_config_uint16
  // triggers synchronous config_changed -> broadcast_async<>, which may delete this
  // AnalogSignal mid-method.
  auto model = _model;
  _zero_offset = ratio2value(ratio);
  sr_channel *probe = model ? model->sr_channel_handle() : nullptr;
  if (model) {
    // set_probe_offset always pushes SR_CONF_PROBE_OFFSET to the device (no
    // "if changed" guard), preserving the original unconditional
    // set_config_uint16 semantics. set_zero_offset below pushes it again when
    // the model field actually changes (same double-push as before).
    model->set_probe_offset((uint16_t)_zero_offset, probe);
    // Task 7.3: 写回 Core SignalModel。不广播：本方法亦被 mainwindow JSON
    // 恢复路径 (mainwindow.cpp restore_session) 调用，广播会触发 rebuild 循环。
    model->set_zero_offset((double)_zero_offset);
  } else {
    _data_source->device()->set_config_uint16(SR_CONF_PROBE_OFFSET,
                                              _zero_offset, probe, nullptr);
  }
}

double AnalogSignal::get_zero_ratio() {
  // FIX: 对 float 电压数据 (demo 驱动)，ADC 概念的 _zero_offset/_ref_min/_ref_max
  // 不适用。初始 _zero_offset=0，而 _ref_min=1/_ref_max=255，value2ratio 内部
  // 有 max(0.0,...) 保护会 clamp 到 0.0（顶部），导致 zeroY 在顶部而非中心。
  // 修复：直接检查 _zero_offset 是否在 [_ref_min, _ref_max] 范围内，
  // 不在范围内则返回中心 0.5，使波形上下对称填满显示区域。
  // 用户手动拖动游标后 _zero_offset 会被设置为 ratio2value(ratio)（落入
  // [_ref_min, _ref_max]），此时返回对应 ratio。
  // 注意：采集前 _data 可能为 nullptr 或 not is_float，但 _zero_offset=0 仍然
  // 不在 [_ref_min, _ref_max] 范围内，统一返回 0.5 使初始化时游标也居中。
  if (_zero_offset >= (int)_ref_min && _zero_offset <= (int)_ref_max) {
    double r = (_zero_offset - _ref_min) / (_ref_max - _ref_min);
    return r;
  }
  return 0.5;
}

/**
 * Paint
 **/
void AnalogSignal::paint_back(QPainter &p, int left, int right, QColor fore,
                              QColor back, const PaintContext &ctx) {
  (void)ctx;
  assert(_view);

  int i, j;
  const double height = get_totalHeight();
  const int DIVS = DS_CONF_DSO_VDIVS;
  const int minDIVS = 5;
  const double STEPS = height / (DIVS * minDIVS);
  const double mapSteps = (get_mapMax() - get_mapMin()) / DIVS;
  const QString mapUnit = get_mapUnit();

  QPen solidPen(fore);
  solidPen.setStyle(Qt::SolidLine);
  p.setPen(solidPen);
  p.setBrush(back);

  // paint rule
  double y = get_y() - height * 0.5;
  double mapValue =
      get_mapMax() + (get_zero_ratio() - 0.5) * (get_mapMax() - get_mapMin());
  for (i = 0; i < DIVS; i++) {
    p.drawLine(left, y, left + 10, y);
    if (i == 0 || i == DIVS / 2)
      p.drawText(QRectF(left + 15, y - 10, 100, 20),
                 Qt::AlignLeft | Qt::AlignVCenter,
                 QString::number(mapValue, 'f', 2) + mapUnit);
    p.drawLine(right, y, right - 10, y);
    if (i == 0 || i == DIVS / 2)
      p.drawText(QRectF(right - 115, y - 10, 100, 20),
                 Qt::AlignRight | Qt::AlignVCenter,
                 QString::number(mapValue, 'f', 2) + mapUnit);
    for (j = 0; j < minDIVS - 1; j++) {
      y += STEPS;
      p.drawLine(left, y, left + 5, y);
      p.drawLine(right, y, right - 5, y);
    }
    y += STEPS;
    mapValue -= mapSteps;
  }
  p.drawLine(left, y, left + 10, y);
  p.drawText(QRectF(left + 15, y - 10, 100, 20),
             Qt::AlignLeft | Qt::AlignVCenter,
             QString::number(mapValue, 'f', 2) + mapUnit);
  p.drawLine(right, y, right - 10, y);
  p.drawText(QRectF(right - 115, y - 10, 100, 20),
             Qt::AlignRight | Qt::AlignVCenter,
             QString::number(mapValue, 'f', 2) + mapUnit);
}

void AnalogSignal::paint_mid(QPainter &p, int left, int right, QColor fore,
                             QColor back, const PaintContext &ctx) {
  (void)fore;
  (void)back;

  // Refresh colour from theme on every paint
  _colour = getSignalColor(_model ? _model->index() : 0);

  if (!_data)
    return;
  assert(_view);
  assert(right >= left);

  const int height = get_totalHeight();
  const float top = get_y() - height * 0.5;
  const float bottom = get_y() + height * 0.5;
  // zeroY 由 get_zero_ratio() 计算；float 数据路径已在 get_zero_ratio() 中
  // 处理 _zero_offset 超界情况 (返回中心 0.5)。
  const float zeroY = ratio2pos(get_zero_ratio());
  const int width = right - left + 1;

  const double scale = ctx.scale;

  if (scale <= 0)
    return;
  const int64_t offset = ctx.offset;

  const int order = _data->get_ch_order(get_index());
  if (order == -1)
    return;

  // The channel have no data.
  if (_data->has_enabled_channel(get_index()) == false) {
    return;
  }

  // 上游 libsigrok analog 数据为 float 电压值时（encoding->is_float），
  // PXView 原有的 ADC 整数路径（hw_offset + _scale）不再适用。
  // 参考 PulseView perform_autoranging + scale_ = div_height / resolution：
  // 根据数据实际 min/max 动态计算 _float_scale，使波形适配显示区域。
  if (_data->is_float() && _data->has_float_range()) {
    float fmin = 0.0f, fmax = 0.0f;
    _data->get_float_min_max(fmin, fmax);
    const float max_abs = std::max(fabsf(fmin), fabsf(fmax));
    if (max_abs > 1e-6f) {
      _float_scale = (height * 0.5f) / max_abs;
    }
  }

  const double pixels_offset = offset;
  // Use document_snapshot_source samplerate for coordinate consistency
  const double samplerate = _data_source->cur_snap_samplerate();
  const int64_t cur_sample_count = _data->get_sample_count();
  const double samples_per_pixel = samplerate * scale;
  const uint64_t ring_start = _data->get_ring_start();

  uint64_t start_index;
  const double index_offset = pixels_offset * samples_per_pixel;
  start_index = (uint64_t)(ring_start + floor(index_offset)) % cur_sample_count;

  int64_t show_length = min(floor(cur_sample_count - floor(index_offset)),
                            ceil(width * samples_per_pixel + 1));
  if (show_length <= 0) {
    return;
  }

  // modernize-thread-model Task 2: the analog waveform logic was extracted to
  // the pure function rasterize_analog_channel() (see pv/view/renderer/rasterize.h).
  // paint_mid stays on the GUI thread and passes the prepare results
  // (zeroY / hw_offset / _scale / _float_scale / top / bottom / _colour) as
  // value parameters; the function reads only the snapshot + those values.
  rasterize_analog_channel(p, _data, zeroY, left, right, start_index,
                           show_length, samples_per_pixel, order, top, bottom,
                           get_hw_offset(), _scale, _float_scale, _colour);
}

void AnalogSignal::paint_fore(QPainter &p, int left, int right, QColor fore,
                              QColor back, const PaintContext &ctx) {
  (void)ctx;
  assert(_view);

  fore.setAlpha(View::BackAlpha);
  QPen pen(fore);
  pen.setStyle(Qt::DotLine);
  p.setPen(pen);
  p.drawLine(left, get_zero_vpos(), right, get_zero_vpos());

  fore.setAlpha(View::ForeAlpha);
  if (enabled()) {
    // Paint measure
    if (ctx.is_stopped_status)
      paint_hover_measure(p, fore, back);
  }
}

void AnalogSignal::paint_hover_measure(QPainter &p, QColor fore, QColor back) {
  const int hw_offset = get_hw_offset();
  const int height = get_totalHeight();
  const float top = get_y() - height * 0.5;
  const float bottom = get_y() + height * 0.5;

  // Hover measure
  if (_hover_en && _hover_point != QPointF(-1, -1)) {
    QString hover_str = get_voltage(hw_offset - _hover_value, 2);
    if (_hover_point.y() <= top || _hover_point.y() >= bottom)
      hover_str += "/out";
    const int hover_width =
        p.boundingRect(0, 0, INT_MAX, INT_MAX, Qt::AlignLeft | Qt::AlignTop,
                       hover_str)
            .width() +
        10;
    const int hover_height =
        p.boundingRect(0, 0, INT_MAX, INT_MAX, Qt::AlignLeft | Qt::AlignTop,
                       hover_str)
            .height();
    QRectF hover_rect(_hover_point.x(), _hover_point.y() - hover_height / 2,
                      hover_width, hover_height);
    if (hover_rect.right() > get_view_rect().right())
      hover_rect.moveRight(_hover_point.x());
    if (hover_rect.top() < get_view_rect().top())
      hover_rect.moveTop(_hover_point.y());
    if (hover_rect.bottom() > get_view_rect().bottom())
      hover_rect.moveBottom(_hover_point.y());

    p.setPen(fore);
    p.setBrush(back);
    p.drawRect(_hover_point.x() - 1, _hover_point.y() - 1, HoverPointSize,
               HoverPointSize);
    p.drawText(hover_rect, Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
               hover_str);
  }

  auto &cursor_list = _view->get_cursorList();
  auto i = cursor_list.begin();

  while (i != cursor_list.end()) {
    float pt_value;
    const QPointF pt = get_point((*i)->index(), pt_value);
    if (pt == QPointF(-1, -1)) {
      i++;
      continue;
    }

    QString pt_str = get_voltage(hw_offset - pt_value, 2);
    if (pt.y() <= top || pt.y() >= bottom)
      pt_str += "/out";
    const int pt_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                        Qt::AlignLeft | Qt::AlignTop, pt_str)
                             .width() +
                         10;
    const int pt_height = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                         Qt::AlignLeft | Qt::AlignTop, pt_str)
                              .height();
    QRectF pt_rect(pt.x(), pt.y() - pt_height / 2, pt_width, pt_height);
    if (pt_rect.right() > get_view_rect().right())
      pt_rect.moveRight(pt.x());
    if (pt_rect.top() < get_view_rect().top())
      pt_rect.moveTop(pt.y());
    if (pt_rect.bottom() > get_view_rect().bottom())
      pt_rect.moveBottom(pt.y());

    p.drawRect(pt.x() - 1, pt.y() - 1, 2, 2);
    p.drawLine(pt.x() - 2, pt.y() - 2, pt.x() + 2, pt.y() + 2);
    p.drawLine(pt.x() + 2, pt.y() - 2, pt.x() - 2, pt.y() + 2);
    p.drawText(pt_rect, Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
               pt_str);

    i++;
  }
}

QString AnalogSignal::get_voltage(double v, int p, bool scaled) {
  const double mapRange = (get_mapMax() - get_mapMin()) * 1000;
  const QString mapUnit = get_mapUnit();

  if (scaled)
    v = v / (double)get_totalHeight() * mapRange;
  else
    v = v * _scale / (double)get_totalHeight() * mapRange;

  return abs(v) >= 1000 ? QString::number(v / 1000.0, 'f', p) + mapUnit
                        : QString::number(v, 'f', p) + "m" + mapUnit;
}

void AnalogSignal::set_data(data::AnalogSnapshot *data) {
  _data = data;
  _data_ref.reset();
}

void AnalogSignal::set_data_from_source(data::DataSource *source) {
  _data_ref = source ? source->get_analog_snapshot_shared() : nullptr;
  _data = source ? source->get_analog_snapshot() : nullptr;
}

void AnalogSignal::clear_data() { _data = nullptr; _data_ref.reset(); }

} // namespace view
} // namespace pv
