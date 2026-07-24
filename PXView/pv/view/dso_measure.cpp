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

#include "dso_measure.h"

#include <QApplication>
#include <functional>
#include <math.h>
#include <libsigrok/libsigrok.h>

#include "../api/types.h"        // Task C1.7: api::MeasurementValue
#include "../data/dsosnapshot.h"
#include "../data/signalmodel.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../sigsession.h"
#include "dsosignal.h"
#include "view.h"

using namespace std;

namespace pv {
namespace view {

DsoMeasure::DsoMeasure(DsoSignal *signal) : _signal(signal) {}

DsoMeasure::~DsoMeasure() {}

QString DsoMeasure::get_measure(int type) {
  const QString mNone = "--";

  if (!_signal->_data || _signal->_data->empty()) {
    return mNone;
  }

  // Task C1.7: computation moved to Core layer (core::MeasureCalculator,
  // reached via DataSource::get_measurements). The View layer keeps only
  // the display formatting logic below. Pass the actual view_rect_height
  // so GUI-displayed voltages match the original DsoMeasure computation
  // (the voltage formula divides by view_rect_height).
  const int view_rect_height = _signal->get_view_rect().height();
  auto measurements = _signal->_data_source->get_measurements(
      _signal->get_index(), view_rect_height);

  const api::MeasurementValue *found = nullptr;
  for (const auto &mv : measurements) {
    if (mv.type == type) {
      found = &mv;
      break;
    }
  }

  if (!found || !found->valid) {
    return mNone;
  }

  // Format the value — matches the original DsoMeasure::get_measure switch.
  // The Core returns values in the base unit (mV / ns / Hz / % / count);
  // the View rescales for display exactly as the original code did.
  switch (type) {
  case DSO_MS_AMPT:
  case DSO_MS_VHIG:
  case DSO_MS_VLOW:
  case DSO_MS_VP2P:
  case DSO_MS_VMAX:
  case DSO_MS_VMIN:
  case DSO_MS_VRMS:
  case DSO_MS_VMEA:
    // value already in millivolts — format as V/mV (matches get_voltage)
    return abs(found->value) >= 1000.0
               ? QString::number(found->value / 1000.0, 'f', 2) + "V"
               : QString::number(found->value, 'f', 2) + "mV";
  case DSO_MS_PERD:
  case DSO_MS_PWDT:
  case DSO_MS_NWDT:
  case DSO_MS_RISE:
  case DSO_MS_FALL:
  case DSO_MS_BRST:
    // value in nanoseconds — format as S/mS/uS/nS (matches get_time)
    return (abs(found->value) > 1000000000.0
                ? QString::number(found->value / 1000000000.0, 'f', 2) + "S"
            : abs(found->value) > 1000000.0
                ? QString::number(found->value / 1000000.0, 'f', 2) + "mS"
            : abs(found->value) > 1000.0
                ? QString::number(found->value / 1000.0, 'f', 2) + "uS"
                : QString::number(found->value, 'f', 2) + "nS");
  case DSO_MS_FREQ: {
    // value already in Hz — format as Hz/kHz/MHz.
    // Original computed frequency from period:
    //   period > 1e6 ns  → freq < 1e3 Hz  → "X.XXHz"
    //   period > 1e3 ns  → freq < 1e6 Hz  → "X.XXkHz"
    //   else             → freq >= 1e6 Hz → "X.XXMHz"
    const double freq = found->value;
    if (freq >= 1000000.0)
      return QString::number(freq / 1000000.0, 'f', 2) + "MHz";
    else if (freq >= 1000.0)
      return QString::number(freq / 1000.0, 'f', 2) + "kHz";
    else
      return QString::number(freq, 'f', 2) + "Hz";
  }
  case DSO_MS_NOVR:
  case DSO_MS_POVR:
  case DSO_MS_PDUT:
  case DSO_MS_NDUT:
    return QString::number(found->value, 'f', 2) + "%";
  case DSO_MS_PCNT: {
    // Original: >1e6 → "X.XXXXXXM", >1e3 → "X.XXXK", else "X"
    const double pcnt = found->value;
    return (pcnt > 1000000.0
                ? QString::number(pcnt / 1000000.0, 'f', 6) + "M"
            : pcnt > 1000.0
                ? QString::number(pcnt / 1000.0, 'f', 3) + "K"
                : QString::number(pcnt, 'f', 0));
  }
  default:
    return "Error";
  }
}

bool DsoMeasure::measure(const QPointF &p) {
  _signal->_hover_en = false;

  if (!_signal->enabled() || !_signal->show())
    return false;

  if (_signal->_data_source->is_stopped_status() == false)
    return false;

  const QRectF window = _signal->get_view_rect();
  if (!window.contains(p))
    return false;

  if (!_signal->_data || _signal->_data->empty())
    return false;

  _signal->_hover_index = _signal->_view->pixel2index(p.x());
  if (_signal->_hover_index >= _signal->_data->get_sample_count())
    return false;

  int chan_index = _signal->get_index();
  if (_signal->_data->has_data(chan_index) == false) {
    pxv_err("channel %d have no data.", chan_index);
    return false;
  }

  _signal->_hover_point = get_point(_signal->_hover_index, _signal->_hover_value);
  _signal->_hover_en = true;
  return true;
}

bool DsoMeasure::get_hover(uint64_t &index, QPointF &p, double &value) {
  if (_signal->_hover_en) {
    index = _signal->_hover_index;
    p = _signal->_hover_point;
    value = _signal->_hover_value;
    return true;
  }
  return false;
}

QPointF DsoMeasure::get_point(uint64_t index, float &value) {
  QPointF pt = QPointF(-1, -1);

  if (!_signal->enabled() || !_signal->_data)
    return pt;

  if (_signal->_data->empty())
    return pt;

  if (index >= _signal->_data->get_sample_count())
    return pt;

  value = *_signal->_data->get_samples(index, index, _signal->get_index());
  const float top = _signal->get_view_rect().top();
  const float bottom = _signal->get_view_rect().bottom();
  const int hw_offset = _signal->get_hw_offset();
  const float x = _signal->_view->index2pixel(index);
  const float y =
      min(max(top, _signal->get_zero_vpos() + (value - hw_offset) * _signal->_scale), bottom);
  pt = QPointF(x, y);

  return pt;
}

double DsoMeasure::get_voltage(uint64_t index) {
  if (!_signal->enabled() || !_signal->_data)
    return 1;

  if (_signal->_data->empty())
    return 1;

  if (index >= _signal->_data->get_sample_count())
    return 1;

  const double value = *_signal->_data->get_samples(index, index, _signal->get_index());
  const int hw_offset = _signal->get_hw_offset();
  uint64_t k = _signal->_data->get_measure_voltage_factor(_signal->get_index());
  float data_scale = _signal->_data->get_data_scale(_signal->get_index());

  return (hw_offset - value) * data_scale * k * _signal->_vDial->get_factor() *
         DS_CONF_DSO_VDIVS / _signal->get_view_rect().height();
}

QString DsoMeasure::get_voltage(double v, int p, bool scaled) {
  if (_signal->_vDial == NULL) {
    assert(false);
    return QString("--");
  }

  if (_signal->get_view_rect().height() == 0) {
    assert(false);
    return QString("--");
  }

  if (!_signal->_data)
    return QString("--");

  uint64_t k = _signal->_data->get_measure_voltage_factor(_signal->get_index());
  float data_scale = _signal->_data->get_data_scale(_signal->get_index());

  if (scaled)
    v = v * k * _signal->_vDial->get_factor() * DS_CONF_DSO_VDIVS /
        _signal->get_view_rect().height();
  else
    v = v * data_scale * k * _signal->_vDial->get_factor() * DS_CONF_DSO_VDIVS /
        _signal->get_view_rect().height();

  return abs(v) >= 1000 ? QString::number(v / 1000.0, 'f', p) + "V"
                        : QString::number(v, 'f', p) + "mV";
}

QString DsoMeasure::get_time(double t) {
  QString str =
      (abs(t) > 1000000000 ? QString::number(t / 1000000000, 'f', 2) + "S"
       : abs(t) > 1000000  ? QString::number(t / 1000000, 'f', 2) + "mS"
       : abs(t) > 1000     ? QString::number(t / 1000, 'f', 2) + "uS"
                           : QString::number(t, 'f', 2) + "nS");
  return str;
}

void DsoMeasure::auto_set() {
  if (_signal->_data_source->is_stopped_status()) {
    if (_signal->_autoV)
      autoV_end();
    if (_signal->_autoH)
      autoH_end();
  } else {
    if (_signal->_autoH && _signal->_autoV && _signal->get_zero_ratio() != 0.5) {
      _signal->set_zero_ratio(0.5);
    }
    if (_signal->_mValid && !_signal->_data_source->get_data_auto_lock()) {
      if (_signal->_autoH) {
        bool roll = false;
        _signal->_data_source->device()->is_roll_mode(roll);

        const double hori_res = _signal->_view->get_hori_res();
        if (_signal->_level_valid &&
            ((!roll && _signal->_pcount < 3) || _signal->_period > 4 * hori_res)) {
          _signal->_view->zoom(-1);
        } else if (_signal->_level_valid && _signal->_pcount > 6 && _signal->_period < 1.5 * hori_res) {
          _signal->_view->zoom(1);
        } else if (_signal->_level_valid) {
          autoH_end();
        }
      }
      if (_signal->_autoV) {
        const bool over_flag = _signal->_max == 0xff || _signal->_min == 0x0;
        const bool out_flag = _signal->_max >= 0xE0 || _signal->_min <= 0x20;
        const bool under_flag = _signal->_max <= 0xA0 && _signal->_min >= 0x60;
        if (over_flag) {
          if (!_signal->_autoV_over)
            _signal->_auto_cnt = 0;
          _signal->_autoV_over = true;
          _signal->go_vDialNext(false);
        } else if (out_flag) {
          _signal->go_vDialNext(false);
        } else if (!_signal->_autoV_over && under_flag) {
          _signal->go_vDialPre(false);
        } else if (!_signal->_autoH) {
          autoV_end();
        }

        if (_signal->_autoV_over && under_flag) {
          if (_signal->_auto_cnt++ > 16)
            _signal->_autoV_over = false;
        } else {
          _signal->_auto_cnt = 0;
        }

        if (_signal->_level_valid) {
          _signal->_trig_value = (_signal->_min + _signal->_max) / 2;
          _signal->set_trig_vpos(_signal->ratio2pos(_signal->get_trig_vrate()));
        }
      }
      if (_signal->_autoH || _signal->_autoV)
        _signal->_data_source->data_auto_lock(DsoSignal::AutoLock);
    }
  }
}

void DsoMeasure::autoV_end() {
  _signal->_autoV = false;
  _signal->_autoV_over = false;
  _signal->_view->auto_trig(_signal->get_index());
  _signal->_trig_value = (_signal->_min + _signal->_max) / 2;
  _signal->set_trig_vpos(_signal->ratio2pos(_signal->get_trig_vrate()));
  _signal->_view->set_update(_signal->_viewport, true);
  _signal->_view->update();
}

void DsoMeasure::autoH_end() {
  _signal->_autoH = false;
  _signal->_view->set_update(_signal->_viewport, true);
  _signal->_view->update();
}

void DsoMeasure::auto_end() {
  if (_signal->_autoV)
    autoV_end();
  if (_signal->_autoH)
    autoH_end();
}

void DsoMeasure::auto_start() {
  if (_signal->_autoV || _signal->_autoH)
    return;

  if (_signal->_data_source->is_running_status()) {
    _signal->_data_source->data_auto_lock(DsoSignal::AutoLock);
    _signal->_autoV = true;
    _signal->_autoH = true;
    _signal->_view->auto_trig(_signal->get_index());
    _signal->_end_timer.TimeOut(DsoSignal::AutoTime, std::bind(&DsoMeasure::call_auto_end,
                                           this)); // start a timeout
  }
}

void DsoMeasure::call_auto_end() { _signal->_data_source->auto_end(); }

void DsoMeasure::paint_hover_measure(QPainter &p, QColor fore, QColor back) {
  const int hw_offset = _signal->get_hw_offset();
  // Hover measure
  if (_signal->_hover_en && _signal->_hover_point != QPointF(-1, -1)) {
    QString hover_str = _signal->get_voltage(hw_offset - _signal->_hover_value, 2);
    const int hover_width =
        p.boundingRect(0, 0, INT_MAX, INT_MAX, Qt::AlignLeft | Qt::AlignTop,
                       hover_str)
            .width() +
        10;
    const int hover_height =
        p.boundingRect(0, 0, INT_MAX, INT_MAX, Qt::AlignLeft | Qt::AlignTop,
                       hover_str)
            .height();
    QRectF hover_rect(_signal->_hover_point.x(), _signal->_hover_point.y() - hover_height / 2,
                      hover_width, hover_height);
    if (hover_rect.right() > _signal->get_view_rect().right())
      hover_rect.moveRight(_signal->_hover_point.x());
    if (hover_rect.top() < _signal->get_view_rect().top())
      hover_rect.moveTop(_signal->_hover_point.y());
    if (hover_rect.bottom() > _signal->get_view_rect().bottom())
      hover_rect.moveBottom(_signal->_hover_point.y());

    p.setPen(fore);
    p.setBrush(back);
    p.drawRect(_signal->_hover_point.x() - 1, _signal->_hover_point.y() - 1, _signal->HoverPointSize,
               _signal->HoverPointSize);
    p.drawText(hover_rect, Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
               hover_str);
  }

  auto &cursor_list = _signal->_view->get_cursorList();
  auto i = cursor_list.begin();

  while (i != cursor_list.end()) {
    float pt_value;

    int chan_index = (*i)->index();
    if (!_signal->_data || _signal->_data->has_data(chan_index) == false) {
      i++;
      continue;
    }

    const QPointF pt = _signal->get_point(chan_index, pt_value);
    if (pt == QPointF(-1, -1)) {
      i++;
      continue;
    }

    QString pt_str = _signal->get_voltage(hw_offset - pt_value, 2);
    const int pt_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                        Qt::AlignLeft | Qt::AlignTop, pt_str)
                             .width() +
                         10;
    const int pt_height = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                         Qt::AlignLeft | Qt::AlignTop, pt_str)
                              .height();
    QRectF pt_rect(pt.x(), pt.y() - pt_height / 2, pt_width, pt_height);
    if (pt_rect.right() > _signal->get_view_rect().right())
      pt_rect.moveRight(pt.x());
    if (pt_rect.top() < _signal->get_view_rect().top())
      pt_rect.moveTop(pt.y());
    if (pt_rect.bottom() > _signal->get_view_rect().bottom())
      pt_rect.moveBottom(pt.y());

    p.drawRect(pt.x() - 1, pt.y() - 1, 2, 2);
    p.drawLine(pt.x() - 2, pt.y() - 2, pt.x() + 2, pt.y() + 2);
    p.drawLine(pt.x() + 2, pt.y() - 2, pt.x() - 2, pt.y() + 2);
    p.drawText(pt_rect, Qt::AlignCenter | Qt::AlignTop | Qt::TextDontClip,
               pt_str);

    i++;
  }
}

} // namespace view
} // namespace pv
