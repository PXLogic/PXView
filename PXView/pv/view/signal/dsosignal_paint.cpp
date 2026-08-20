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

// =============================================================================
// Phase 5: Paint methods extracted from dsosignal.cpp for better modularity.
// This file contains all QPainter-based rendering methods for DsoSignal.
// =============================================================================

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
#include "pv/view/renderer/rasterize.h"

using namespace std;

namespace pv {
namespace view {

QRect DsoSignal::get_view_rect() {
  assert(_viewport);
  // In MSO/LOGIC mode, the DSO signal occupies a specific area determined
  // by v_offset (center) and totalHeight, offset by the vertical scroll.
  // Without this, ratio2pos()/pos2ratio() calculate trigger positions
  // based on the full viewport, making the trigger cursor drawn at the
  // wrong position and ungrabbable when scrolled or when other traces
  // are above this DSO signal.
  if (_view && _view->is_logic_rendering_mode()) {
    int top = get_v_offset() - get_totalHeight() / 2 - _view->get_vOffset();
    return QRect(0, top, _viewport->width() - RightMargin, get_totalHeight());
  }
  return QRect(0, UpMargin, _viewport->width() - RightMargin,
               _viewport->height() - UpMargin - DownMargin);
}

void DsoSignal::paint_prepare() {
  assert(_view);

  if (!_data || _data->empty() || !_data->has_data(get_index()))
    return;

  if (_data_source->trigd()) {
    if (get_index() == _data_source->trigd_ch()) {
      uint8_t slope = DSO_TRIGGER_RISING;
      int v;
      bool ret;

      ret = _data_source->device()->get_config_byte(SR_CONF_TRIGGER_SLOPE, v);
      if (ret) {
        slope = (uint8_t)v;
      }

      int64_t trig_index = _view->get_trig_cursor()->index();
      if (trig_index >= (int64_t)_data->get_sample_count())
        return;

      const uint8_t *const trig_samples = _data->get_samples(0, 0, get_index());
      for (uint16_t i = 0; i < TrigHRng; i++) {
        const int64_t i0 = trig_index - i - 1;
        const int64_t i1 = trig_index - i;
        if (i1 < 0)
          break;
        if (i0 < 0)          /* prevent trig_samples[-1] read when trig_index==0 */
          break;
        const uint8_t t0 = trig_samples[i0];
        const uint8_t t1 = trig_samples[i1];
        if ((slope == DSO_TRIGGER_RISING && t0 >= _trig_value &&
             t1 <= _trig_value) ||
            (slope == DSO_TRIGGER_FALLING && t0 <= _trig_value &&
             t1 >= _trig_value)) {
          const double xoff =
              (t1 == t0) ? 0 : (_trig_value - t0) * 1.0 / (t1 - t0);
          _view->set_trig_hoff(i + 1 - xoff);
          break;
        }
      }
    }
  } else {
    _view->set_trig_hoff(0);
  }
}

void DsoSignal::paint_back(QPainter &p, int left, int right, QColor fore,
                           QColor back, const PaintContext &ctx) {
  assert(_view);

  if (!_show)
    return;

  int i, j;
  const int height = get_view_rect().height();
  const int width = right - left;

  fore.setAlpha(View::BackAlpha);

  QPen solidPen(fore);
  solidPen.setStyle(Qt::SolidLine);
  p.setPen(solidPen);
  p.setBrush(back.black() > 0x80 ? back.darker() : back.lighter());
  p.drawRect(left, UpMargin, width, height);

  // draw zoom region
  fore.setAlpha(View::ForeAlpha);
  p.setPen(fore);

  const uint64_t sample_len = _data_source->cur_samplelimits();
  const double samplerate = _data_source->cur_snap_samplerate();
  const double samples_per_pixel = samplerate * ctx.scale;
  const double shown_rate =
      min(samples_per_pixel * width * 1.0 / sample_len, 1.0);
  const double start = ctx.offset * samples_per_pixel;
  const double shown_offset = min(start / sample_len, 1.0) * width;
  const double shown_len = max(shown_rate * width, 6.0);
  const QPointF left_edge[] = {QPoint(shown_offset + 3, UpMargin / 2 - 6),
                               QPoint(shown_offset, UpMargin / 2 - 6),
                               QPoint(shown_offset, UpMargin / 2 + 6),
                               QPoint(shown_offset + 3, UpMargin / 2 + 6)};
  const QPointF right_edge[] = {
      QPoint(shown_offset + shown_len - 3, UpMargin / 2 - 6),
      QPoint(shown_offset + shown_len, UpMargin / 2 - 6),
      QPoint(shown_offset + shown_len, UpMargin / 2 + 6),
      QPoint(shown_offset + shown_len - 3, UpMargin / 2 + 6)};
  p.drawLine(left, UpMargin / 2, shown_offset, UpMargin / 2);
  p.drawLine(shown_offset + shown_len, UpMargin / 2, left + width,
             UpMargin / 2);
  p.drawPolyline(left_edge, countof(left_edge));
  p.drawPolyline(right_edge, countof(right_edge));
  p.setBrush(fore);
  p.drawRect(shown_offset, UpMargin / 2 - 3, shown_len, 6);

  // draw divider
  fore.setAlpha(View::BackAlpha);
  QPen dashPen(fore);
  dashPen.setStyle(Qt::DashLine);
  p.setPen(dashPen);
  const double spanY = height * 1.0 / DS_CONF_DSO_VDIVS;
  for (i = 1; i <= DS_CONF_DSO_VDIVS; i++) {
    const double posY = spanY * i + UpMargin;
    if (i != DS_CONF_DSO_VDIVS)
      p.drawLine(left, posY, right, posY);
    const double miniSpanY = spanY / 5;
    for (j = 1; j < 5; j++) {
      p.drawLine(width / 2.0f - 5, posY - miniSpanY * j, width / 2.0f + 5,
                 posY - miniSpanY * j);
    }
  }
  const double spanX = width * 1.0 / DS_CONF_DSO_HDIVS;
  for (i = 1; i <= DS_CONF_DSO_HDIVS; i++) {
    const double posX = spanX * i;
    if (i != DS_CONF_DSO_HDIVS)
      p.drawLine(posX, UpMargin, posX, height + UpMargin);
    const double miniSpanX = spanX / 5;
    for (j = 1; j < 5; j++) {
      p.drawLine(posX - miniSpanX * j, height / 2.0f + UpMargin - 5,
                 posX - miniSpanX * j, height / 2.0f + UpMargin + 5);
    }
  }
  _view->set_back(true);
}

void DsoSignal::paint_mid(QPainter &p, int left, int right, QColor fore,
                          QColor back, const PaintContext &ctx) {
  (void)fore;
  (void)back;

  // Refresh colour from theme on every paint
  _colour = getSignalColor(_model ? _model->index() : 0);

  if (!_show || right <= left) {
    return;
  }

  if (!_data)
    return;

  assert(_view);

  if (enabled()) {
    const int index = get_index();
    const int width = right - left;
    const float zeroY = get_zero_vpos();

    const double scale = ctx.scale;
    if (scale <= 0)
      return;
    const int64_t offset = ctx.offset;

    if (!_data || _data->empty() || !_data->has_data(index)) {
      return;
    }

    // Use document_snapshot_source samplerate for coordinate consistency
    const double samplerate = _data_source->cur_snap_samplerate();

    if (samplerate <= 0) {
        pxv_warn("DsoSignal::paint_mid: samplerate <= 0, skipping paint");
        return;
    }

    const int64_t last_sample =
        max((int64_t)(_data->get_sample_count() - 1), (int64_t)0);
    const double samples_per_pixel = samplerate * scale;
    const double start = offset * samples_per_pixel - ctx.trig_hoff;
    const double end = start + samples_per_pixel * width;

    const int64_t start_sample =
        min(max((int64_t)floor(start), (int64_t)0), last_sample);
    const int64_t end_sample =
        min(max((int64_t)ceil(end) + 1, (int64_t)0), last_sample);

    QElapsedTimer dso_ft;
    dso_ft.start();
    const int hw_offset = get_hw_offset();
    s_dso_timing.hw_offset_ms = dso_ft.elapsed();

    // modernize-thread-model Task 2: the DSO waveform logic was extracted to
    // the pure function rasterize_dso_channel() (see pv/view/renderer/rasterize.h).
    // paint_mid stays on the GUI thread and passes the prepare results
    // (zeroY / hw_offset / view-rect top-bottom / _scale / _colour) as value
    // parameters; the function reads only the snapshot + those values.
    qint64 dso_paint_start = dso_ft.elapsed();
    const QRect vrect = get_view_rect();
    rasterize_dso_channel(p, _data, zeroY, left, right, start_sample,
                          end_sample, hw_offset, samples_per_pixel,
                          get_index(), vrect.top(), vrect.bottom(), _scale,
                          _colour);
    s_dso_timing.paint_draw_ms = dso_ft.elapsed() - dso_paint_start;
    s_dso_timing.active = true;
    s_dso_timing.sample_count = end_sample - start_sample + 1;
    s_dso_timing.samples_per_pixel = samples_per_pixel;

    // Hot-path debug logging removed for performance — was printing every 20 frames
  }
}

// P1: computed on the GUI thread so the render worker can rasterize this DSO
// channel off-thread. Mirrors the param computation in paint_mid above; keep
// the two in sync. Non-const because it reads GUI-thread state (zero pos /
// hw offset / samplerate).
bool DsoSignal::prepare_raster(const PaintContext &ctx, int left, int right,
                               DsoRasterPrepare &out) {
  if (!_show || right <= left || !_data || !enabled())
    return false;

  const int index = get_index();
  const int width = right - left;
  out.zeroY = get_zero_vpos();

  const double scale = ctx.scale;
  if (scale <= 0)
    return false;
  const int64_t offset = ctx.offset;

  if (!_data->empty() || !_data->has_data(index))
    return false;

  // Use document_snapshot_source samplerate for coordinate consistency
  const double samplerate = _data_source->cur_snap_samplerate();
  if (samplerate <= 0)
    return false;

  const int64_t last_sample =
      max((int64_t)(_data->get_sample_count() - 1), (int64_t)0);
  const double samples_per_pixel = samplerate * scale;
  const double start = offset * samples_per_pixel - ctx.trig_hoff;
  const double end = start + samples_per_pixel * width;

  out.left = left;
  out.right = right;
  out.start_sample =
      min(max((int64_t)floor(start), (int64_t)0), last_sample);
  out.end_sample =
      min(max((int64_t)ceil(end) + 1, (int64_t)0), last_sample);
  out.hw_offset = get_hw_offset();
  out.samples_per_pixel = samples_per_pixel;
  out.channel_index = index;
  const QRect vrect = get_view_rect();
  out.top = vrect.top();
  out.bottom = vrect.bottom();
  out.scale = _scale;
  out.colour = getSignalColor(_model ? _model->index() : 0);
  return true;
}

void DsoSignal::paint_fore(QPainter &p, int left, int right, QColor fore,
                           QColor back, const PaintContext &ctx) {
  if (!_show)
    return;

  assert(_view);

  fore.setAlpha(View::BackAlpha);
  QPen pen(fore);
  pen.setStyle(Qt::DotLine);
  p.setPen(pen);
  p.drawLine(left, get_zero_vpos(), right, get_zero_vpos());

  fore.setAlpha(View::ForeAlpha);
  if (enabled()) {
    const QPointF mouse_point = ctx.hover_point;
    const QRectF label_rect = get_trig_rect(left, right);
    const bool hover = label_rect.contains(mouse_point);

    // Paint the trig line
    const QPointF points[] = {QPointF(right, ratio2pos(get_trig_vrate())),
                              label_rect.topLeft(), label_rect.topRight(),
                              label_rect.bottomRight(),
                              label_rect.bottomLeft()};

    p.setPen(Qt::transparent);
    p.setBrush(_colour);
    p.drawPolygon(points, countof(points));

    p.setPen(fore);
    const QPointF arrow_points[] = {
        QPoint(label_rect.left(), label_rect.center().y()),
        QPoint(label_rect.left(), label_rect.center().y() - 1),
        QPoint(label_rect.left(), label_rect.center().y() + 1),
        QPoint(label_rect.left(), label_rect.center().y() - 2),
        QPoint(label_rect.left(), label_rect.center().y() + 2),
        QPoint(label_rect.left(), label_rect.center().y() - 3),
        QPoint(label_rect.left(), label_rect.center().y() + 3),
        QPoint(label_rect.left(), label_rect.center().y() - 4),
        QPoint(label_rect.left(), label_rect.center().y() + 4),
        QPoint(label_rect.left() - 1, label_rect.center().y() - 3),
        QPoint(label_rect.left() - 1, label_rect.center().y() + 3),
        QPoint(label_rect.left() + 1, label_rect.center().y() - 3),
        QPoint(label_rect.left() + 1, label_rect.center().y() + 3),
        QPoint(label_rect.left() - 1, label_rect.center().y() - 2),
        QPoint(label_rect.left() - 1, label_rect.center().y() + 2),
        QPoint(label_rect.left() + 1, label_rect.center().y() - 2),
        QPoint(label_rect.left() + 1, label_rect.center().y() + 2),
        QPoint(label_rect.left() - 2, label_rect.center().y() - 2),
        QPoint(label_rect.left() - 2, label_rect.center().y() + 2),
        QPoint(label_rect.left() + 2, label_rect.center().y() - 2),
        QPoint(label_rect.left() + 2, label_rect.center().y() + 2),
    };
    if (hover || selected())
      p.drawPoints(arrow_points, countof(arrow_points));

    // paint the trig voltage
    int trigp = ratio2pos(get_trig_vrate());
    QString t_vol_s = get_voltage(get_zero_vpos() - trigp, 2, true);
    int vol_width = p.boundingRect(0, 0, INT_MAX, INT_MAX,
                                   Qt::AlignLeft | Qt::AlignTop, t_vol_s)
                        .width();
    const QRectF t_vol_rect =
        QRectF(right - vol_width, trigp - 10, vol_width, 20);
    p.setPen(fore);
    p.drawText(t_vol_rect, Qt::AlignRight | Qt::AlignVCenter | Qt::TextDontClip,
               t_vol_s);

    // paint the _trig_vpos line
    if (ctx.dso_trig_moved) {
      p.setPen(QPen(_colour, 1, Qt::DotLine));
      p.drawLine(left, trigp,
                 right -
                     p.boundingRect(t_vol_rect, Qt::AlignLeft, t_vol_s).width(),
                 trigp);
    }

    // Paint the text
    p.setPen(fore);
    p.drawText(label_rect,
               Qt::AlignCenter | Qt::AlignVCenter | Qt::TextDontClip, "T");

    // Paint measure
    if (ctx.is_stopped_status)
      paint_hover_measure(p, fore, back);

    // autoset — throttled to every 10th frame (~3/sec at 30 FPS) to avoid
    // cascading update() calls from zoom/go_vDial* inside auto_set().
    // auto_set() calls _view->zoom(), _view->go_vDialNext/Pre() etc., each
    // of which triggers _view->update(), creating a cascade of repaints
    // when called on every single paint cycle.
    static thread_local int _auto_set_frame_cnt = 0;
    if (++_auto_set_frame_cnt >= 10) {
      _auto_set_frame_cnt = 0;
      auto_set();
    }
  }
}

QRectF DsoSignal::get_trig_rect(int left, int right) {
  (void)left;

  return QRectF(right + SquareWidth / 2,
                ratio2pos(get_trig_vrate()) - SquareWidth / 2, SquareWidth,
                SquareWidth);
}

void DsoSignal::paint_type_options(QPainter &p, int right, const QPoint pt,
                                   QColor fore) {
  p.setRenderHint(QPainter::Antialiasing, true);

  QColor foreBack = fore;
  foreBack.setAlpha(View::BackAlpha);
  int y = get_y();
  const QRectF vDial_rect = get_rect(DSO_VDIAL, y, right);
  const QRectF x1_rect = get_rect(DSO_X1, y, right);
  const QRectF x10_rect = get_rect(DSO_X10, y, right);
  const QRectF x100_rect = get_rect(DSO_X100, y, right);
  const QRectF acdc_rect = get_rect(DSO_ACDC, y, right);
  const QRectF chEn_rect = get_rect(DSO_CHEN, y, right);
  const QRectF auto_rect = get_rect(DSO_AUTO, y, right);

  QString pText;
  _vDial->paint(p, vDial_rect, _colour, pt, pText);
  QFontMetrics fm(p.font());
  const QRectF valueRect =
      QRectF(chEn_rect.left(), vDial_rect.top() - fm.height() - 10, right,
             fm.height());
  p.drawText(valueRect, Qt::AlignCenter, pText);

  QString strings[6];
  strings[0] = L_S(STR_PAGE_DLG, S_ID(IDS_DSO_CTR_EN), "EN");
  strings[1] = L_S(STR_PAGE_DLG, S_ID(IDS_DSO_CTR_DIS), "DIS");
  strings[2] = L_S(STR_PAGE_DLG, S_ID(IDS_DSO_CTR_GND), "GND");
  strings[3] = L_S(STR_PAGE_DLG, S_ID(IDS_DSO_CTR_DC), "DC");
  strings[4] = L_S(STR_PAGE_DLG, S_ID(IDS_DSO_CTR_AC), "AC");
  strings[5] = L_S(STR_PAGE_DLG, S_ID(IDS_DSO_CTR_AUTO), "AUTO");

  p.setPen(Qt::transparent);
  p.setBrush(chEn_rect.contains(pt) ? _colour.darker() : _colour);
  p.drawRect(chEn_rect);
  p.setPen(Qt::white);
  p.drawText(chEn_rect, Qt::AlignCenter | Qt::AlignVCenter,
             enabled() ? strings[0] : strings[1]);

  p.setPen(Qt::transparent);
  p.setBrush(enabled() ? (acdc_rect.contains(pt) ? _colour.darker() : _colour)
                       : foreBack);
  p.drawRect(acdc_rect);
  p.setPen(Qt::white);
  p.drawText(acdc_rect, Qt::AlignCenter | Qt::AlignVCenter,
             (_acCoupling == SR_GND_COUPLING)  ? strings[2]
             : (_acCoupling == SR_DC_COUPLING) ? strings[3]
                                               : strings[4]);

  if (_data_source->device()->is_hardware()) {
    p.setPen(Qt::transparent);
    p.setBrush(enabled() ? (auto_rect.contains(pt) ? _colour.darker() : _colour)
                         : foreBack);
    p.drawRect(auto_rect);
    p.setPen(Qt::white);
    p.drawText(auto_rect, Qt::AlignCenter | Qt::AlignVCenter, strings[5]);
  }

  // paint the probe factor selector
  uint64_t factor = get_factor();

  p.setPen(Qt::transparent);
  p.setBrush((enabled() && (factor == 100))
                 ? (x100_rect.contains(pt) ? _colour.darker() : _colour)
                 : (x100_rect.contains(pt) ? _colour.darker() : foreBack));
  p.drawRect(x100_rect);
  p.setBrush((enabled() && (factor == 10))
                 ? (x10_rect.contains(pt) ? _colour.darker() : _colour)
                 : (x10_rect.contains(pt) ? _colour.darker() : foreBack));
  p.drawRect(x10_rect);
  p.setBrush((enabled() && (factor == 1))
                 ? (x1_rect.contains(pt) ? _colour.darker() : _colour)
                 : (x1_rect.contains(pt) ? _colour.darker() : foreBack));
  p.drawRect(x1_rect);

  p.setPen(Qt::white);
  p.drawText(x100_rect, Qt::AlignCenter | Qt::AlignVCenter, "x100");
  p.drawText(x10_rect, Qt::AlignCenter | Qt::AlignVCenter, "x10");
  p.drawText(x1_rect, Qt::AlignCenter | Qt::AlignVCenter, "x1");

  p.setRenderHint(QPainter::Antialiasing, false);
}

void DsoSignal::paint_hover_measure(QPainter &p, QColor fore, QColor back) {
  _measure->paint_hover_measure(p, fore, back);
}

} // namespace view
} // namespace pv
