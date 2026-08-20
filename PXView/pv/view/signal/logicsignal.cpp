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

#include "pv/view/signal/logicsignal.h"
#include "pv/config/appconfig.h"
#include "pv/data/datasource.h"
#include "pv/data/pulse_analyzer.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/model/signalmodel.h"
#include "pv/base/pxvdef.h"
#include "pv/session/sigsession.h"
#include "pv/view/view.h"
#include "pv/view/renderer/rasterize.h"
#include <libsigrokdecode.h>
#include <cmath>

using namespace std;

namespace pv {
namespace view {

// const float LogicSignal::Oversampling = 2.0f;
const float LogicSignal::Oversampling = 1.0f;
const int LogicSignal::StateHeight = 12;
const int LogicSignal::StateRound = 5;

LogicSignal::LogicSignal(data::LogicSnapshot *data,
                         std::shared_ptr<data::SignalModel> model,
                         data::DataSource *data_source)
    : Signal(model, data_source), _data(data) {
  _trig = NONTRIG;
  _paint_align_sample_count = 0;

  QString heightStr =
      AppConfig::Instance().GetThemeTokenValue("@logic-channel-height");
  bool ok;
  int h = heightStr.toInt(&ok);
  if (ok && h > 0) {
    set_totalHeight(h);
  }
}

LogicSignal::LogicSignal(view::LogicSignal *s, data::LogicSnapshot *data,
                         std::shared_ptr<data::SignalModel> model,
                         data::DataSource *data_source)
    : Signal(*s, model, data_source), _data(data), _trig(s->get_trig()) {
  _paint_align_sample_count = 0;

  QString heightStr =
      AppConfig::Instance().GetThemeTokenValue("@logic-channel-height");
  bool ok;
  int h = heightStr.toInt(&ok);
  if (ok && h > 0) {
    set_totalHeight(h);
  }
}

LogicSignal *LogicSignal::clone() const {
  LogicSignal *cloned = new LogicSignal(const_cast<LogicSignal *>(this),
                                        nullptr, _model, _data_source);
  cloned->_local_enabled = _local_enabled;
  cloned->_visible = _visible;
  return cloned;
}

LogicSignal::~LogicSignal() {
  _cur_edges.clear();
  _cur_pulses.clear();
}

void LogicSignal::set_trig(int trig) {
  const int old_trig = static_cast<int>(_trig);

  if (trig > NONTRIG && trig <= EDGTRIG)
    _trig = (LogicSetRegions)trig;
  else
    _trig = NONTRIG;

  // R2: 实时写回 Core。SignalModel 是 trig_type 的 single source of truth。
  // 后续 SigSession::reload() 重建 SignalModel 时会从 old_model 保留 trig_type
  // (sigsession.cpp:1141)，确保触发状态在 reload 后不丢失。
  // 注意: SignalModel::set_trig_type 内部有 `if (_trig_type != trig_type)`
  // 保护，
  //       值相同时不 emit trig_type_changed，因此 apply_model_properties 调用
  //       本函数同步时不会产生递归。值不同时 emit 会回调本函数一次，但第二次
  //       进入时 model 值已更新，early return，递归终止（最多 1 层）。
  // Phase 2: use the injected _model directly instead of querying the session.
  if (_model)
    _model->set_trig_type(static_cast<int>(_trig));

  // Task 11.2: 仅当触发类型实际变化时才广播 SimpleTriggerChanged。
  // 这也阻断了 set_trig_type emit trig_type_changed
  // 回调本函数（重入）时的冗余广播： 重入时 _trig 已是目标值，old_trig ==
  // _trig，跳过广播，保证单次用户操作只广播一次。
  if (old_trig != static_cast<int>(_trig)) {
    // 通知 TriggerDock UI 同步触发类型显示
    if (_view) {
      _view->session().broadcast_async<interface::SimpleTriggerChanged>({});
    }
  }
}

bool LogicSignal::commit_trig() {
  // B2 fix: View layer must not call ds_trigger_* directly. The trigger state
  // was already written to SignalModel::set_trig_type() by set_trig(). The
  // actual libsigrok sync happens in SigSession::sync_trigger_to_libsigrok()
  // at capture start, which reads SignalModel::trig_type() and calls
  // ds_trigger_probe_set + ds_trigger_set_en for all logic channels at once.
  return _trig != NONTRIG;
}

void LogicSignal::paint_mid(QPainter &p, int left, int right, QColor fore,
                            QColor back, const PaintContext &ctx) {
  if (!_data)
    return;
  uint64_t ring_count = _data->get_ring_sample_count();
  if (ring_count == 0) {
    pxv_warn("LogicSignal::paint_mid: ring_sample_count==0, skipping paint");
    return;
  }
  uint64_t end_align_sample = ring_count - 1;
  paint_mid_align(p, left, right, fore, back, end_align_sample, ctx);
}

void LogicSignal::paint_mid_align_sample(QPainter &p, int left, int right,
                                         QColor fore, QColor back,
                                         uint64_t end_align_sample,
                                         const PaintContext &ctx) {
  paint_mid_align(p, left, right, fore, back, end_align_sample, ctx);
}

void LogicSignal::paint_mid_align(QPainter &p, int left, int right, QColor fore,
                                  QColor back, uint64_t end_align_sample,
                                  const PaintContext &ctx) {
  (void)back;

  if (!_data)
    return;
  assert(_view);

  // modernize-thread-model Task 2: the waveform + glitch-filter overlay logic
  // was extracted to the pure function rasterize_logic_channel() (see
  // pv/view/renderer/rasterize.h). It reads only the snapshot + value params
  // (all local buffers, no member mutation), so it is thread-safe and
  // directly unit-testable without a View. Pixel output is identical.
  //
  // The glitch LIVE preview (orange overlay) is GUI state read via
  // View::get_preview_ranges and passed in as GlitchRange values; the
  // already-filtered (red) overlay is read from the snapshot inside the
  // rasterizer.
  const int y = get_y() + _totalHeight * 0.5;
  const int64_t offset = ctx.offset;

  std::vector<GlitchRange> preview;
  const std::vector<GlitchRange> *preview_ptr = nullptr;
  if (_view) {
    if (const auto *pv_ranges = _view->get_preview_ranges(this);
        pv_ranges && !pv_ranges->empty()) {
      preview.reserve(pv_ranges->size());
      for (const auto &pulse : *pv_ranges)
        preview.push_back({pulse.start, pulse.end});
      preview_ptr = &preview;
    }
  }

  rasterize_logic_channel(p, _data, _model ? _model->index() : 0, left, right,
                          y, _totalHeight, _colour.isValid() ? _colour : fore,
                          ctx.scale, offset, end_align_sample, ctx,
                          preview_ptr);
}

void LogicSignal::paint_caps(QPainter &p, QLineF *const lines,
                             std::vector<pair<uint64_t, bool>> &edges,
                             bool level, double samples_per_pixel,
                             double pixels_offset, float x_offset,
                             float y_offset) {
  QLineF *line = lines;

  uint64_t curX = 0;
  uint64_t nxtX = 0;
  for (std::vector<pv::data::LogicSnapshot::EdgePair>::const_iterator i =
           edges.begin();
       i != (edges.end() - 1); i++)
    if ((*i).second == level) {
      curX = ((*i).first / samples_per_pixel - pixels_offset) + x_offset;
      nxtX = ((*(i + 1)).first / samples_per_pixel - pixels_offset) + x_offset;
      if (nxtX > curX)
        *line++ = QLineF(curX, y_offset, nxtX, y_offset);
    }

  p.drawLines(lines, line - lines);
}

void LogicSignal::paint_type_options(QPainter &p, int right, const QPoint pt,
                                     QColor fore) {
  int y = get_y();
  const QRectF posTrig_rect = get_rect(POSTRIG, y, right);
  const QRectF higTrig_rect = get_rect(HIGTRIG, y, right);
  const QRectF negTrig_rect = get_rect(NEGTRIG, y, right);
  const QRectF lowTrig_rect = get_rect(LOWTRIG, y, right);
  const QRectF edgeTrig_rect = get_rect(EDGTRIG, y, right);

  p.setPen(Qt::NoPen);

  if (true) {
    QColor color = View::Blue;

    if (_view && _view->session().is_loop_mode()) {
      color = QColor(0x70, 0x70, 0x70, 255);
    }

    p.setBrush(posTrig_rect.contains(pt) ? color.lighter()
               : (_trig == POSTRIG)      ? color
                                         : Qt::transparent);
    p.drawRect(posTrig_rect);
    p.setBrush(higTrig_rect.contains(pt) ? color.lighter()
               : (_trig == HIGTRIG)      ? color
                                         : Qt::transparent);
    p.drawRect(higTrig_rect);
    p.setBrush(negTrig_rect.contains(pt) ? color.lighter()
               : (_trig == NEGTRIG)      ? color
                                         : Qt::transparent);
    p.drawRect(negTrig_rect);
    p.setBrush(lowTrig_rect.contains(pt) ? color.lighter()
               : (_trig == LOWTRIG)      ? color
                                         : Qt::transparent);
    p.drawRect(lowTrig_rect);
    p.setBrush(edgeTrig_rect.contains(pt) ? color.lighter()
               : (_trig == EDGTRIG)       ? color
                                          : Qt::transparent);
    p.drawRect(edgeTrig_rect);
  }

  p.setPen(QPen(fore, 2, Qt::SolidLine));
  p.setBrush(Qt::transparent);
  p.drawLine(posTrig_rect.left() + 5, posTrig_rect.bottom() - 5,
             posTrig_rect.center().x(), posTrig_rect.bottom() - 5);
  p.drawLine(posTrig_rect.center().x(), posTrig_rect.bottom() - 5,
             posTrig_rect.center().x(), posTrig_rect.top() + 5);
  p.drawLine(posTrig_rect.center().x(), posTrig_rect.top() + 5,
             posTrig_rect.right() - 5, posTrig_rect.top() + 5);

  p.drawLine(higTrig_rect.left() + 5, higTrig_rect.top() + 5,
             higTrig_rect.right() - 5, higTrig_rect.top() + 5);

  p.drawLine(negTrig_rect.left() + 5, negTrig_rect.top() + 5,
             negTrig_rect.center().x(), negTrig_rect.top() + 5);
  p.drawLine(negTrig_rect.center().x(), negTrig_rect.top() + 5,
             negTrig_rect.center().x(), negTrig_rect.bottom() - 5);
  p.drawLine(negTrig_rect.center().x(), negTrig_rect.bottom() - 5,
             negTrig_rect.right() - 5, negTrig_rect.bottom() - 5);

  p.drawLine(lowTrig_rect.left() + 5, lowTrig_rect.bottom() - 5,
             lowTrig_rect.right() - 5, lowTrig_rect.bottom() - 5);

  p.drawLine(edgeTrig_rect.left() + 5, edgeTrig_rect.top() + 5,
             edgeTrig_rect.center().x() - 2, edgeTrig_rect.top() + 5);
  p.drawLine(edgeTrig_rect.center().x() + 2, edgeTrig_rect.top() + 5,
             edgeTrig_rect.right() - 5, edgeTrig_rect.top() + 5);
  p.drawLine(edgeTrig_rect.center().x(), edgeTrig_rect.top() + 7,
             edgeTrig_rect.center().x(), edgeTrig_rect.bottom() - 7);
  p.drawLine(edgeTrig_rect.left() + 5, edgeTrig_rect.bottom() - 5,
             edgeTrig_rect.center().x() - 2, edgeTrig_rect.bottom() - 5);
  p.drawLine(edgeTrig_rect.center().x() + 2, edgeTrig_rect.bottom() - 5,
             edgeTrig_rect.right() - 5, edgeTrig_rect.bottom() - 5);
}

bool LogicSignal::measure(const QPointF &p, uint64_t &index0, uint64_t &index1,
                          uint64_t &index2) {
  const float gap = abs(p.y() - get_y());
  if (gap < get_totalHeight() * 0.5) {
    if (!_data || _data->empty() ||
        !_data->has_data(_model ? _model->index() : 0)) {
      pxv_warn("LogicSignal::measure: no data or has_data false, skipping");
      return false;
    }

    const uint64_t ring_count = _data->get_ring_sample_count();
    if (ring_count == 0) {
      pxv_warn("LogicSignal: ring_sample_count==0, skipping edge/measure");
      return false;
    }
    const uint64_t end = ring_count - 1;
    uint64_t index =
        _data->samplerate() * _view->scale() * (_view->offset() + p.x());

    if (index > end) {
      return false;
    }

    bool sample = _data->get_sample(index, get_index());
    if (index == 0) {
      index0 = index;
    } else {
      index--;
      if (_data->get_pre_edge(index, sample, 1, get_index()))
        index0 = index;
      else
        index0 = 0;
    }

    sample = _data->get_sample(index, get_index());
    index++;
    if (_data->get_nxt_edge(index, sample, end, 1, get_index())) {
      index1 = index;
    } else {
      if (index0 == 0)
        return false;
      index1 = end + 1;
      index2 = 0;
      return true;
    }

    sample = _data->get_sample(index, get_index());
    index++;
    if (_data->get_nxt_edge(index, sample, end, 1, get_index()))
      index2 = index;
    else
      index2 = end + 1;

    return true;
  }
  return false;
}

bool LogicSignal::is_by_edge(const QPointF &p, uint64_t &index, int radius) {
  uint64_t pre_index, nxt_index;
  const float gap = abs(p.y() - get_y());

  if (gap < get_totalHeight() * 0.5) {
    if (!_data || _data->empty() ||
        !_data->has_data(_model ? _model->index() : 0)) {
      pxv_warn("LogicSignal::is_by_edge: no data or has_data false, skipping");
      return false;
    }

    const uint64_t ring_count = _data->get_ring_sample_count();
    if (ring_count == 0) {
      pxv_warn("LogicSignal: ring_sample_count==0, skipping edge/measure");
      return false;
    }
    const uint64_t end = ring_count - 1;
    const double pos =
        _data->samplerate() * _view->scale() * (_view->offset() + p.x());
    index = floor(pos + 0.5);
    if (index > end)
      return false;

    bool sample = _data->get_sample(index, get_index());
    if (index == 0)
      pre_index = index;
    else {
      index--;
      if (_data->get_pre_edge(index, sample, 1, get_index()))
        pre_index = index;
      else
        pre_index = 0;
    }

    sample = _data->get_sample(index, get_index());
    index++;
    if (_data->get_nxt_edge(index, sample, end, 1, get_index()))
      nxt_index = index;
    else
      nxt_index = 0;

    if (pre_index == 0 && nxt_index == 0)
      return false;

    if (pre_index > 0 && nxt_index > 0) {
      if (pos - pre_index > nxt_index - pos)
        index = nxt_index;
      else
        index = pre_index;
    } else {
      index = pre_index > 0 ? pre_index : nxt_index;
    }

    if (radius > abs((index - pos) / _view->scale() / _data->samplerate()))
      return true;
  }
  return false;
}

bool LogicSignal::edge(const QPointF &p, uint64_t &index, int radius) {
  uint64_t pre_index, nxt_index;
  const float gap = abs(p.y() - get_y());

  if (gap < get_totalHeight() * 0.5) {
    if (!_data || _data->empty() ||
        !_data->has_data(_model ? _model->index() : 0)) {
      pxv_warn("LogicSignal::edge: no data or has_data false, skipping");
      return false;
    }

    const uint64_t ring_count = _data->get_ring_sample_count();
    if (ring_count == 0) {
      pxv_warn("LogicSignal: ring_sample_count==0, skipping edge/measure");
      return false;
    }
    const uint64_t end = ring_count - 1;
    const double pos =
        _data->samplerate() * _view->scale() * (_view->offset() + p.x());
    index = floor(pos + 0.5);
    if (index > end)
      return false;

    bool sample = _data->get_sample(index, get_index());
    if (index == 0)
      pre_index = index;
    else {
      index--;
      if (_data->get_pre_edge(index, sample, 1, get_index()))
        pre_index = index;
      else
        pre_index = 0;
    }

    sample = _data->get_sample(index, get_index());
    index++;
    if (_data->get_nxt_edge(index, sample, end, 1, get_index()))
      nxt_index = index;
    else
      nxt_index = 0;

    if (pre_index == 0 || nxt_index == 0)
      return false;

    if (pos - pre_index > nxt_index - pos)
      index = nxt_index;
    else
      index = pre_index;

    if (radius > abs((index - pos) / _view->scale() / _data->samplerate()))
      return true;
  }
  return false;
}

bool LogicSignal::edges(const QPointF &p, uint64_t start, uint64_t &rising,
                        uint64_t &falling) {
  uint64_t end;
  const float gap = abs(p.y() - get_y());
  if (gap < get_totalHeight() * 0.5) {
    if (!_data)
      return false;
    end = _data->samplerate() * _view->scale() * (_view->offset() + p.x());
    return edges(end, start, rising, falling);
  }
  return false;
}

bool LogicSignal::edges(uint64_t end, uint64_t start, uint64_t &rising,
                        uint64_t &falling) {
  if (!_data || _data->empty() ||
      !_data->has_data(_model ? _model->index() : 0))
    return false;

  uint64_t index = min(start, end);
  const uint64_t sample_count = _data->get_ring_sample_count();
  end = max(start, end);
  start = index;
  if (end > (sample_count - 1))
    return false;

  const int ch_index = get_index();
  bool sample = _data->get_sample(start, ch_index);

  rising = 0;
  falling = 0;
  do {
    if (_data->get_nxt_edge(index, sample, sample_count, 1, ch_index)) {
      if (index > end)
        break;
      rising += !sample;
      falling += sample;
      sample = !sample;
    } else {
      break;
    }
  } while (index <= end);

  return true;
}

bool LogicSignal::mouse_press(int right, const QPoint pt) {
  int y = get_y();
  const QRectF posTrig = get_rect(POSTRIG, y, right);
  const QRectF higTrig = get_rect(HIGTRIG, y, right);
  const QRectF negTrig = get_rect(NEGTRIG, y, right);
  const QRectF lowTrig = get_rect(LOWTRIG, y, right);
  const QRectF edgeTrig = get_rect(EDGTRIG, y, right);

  if (posTrig.contains(pt))
    set_trig((_trig == POSTRIG) ? NONTRIG : POSTRIG);
  else if (higTrig.contains(pt))
    set_trig((_trig == HIGTRIG) ? NONTRIG : HIGTRIG);
  else if (negTrig.contains(pt))
    set_trig((_trig == NEGTRIG) ? NONTRIG : NEGTRIG);
  else if (lowTrig.contains(pt))
    set_trig((_trig == LOWTRIG) ? NONTRIG : LOWTRIG);
  else if (edgeTrig.contains(pt))
    set_trig((_trig == EDGTRIG) ? NONTRIG : EDGTRIG);
  else
    return false;

  return true;
}

QRectF LogicSignal::get_rect(LogicSetRegions type, int y, int right) {
  const QSizeF name_size(right - get_leftWidth() - get_rightWidth(),
                         SquareWidth);

  if (type == POSTRIG)
    return QRectF(get_leftWidth() + name_size.width() + Margin,
                  y - SquareWidth / 2.0, SquareWidth, SquareWidth);
  else if (type == HIGTRIG)
    return QRectF(get_leftWidth() + name_size.width() + SquareWidth + Margin,
                  y - SquareWidth / 2.0, SquareWidth, SquareWidth);
  else if (type == NEGTRIG)
    return QRectF(get_leftWidth() + name_size.width() + 2 * SquareWidth +
                      Margin,
                  y - SquareWidth / 2, SquareWidth, SquareWidth);
  else if (type == LOWTRIG)
    return QRectF(get_leftWidth() + name_size.width() + 3 * SquareWidth +
                      Margin,
                  y - SquareWidth / 2, SquareWidth, SquareWidth);
  else if (type == EDGTRIG)
    return QRectF(get_leftWidth() + name_size.width() + 4 * SquareWidth +
                      Margin,
                  y - SquareWidth / 2, SquareWidth, SquareWidth);
  else
    return QRectF(0, 0, 0, 0);
}

void LogicSignal::paint_mark(QPainter &p, int xstart, int xend, int type, int edge_dir) {
  const int ypos = get_y();
  const int msize = 3;
  if (ypos < -10000 || ypos > 10000)
    return;
  if (xstart < -10000 || xstart > 10000)
    return;
  p.setPen(p.brush().color());
  if (type == SRD_CHANNEL_SDATA) {
    p.drawEllipse(QPoint(xstart, ypos), msize, msize);
  } else if (type == SRD_CHANNEL_SCLK) {
    const QPoint triangle[] = {
        QPoint(xstart, ypos - 2),     QPoint(xstart - 1, ypos - 1),
        QPoint(xstart, ypos - 1),     QPoint(xstart + 1, ypos - 1),
        QPoint(xstart - 2, ypos),     QPoint(xstart - 1, ypos),
        QPoint(xstart, ypos),         QPoint(xstart + 1, ypos),
        QPoint(xstart + 2, ypos),     QPoint(xstart - 3, ypos + 1),
        QPoint(xstart - 2, ypos + 1), QPoint(xstart - 1, ypos + 1),
        QPoint(xstart, ypos + 1),     QPoint(xstart + 1, ypos + 1),
        QPoint(xstart + 2, ypos + 1), QPoint(xstart + 3, ypos + 1),
    };
    p.drawPoints(triangle, 16);
  } else if (type == SRD_CHANNEL_ADATA) {
    int mid = (xstart + xend) / 2;
    if (mid < -10000 || mid > 10000)
      return;
    p.drawEllipse(QPoint(mid, ypos), msize, msize);
  }

  // Rising/falling edge triangle markers (edge_dir: 1=rising, -1=falling, 0=none)
  if (edge_dir != 0) {
    const int ts = 5;  // triangle size
    if (edge_dir > 0) {
      // Rising edge: upward-pointing triangle
      const QPoint tri[] = {
          QPoint(xstart, ypos - ts),
          QPoint(xstart - ts, ypos + ts),
          QPoint(xstart + ts, ypos + ts),
      };
      p.drawPolygon(tri, 3);
    } else {
      // Falling edge: downward-pointing triangle
      const QPoint tri[] = {
          QPoint(xstart, ypos + ts),
          QPoint(xstart - ts, ypos - ts),
          QPoint(xstart + ts, ypos - ts),
      };
      p.drawPolygon(tri, 3);
    }
  }
}

void LogicSignal::set_data(data::LogicSnapshot *data) { _data = data; _data_ref.reset(); }

void LogicSignal::set_data_from_source(data::DataSource *source) {
    _data_ref = source ? source->get_logic_snapshot_shared() : nullptr;
    _data = source ? source->get_logic_snapshot() : nullptr;
}

void LogicSignal::clear_data() { _data = nullptr; _data_ref.reset(); }

} // namespace view
} // namespace pv
