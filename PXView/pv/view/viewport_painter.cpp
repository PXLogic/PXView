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

#include "viewport_painter.h"
#include "viewport.h"
#include "ruler.h"
#include "viewstatus.h"

#include "../data/logicsnapshot.h"
#include "../dialogs/dsomeasure.h"
#include "../sigsession.h"
#include "analogsignal.h"
#include "decodetrace.h"
#include "dsosignal.h"
#include "logicsignal.h"
#include "signal.h"
#include "spectrumtrace.h"

#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyleOption>
#include <QWheelEvent>
#include <math.h>
#include <set>

#include "../config/appconfig.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"
#include "lissajoustrace.h"
#include "mathtrace.h"

using namespace std;

namespace pv {
namespace view {

struct BrutalStyle {
  QColor bg;
  QColor text;
};

static BrutalStyle getBrutalStyle(const QColor &back, const QColor &panelBg,
                                  const QColor &panelText) {
  double luminance =
      (back.red() * 0.299 + back.green() * 0.587 + back.blue() * 0.114);
  bool isDark = luminance < 128;

  if (isDark) {
    return {panelBg, panelText};
  } else {
    return {panelText, panelBg};
  }
}

static void drawFloatingPanel(QPainter &p, const QPointF &cursorPos,
                              double viewWidth, double viewHeight,
                              const QColor &back, const QColor &panelBg,
                              const QColor &panelText,
                              const vector<pair<QString, QString>> &rows) {
  BrutalStyle style = getBrutalStyle(back, panelBg, panelText);

  QFont labelFont = p.font();
  labelFont.setPixelSize(floating_panel_font_label_size());
  labelFont.setWeight(QFont::Black);
  labelFont.setCapitalization(QFont::AllUppercase);
  labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
  apply_global_font_strategy(labelFont);

  QFont valueFont = p.font();
  valueFont.setPixelSize(floating_panel_font_value_size());
  valueFont.setWeight(QFont::Black);
  valueFont.setFamily("Space Mono, Courier New, monospace");
  apply_global_font_strategy(valueFont);

  QFontMetrics fmLabel(labelFont);
  QFontMetrics fmValue(valueFont);

  const int pad = 14;
  const int gridGapH = 14;
  const int gridGapV = 10;
  const int labelValueGap = 2;

  bool hasLabels = false;
  for (const auto &row : rows) {
    if (!row.first.isEmpty()) {
      hasLabels = true;
      break;
    }
  }

  int cols = (hasLabels && rows.size() >= 2) ? 2 : 1;
  int gridRows = ((int)rows.size() + cols - 1) / cols;

  int cellH = fmLabel.height() + labelValueGap + fmValue.height();
  int cellH_noLabel = fmValue.height();

  int colWidths[2] = {0, 0};
  for (size_t i = 0; i < rows.size(); i++) {
    int col = (int)i % cols;
    QString cleanLabel = rows[i].first.trimmed().toUpper();
    if (cleanLabel.endsWith(':'))
      cleanLabel.chop(1);
    int labelW =
        cleanLabel.isEmpty() ? 0 : fmLabel.horizontalAdvance(cleanLabel);

    QString val = rows[i].second;
    if (val.startsWith('+'))
      val.remove(0, 1);
    int valW = fmValue.horizontalAdvance(val);

    colWidths[col] = qMax(colWidths[col], qMax(labelW, valW));
  }

  double panelW, panelH;
  if (cols == 2)
    panelW = pad * 2 + colWidths[0] + gridGapH + colWidths[1];
  else
    panelW = pad * 2 + colWidths[0];

  int usedCellH = hasLabels ? cellH : cellH_noLabel;
  panelH = pad * 2 + gridRows * usedCellH + (gridRows - 1) * gridGapV;

  const double offsetX = 15, offsetY = 20;
  double px = cursorPos.x() + offsetX;
  double py = cursorPos.y() + offsetY;
  if (px + panelW > viewWidth)
    px = cursorPos.x() - panelW - offsetX;
  if (py + panelH > viewHeight)
    py = cursorPos.y() - panelH - offsetY;

  QRectF panelRect(px, py, panelW, panelH);

  p.setRenderHint(QPainter::Antialiasing, false);

  p.setPen(Qt::NoPen);
  p.setBrush(style.bg);
  p.drawRect(panelRect);

  double y = panelRect.top() + pad;
  for (size_t i = 0; i < rows.size(); i++) {
    int col = (int)i % cols;
    int row = (int)i / cols;

    double cellX = panelRect.left() + pad + col * (colWidths[0] + gridGapH);
    double cellY = y + row * (usedCellH + gridGapV);

    QString cleanLabel = rows[i].first.trimmed();
    if (cleanLabel.endsWith(':') || cleanLabel.endsWith(QChar(0xFF1A)))
      cleanLabel.chop(1);
    cleanLabel = cleanLabel.trimmed();

    if (!cleanLabel.isEmpty()) {
      p.setFont(labelFont);
      p.setPen(style.text);
      QString upperLabel = cleanLabel.toUpper();
      double labelY = cellY + fmLabel.ascent();
      p.drawText(QPointF(cellX, labelY), upperLabel);
    }

    p.setFont(valueFont);
    p.setPen(style.text);
    double valueY = cleanLabel.isEmpty() ? cellY + fmValue.ascent()
                                         : cellY + fmLabel.height() +
                                               labelValueGap + fmValue.ascent();

    QString valText = rows[i].second;
    if (valText.startsWith('+'))
      valText.remove(0, 1);
    p.drawText(QPointF(cellX, valueY), valText);
  }
}

ViewportPainter::ViewportPainter(Viewport *viewport) : _viewport(viewport) {}

ViewportPainter::~ViewportPainter() {}

void ViewportPainter::paintEvent(QPaintEvent *event) {
  if (_viewport->g_drag_active && !_viewport->g_drag_snapshot.isNull()) {
    QPainter p(_viewport);
    p.drawPixmap(0, 0, _viewport->g_drag_snapshot);
    return;
  }

  (void)event;

  _viewport->_paint_in_this_second++;
  if (_viewport->_is_idle || !_viewport->_frame_interval_timer.isValid()) {
    _viewport->_frame_interval_timer.restart();
    _viewport->_is_idle = false;
  } else {
    int elapsed =
        static_cast<int>(_viewport->_frame_interval_timer.restart());
    if (elapsed > _viewport->_max_frame_time) {
      _viewport->_max_frame_time = elapsed;
    }
  }

  doPaint(event->rect());
}

void ViewportPainter::doPaint(const QRect & /* dirtyRect */) {
  using pv::view::Signal;
#ifndef NDEBUG
  QElapsedTimer timer;
  timer.start();
#endif

  QStyleOption o;
  o.initFrom(_viewport);
  QPainter p(_viewport);
  _viewport->style()->drawPrimitive(QStyle::PE_Widget, &o, &p, _viewport);

  QFont font = theme_font_cursor();
  p.setFont(font);

#ifndef NDEBUG
  qint64 t_init = timer.elapsed();
#endif

#ifndef NDEBUG
  QElapsedTimer checkUpdateTimer;
  checkUpdateTimer.start();
#endif
  _viewport->_view.session().check_update();
#ifndef NDEBUG
  qint64 t_check_update = checkUpdateTimer.elapsed();
#endif

  QColor fore(_viewport->palette().color(_viewport->foregroundRole()));
  QColor back(_viewport->palette().color(_viewport->backgroundRole()));
  fore.setAlpha(View::ForeAlpha);
  _viewport->_view.set_back(false);

  std::vector<Trace *> traces;
  _viewport->_view.get_traces(_viewport->_type, traces);
#ifndef NDEBUG
  qint64 t_get_traces = timer.elapsed() - (t_init + t_check_update);
#endif

  p.save();
  p.translate(0, -_viewport->_view.get_vOffset());

#ifndef NDEBUG
  qint64 t_group_cards = 0;
#endif
  if (_viewport->_type == TIME_VIEW &&
      _viewport->_view.is_logic_rendering_mode()) {
#ifndef NDEBUG
    QElapsedTimer groupTimer;
    groupTimer.start();
#endif
    const auto &groups = _viewport->_view.get_signal_groups();
    if (!groups.empty()) {
      std::vector<size_t> group_indices(groups.size());
      for (size_t i = 0; i < groups.size(); i++)
        group_indices[i] = i;
      std::sort(group_indices.begin(), group_indices.end(),
                [&groups](size_t a, size_t b) {
                  if (groups[a].traces.empty())
                    return false;
                  if (groups[b].traces.empty())
                    return true;
                  return groups[a].traces[0]->get_v_offset() <
                         groups[b].traces[0]->get_v_offset();
                });

      for (size_t idx = 0; idx < group_indices.size(); idx++) {
        const auto &group = groups[group_indices[idx]];
        if (group.traces.empty())
          continue;
        double groupTop = 1e9;
        double groupBottom = -1e9;
        for (auto gt : group.traces) {
          double traceTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 -
                            View::SignalMargin;
          double traceBottom = gt->get_v_offset() +
                               gt->get_totalHeight() * 0.5 + View::SignalMargin;
          groupTop = min(groupTop, traceTop);
          groupBottom = max(groupBottom, traceBottom);
        }

        double cardTop = groupTop - View::GroupGap * 0.5;
        double cardHeight = groupBottom - groupTop + View::GroupGap;

        QRectF cardRect(-View::GroupCardRadius, cardTop,
                        _viewport->width() + View::GroupCardRadius + 1,
                        cardHeight);
        QPainterPath groupPath;
        groupPath.addRoundedRect(cardRect, View::GroupCardRadius,
                                 View::GroupCardRadius);

        if (_viewport->_view.is_colored_card_mode()) {
          p.save();
          p.setClipPath(groupPath);
          p.setPen(Qt::NoPen);

          for (size_t i = 0; i < group.traces.size(); i++) {
            auto gt = group.traces[i];
            double tTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 -
                          View::SignalMargin;
            double tBottom = gt->get_v_offset() + gt->get_totalHeight() * 0.5 +
                             View::SignalMargin;

            if (i == 0)
              tTop -= View::GroupGap * 0.5;
            if (i == group.traces.size() - 1)
              tBottom += View::GroupGap * 0.5;

            QRectF traceRect(-View::GroupCardRadius, tTop,
                             _viewport->width() + View::GroupCardRadius + 1,
                             tBottom - tTop);
            p.setBrush(_viewport->_view.get_trace_card_color(gt));
            p.drawRect(traceRect);
          }
          p.restore();
        } else {
          p.setPen(Qt::NoPen);
          p.setBrush(_viewport->_view.get_group_card_color());
          p.drawPath(groupPath);
        }
      }
    }
#ifndef NDEBUG
    t_group_cards = groupTimer.elapsed();
#endif
  }

#ifndef NDEBUG
  qint64 t_dividers = 0;
  QElapsedTimer dividerTimer;
  dividerTimer.start();
#endif
  QColor dividerColor =
      AppConfig::Instance().GetThemeColor("@border-strong");
  if (!dividerColor.isValid()) {
    double lum =
        back.red() * 0.299 + back.green() * 0.587 + back.blue() * 0.114;
    dividerColor =
        lum < 128 ? QColor(0x37, 0x37, 0x3b) : QColor(0xd5, 0xd5, 0xd5);
  }

  std::set<Trace *> lastInGroup;
  if (_viewport->_type == TIME_VIEW &&
      _viewport->_view.is_logic_rendering_mode()) {
    const auto &groups = _viewport->_view.get_signal_groups();
    for (const auto &group : groups) {
      if (group.traces.empty())
        continue;
      Trace *last = nullptr;
      for (auto gt : group.traces) {
        if (gt->enabled())
          last = gt;
      }
      if (last)
        lastInGroup.insert(last);
    }
  }

  // Find the last enabled trace (no divider below it)
  Trace *lastEnabledTrace = nullptr;
  for (auto it = traces.rbegin(); it != traces.rend(); ++it) {
    if ((*it)->enabled() || dynamic_cast<DsoSignal *>(*it)) {
      lastEnabledTrace = *it;
      break;
    }
  }

  p.setPen(QPen(dividerColor, 1));
  for (auto t : traces) {
    if (!t->enabled() && !dynamic_cast<DsoSignal *>(t))
      continue;
    if (lastInGroup.count(t))
      continue;
    if (t == lastEnabledTrace)
      continue;
    int traceBottom =
        t->get_v_offset() + t->get_totalHeight() / 2 + View::SignalMargin;
    p.drawLine(0, traceBottom, _viewport->_view.get_view_width(),
               traceBottom);
  }
#ifndef NDEBUG
  t_dividers = dividerTimer.elapsed();
#endif

#ifndef NDEBUG
  qint64 t_paint_back = 0;
  QElapsedTimer backTimer;
  backTimer.start();
#endif
  for (auto t : traces) {
    if (!t->enabled() && !dynamic_cast<DsoSignal *>(t))
      continue;
    t->paint_back(p, 0, _viewport->_view.get_view_width(), fore, back);
    if (_viewport->_view.back_ready())
      break;
  }
#ifndef NDEBUG
  t_paint_back = backTimer.elapsed();
#endif

  p.restore();

#ifndef NDEBUG
  qint64 t_paint_signals = 0;
  QElapsedTimer signalsTimer;
  signalsTimer.start();
#endif
  if (_viewport->_view.is_logic_rendering_mode() ||
      _viewport->_view.session().is_instant()) {
    if (_viewport->_view.session().is_init_status()) {
      paintCursors(p);
    } else if (_viewport->_view.session().is_stopped_status()) {
      paintSignals(p, fore, back);
    } else if (_viewport->_view.session().is_realtime_refresh()) {
      _viewport->_view.session().have_new_realtime_refresh(false);

      if (_viewport->_view.session().have_view_data() ||
          _viewport->_view.session().is_instant())
        paintSignals(p, fore, back);
      else
        paintProgress(p, fore, back);
    } else if (_viewport->_view.session().is_running_status()) {
      if (_viewport->_view.session().is_repeat_mode()) {
        paintSignals(p, fore, back);

        if (!_viewport->_transfer_started) {
          bool triggered;
          int captured_progress;

          if (_viewport->_view.session().get_capture_status(
                  triggered, captured_progress)) {
            _viewport->_view.show_captured_progress(triggered,
                                                    captured_progress);
          }
        }
      } else if (_viewport->_type == TIME_VIEW) {
        _viewport->_view.repeat_unshow();
        paintProgress(p, fore, back);
      }
    }
  } else {
    paintSignals(p, fore, back);
  }
#ifndef NDEBUG
  t_paint_signals = signalsTimer.elapsed();
#endif

#ifndef NDEBUG
  qint64 t_paint_fore = 0;
  QElapsedTimer foreTimer;
  foreTimer.start();
#endif
  p.save();
  p.translate(0, -_viewport->_view.get_vOffset());
  for (auto t : traces) {
    if (t->enabled())
      t->paint_fore(p, 0, _viewport->_view.get_view_width(), fore, back);
  }
  p.restore();
#ifndef NDEBUG
  t_paint_fore = foreTimer.elapsed();
#endif

  if (_viewport->_view.get_signalHeight() != _viewport->_curSignalHeight)
    _viewport->_curSignalHeight = _viewport->_view.get_signalHeight();

  p.end();
}

void ViewportPainter::paintCursors(QPainter &p) {
  const QRect xrect = _viewport->_view.get_view_rect();
  auto &cursor_list = _viewport->_view.get_cursorList();

  if (_viewport->_view.cursors_shown() && _viewport->_type == TIME_VIEW) {

    for (auto cursor : cursor_list) {
      const int64_t cursorX = _viewport->_view.index2pixel(cursor->index());
      if (xrect.contains(_viewport->_view.hover_point().x(),
                         _viewport->_view.hover_point().y()) &&
          qAbs(cursorX - _viewport->_view.hover_point().x()) <=
              Viewport::HitCursorMargin)
        cursor->paint(p, xrect, 1,
                      _viewport->_view.session().is_stopped_status());
      else
        cursor->paint(p, xrect, 0,
                      _viewport->_view.session().is_stopped_status());
    }
  }
}

void ViewportPainter::paintSignals(QPainter &p, QColor fore, QColor back) {
#ifndef NDEBUG
  QElapsedTimer timer;
  timer.start();
  qint64 t_rebuild = 0, t_blit = 0, t_decode = 0, t_cursor = 0, t_xcursor = 0,
         t_marker = 0, t_measure = 0;
#endif

  std::vector<Trace *> traces;
  _viewport->_view.get_traces(_viewport->_type, traces);
  std::list<int> _index_list;

  bool rebuilt = false;

  if (_viewport->_view.is_logic_rendering_mode()) {
    // Determine if view parameters changed (requires full logic signal rebuild)
    bool view_params_changed =
        (_viewport->_view.scale() != _viewport->_curScale ||
         _viewport->_view.offset() != _viewport->_curOffset ||
         _viewport->_view.get_signalHeight() != _viewport->_curSignalHeight ||
         _viewport->_view.get_vOffset() != _viewport->_curVOffset);

    const qreal dpr = _viewport->devicePixelRatioF();
    const QSize pixmapSize = (QSizeF(_viewport->size()) * dpr).toSize();
    const bool pixmap_changed =
        _viewport->_pixmap.isNull() ||
        _viewport->_pixmap.size() != pixmapSize ||
        !qFuzzyCompare(_viewport->_pixmap.devicePixelRatioF(), dpr);

    if (view_params_changed || _viewport->_need_update || pixmap_changed) {
      rebuilt = true;
      (void)rebuilt;
#ifndef NDEBUG
      QElapsedTimer rebuildTimer;
      rebuildTimer.start();
#endif

      _viewport->_curScale = _viewport->_view.scale();
      _viewport->_curOffset = _viewport->_view.offset();
      _viewport->_curSignalHeight = _viewport->_view.get_signalHeight();
      _viewport->_curVOffset = _viewport->_view.get_vOffset();

      _viewport->_pixmap = QPixmap(pixmapSize);
      _viewport->_pixmap.setDevicePixelRatio(dpr);
      _viewport->_pixmap.fill(Qt::transparent);

      QPainter dbp(&_viewport->_pixmap);
      dbp.translate(0, -_viewport->_view.get_vOffset());

      bool bFirst = true;
      uint64_t end_align_sample = 0;

      for (auto t : traces) {
        if (t->enabled()) {
          _index_list = t->get_index_list();
          int idx = *_index_list.begin() % 8;
          QString token = QString("@logic-channel-%1").arg(idx);
          QColor color = AppConfig::Instance().GetThemeColor(token);
          if (!color.isValid()) {
            color = Viewport::PROBE_COLORS[idx];
          }
          if (t->signal_type() == SR_CHANNEL_LOGIC) {
            LogicSignal *logic_signal = (LogicSignal *)t;
            if (bFirst && logic_signal->data())
              end_align_sample =
                  logic_signal->data()->get_ring_sample_count();
            logic_signal->paint_mid_align_sample(
                dbp, 0, t->get_view_rect().right(), color, back,
                end_align_sample);
            bFirst = false;
          } else if (t->signal_type() != SR_CHANNEL_DECODER) {
            // Non-logic, non-decoder traces go into the cached pixmap
            t->paint_mid(dbp, 0, t->get_view_rect().right(), fore, back);
          }
        }
      }
      _viewport->_need_update = false;
#ifndef NDEBUG
      t_rebuild = rebuildTimer.elapsed();
#endif
    }

    // 1. Blit the cached logic signal pixmap (cheap: just a memcpy)
#ifndef NDEBUG
    QElapsedTimer blitTimer;
    blitTimer.start();
#endif
    p.drawPixmap(0, 0, _viewport->_pixmap);
#ifndef NDEBUG
    t_blit = blitTimer.elapsed();
#endif

    // 2. Paint decode traces directly on the widget (not via QPixmap).
    //    Rendering text into a QPixmap forces grayscale antialiasing
    //    even when the font strategy requests no antialiasing, because
    //    the raster paint engine on an offscreen surface ignores the
    //    TextAntialiasing hint.  Painting directly on the QWidget
    //    respects the hint, producing crisp pixel-aligned text.
    //    Logic-signal waveforms are still cached in _pixmap because
    //    lines are not affected by the antialiasing difference.
    {
      p.save();
      p.translate(0, -_viewport->_view.get_vOffset());

      QFont dfont = theme_font_trace_label();
      p.setFont(dfont);

      for (auto t : traces) {
        if (t->enabled() && t->signal_type() == SR_CHANNEL_DECODER) {
          t->paint_mid(p, 0, t->get_view_rect().right(), fore, back);
        }
      }
      p.restore();
    }
  } else {
    const qreal dpr = _viewport->devicePixelRatioF();
    const QSize pixmapSize = (QSizeF(_viewport->size()) * dpr).toSize();
    const bool pixmap_changed =
        _viewport->_pixmap.isNull() ||
        _viewport->_pixmap.size() != pixmapSize ||
        !qFuzzyCompare(_viewport->_pixmap.devicePixelRatioF(), dpr);

    if (_viewport->_view.scale() != _viewport->_curScale ||
        _viewport->_view.offset() != _viewport->_curOffset ||
        _viewport->_view.get_signalHeight() != _viewport->_curSignalHeight ||
        _viewport->_view.get_vOffset() != _viewport->_curVOffset ||
        _viewport->_need_update || pixmap_changed) {

      rebuilt = true;
#ifndef NDEBUG
      QElapsedTimer rebuildTimer;
      rebuildTimer.start();
#endif

      _viewport->_curScale = _viewport->_view.scale();
      _viewport->_curOffset = _viewport->_view.offset();
      _viewport->_curSignalHeight = _viewport->_view.get_signalHeight();
      _viewport->_curVOffset = _viewport->_view.get_vOffset();

      _viewport->_pixmap = QPixmap(pixmapSize);
      _viewport->_pixmap.setDevicePixelRatio(dpr);
      _viewport->_pixmap.fill(Qt::transparent);

      QPainter dbp(&_viewport->_pixmap);
      dbp.translate(0, -_viewport->_view.get_vOffset());

      bool isLissa = false;

      if (_viewport->_view.get_work_mode() == DSO) {
        auto lis_trace = _viewport->_view.get_own_lissajous_trace();
        if (lis_trace && lis_trace->enabled()) {
          isLissa = true;
        }
      }

      for (auto t : traces) {
        if (t->enabled()) {
          if (isLissa && t->signal_type() == SR_CHANNEL_DSO)
            continue;
          if (isLissa && t->signal_type() == SR_CHANNEL_MATH)
            continue;

          t->paint_mid(dbp, 0, t->get_view_rect().right(), fore, back);
        }
      }
      _viewport->_need_update = false;
#ifndef NDEBUG
      t_rebuild = rebuildTimer.elapsed();
#endif
    }
#ifndef NDEBUG
    QElapsedTimer blitTimer;
    blitTimer.start();
#endif
    p.drawPixmap(0, 0, _viewport->_pixmap);
#ifndef NDEBUG
    t_blit = blitTimer.elapsed();
#endif
  }

  // plot cursors
#ifndef NDEBUG
  QElapsedTimer cursorTimer;
  cursorTimer.start();
#endif
  paintCursors(p);
#ifndef NDEBUG
  t_cursor = cursorTimer.elapsed();
#endif

  const QRect xrect = _viewport->_view.get_view_rect();

  if (_viewport->_view.xcursors_shown() && _viewport->_type == TIME_VIEW) {
#ifndef NDEBUG
    QElapsedTimer xcursorTimer;
    xcursorTimer.start();
#endif
    auto &xcursor_list = _viewport->_view.get_xcursorList();
    auto i = xcursor_list.begin();
    int index = 0;
    bool hovered = false;

    while (i != xcursor_list.end()) {
      const double cursorX =
          xrect.left() + (*i)->value(XCursor::XCur_Y) * xrect.width();
      const double cursorY0 =
          xrect.top() + (*i)->value(XCursor::XCur_X0) * xrect.height();
      const double cursorY1 =
          xrect.top() + (*i)->value(XCursor::XCur_X1) * xrect.height();

      if (!hovered &&
          ((*i)->get_close_rect(xrect).contains(
              _viewport->_view.hover_point()) ||
           (*i)->get_map_rect(xrect).contains(
               _viewport->_view.hover_point()))) {
        (*i)->paint(p, xrect, XCursor::XCur_All);
        hovered = true;
      } else if (!hovered && xrect.contains(_viewport->_view.hover_point())) {
        if (qAbs(cursorX - _viewport->_view.hover_point().x()) <=
                Viewport::HitCursorMargin &&
            _viewport->_view.hover_point().y() > min(cursorY0, cursorY1) &&
            _viewport->_view.hover_point().y() < max(cursorY0, cursorY1)) {
          (*i)->paint(p, xrect, XCursor::XCur_Y);
          hovered = true;
        } else if (qAbs(cursorY0 - _viewport->_view.hover_point().y()) <=
                   Viewport::HitCursorMargin) {
          (*i)->paint(p, xrect, XCursor::XCur_X0);
          hovered = true;
        } else if (qAbs(cursorY1 - _viewport->_view.hover_point().y()) <=
                   Viewport::HitCursorMargin) {
          (*i)->paint(p, xrect, XCursor::XCur_X1);
          hovered = true;
        } else {
          (*i)->paint(p, xrect, XCursor::XCur_None);
        }
      } else {
        (*i)->paint(p, xrect, XCursor::XCur_None);
      }

      i++;
      index++;
    }
#ifndef NDEBUG
    t_xcursor = xcursorTimer.elapsed();
#endif
  }

  if (_viewport->_type == TIME_VIEW) {
#ifndef NDEBUG
    QElapsedTimer markerTimer;
    markerTimer.start();
#endif
    if (_viewport->_view.trig_cursor_shown()) {
      _viewport->_view.get_trig_cursor()->paint(p, xrect, 0, false);
    }
    if (_viewport->_view.search_cursor_shown()) {
      const int64_t searchX = _viewport->_view.index2pixel(
          _viewport->_view.get_search_cursor()->index());
      if (xrect.contains(_viewport->_view.hover_point().x(),
                         _viewport->_view.hover_point().y()) &&
          qAbs(searchX - _viewport->_view.hover_point().x()) <=
              Viewport::HitCursorMargin)
        _viewport->_view.get_search_cursor()->paint(p, xrect, 1, -1);
      else
        _viewport->_view.get_search_cursor()->paint(p, xrect, 0, -1);
    }
#ifndef NDEBUG
    t_marker = markerTimer.elapsed();
#endif

    // plot zoom rect
    if (_viewport->_action_type == LOGIC_ZOOM) {
      p.setPen(Qt::NoPen);
      p.setBrush(View::LightBlue);
      p.drawRect(
          QRectF(_viewport->_mouse_down_point, _viewport->_mouse_point));
    }

    // plot measure arrow
#ifndef NDEBUG
    QElapsedTimer measureTimer;
    measureTimer.start();
#endif
    paintMeasure(p, fore, back);
#ifndef NDEBUG
    t_measure = measureTimer.elapsed();
#endif

    // plot trigger information
    auto *dev = _viewport->_view.data_source()->device();
    if (_viewport->_view.get_work_mode() == DSO &&
        _viewport->_view.session().is_running_status() && dev &&
        dev->is_dsl_device()) {
      int type;
      bool roll = false;
      QString type_str = "";
      bool ret = false;

      dev->get_config_bool(SR_CONF_ROLL, roll);

      ret = dev->get_config_byte(SR_CONF_TRIGGER_SOURCE, type);
      if (ret) {
        bool bDot = false;

        if (type == DSO_TRIGGER_AUTO && roll) {
          type_str =
              L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO_ROLL), "Auto(Roll)");

          if (_viewport->_view.session().is_instant()) {
            type_str += ", ";
            type_str += L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VIEW_CAPTURE),
                            "Capturing");
            bDot = true;
          }
        } else if (type == DSO_TRIGGER_AUTO &&
                   !_viewport->_view.session().trigd()) {
          type_str = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO), "Auto");

          if (_viewport->_view.session().is_instant()) {
            type_str += ", ";
            type_str += L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VIEW_CAPTURE),
                            "Capturing");
            bDot = true;
          }
        } else if (_viewport->_waiting_trig > 0) {
          type_str = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_WAITING_TRIG),
                         "Waiting Trig");
          bDot = true;
        } else {
          type_str = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIG_D), "Trig'd");
        }

        if (bDot) {
          for (int i = 0; i < _viewport->_tigger_wait_times; i++) {
            type_str += ".";
          }

          high_resolution_clock::time_point cur_time =
              high_resolution_clock::now();
          milliseconds timeInterval = std::chrono::duration_cast<milliseconds>(
              cur_time - _viewport->_lst_wait_tigger_time);
          int64_t time_keep = timeInterval.count();

          if (time_keep >= 500) {
            _viewport->_tigger_wait_times++;
            _viewport->_lst_wait_tigger_time = cur_time;
          }

          if (_viewport->_tigger_wait_times > 4)
            _viewport->_tigger_wait_times = 0;
        }
      }
      p.setPen(fore);
      p.drawText(_viewport->_view.get_view_rect(),
                 Qt::AlignLeft | Qt::AlignTop, type_str);

      if (dev->is_hardware()) {
        if (_viewport->_view.session().dso_data_is_out_off_range()) {
          QString data_status = L_S(STR_PAGE_DLG,
                                    S_ID(IDS_DLG_DATA_OUT_OFF_RANGE),
                                    "Out off range");
          data_status += "! ";
          QColor warnRed = AppConfig::Instance().GetThemeColor("@warn-red");
          if (!warnRed.isValid())
            warnRed = QColor(255, 0, 0, 200);
          p.setPen(warnRed);
          p.drawText(_viewport->_view.get_view_rect(),
                     Qt::AlignRight | Qt::AlignTop, data_status);
          p.setPen(fore);
        }
      }
    }
  }
}

void ViewportPainter::paintProgress(QPainter &p, QColor fore, QColor back) {
  (void)back;

  if (_viewport->_view.is_logic_rendering_mode() &&
      _viewport->_view.session().is_repeat_mode()) {
    return;
  }

  using pv::view::Signal;

  double progress = 0;
  int progress100 = 0;
  int captured_progress = 0;

  _viewport->get_captured_progress(progress, progress100);

  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::gray);
  p.setBrush(Qt::NoBrush);
  const QPoint cenPos =
      QPoint(_viewport->_view.get_view_width() / 2, _viewport->height() / 2);
  const int radius =
      min(0.3 * _viewport->_view.get_view_width(), 0.3 * _viewport->height());
  p.drawEllipse(cenPos, radius - 2, radius - 2);
  p.setPen(QPen(View::Green, 4, Qt::SolidLine));
  p.drawArc(cenPos.x() - radius, cenPos.y() - radius, 2 * radius, 2 * radius,
            180 * 16, progress);

  if (!_viewport->_transfer_started) {
    const int width = _viewport->_view.get_view_width();
    const QPoint cenLeftPos =
        QPoint(width / 2 - 0.05 * width, _viewport->height() / 2);
    const QPoint cenRightPos =
        QPoint(width / 2 + 0.05 * width, _viewport->height() / 2);
    const int trigger_radius = min(0.02 * width, 0.02 * _viewport->height());

    QColor foreBack = fore;
    foreBack.setAlpha(View::BackAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush((_viewport->_timer_cnt % 3) == 0 ? fore : foreBack);
    p.drawEllipse(cenLeftPos, trigger_radius, trigger_radius);
    p.setBrush((_viewport->_timer_cnt % 3) == 1 ? fore : foreBack);
    p.drawEllipse(cenPos, trigger_radius, trigger_radius);
    p.setBrush((_viewport->_timer_cnt % 3) == 2 ? fore : foreBack);
    p.drawEllipse(cenRightPos, trigger_radius, trigger_radius);

    bool triggered;

    if (_viewport->_view.session().get_capture_status(
            triggered, captured_progress)) {
      p.setPen(View::Blue);

      QFont font = theme_font_cursor();
      p.setFont(font);

      QRect status_rect =
          QRect(cenPos.x() - radius, cenPos.y() + radius * 0.4,
                radius * 2, radius * 0.5);

      if (triggered) {
        p.drawText(status_rect, Qt::AlignCenter | Qt::AlignVCenter,
                   L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGERED),
                       "Triggered! ") +
                       QString::number(captured_progress) +
                       L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CAPTURED),
                           "% Captured"));
      } else {
        p.drawText(status_rect, Qt::AlignCenter | Qt::AlignVCenter,
                   L_S(STR_PAGE_DLG, S_ID(IDS_DLG_WAITING_FOR_TRIGGER),
                       "Waiting for Trigger! ") +
                       QString::number(captured_progress) +
                       L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CAPTURED),
                           "% Captured"));
      }

      _viewport->prgRate(captured_progress);
    }

  } else {
    p.setPen(View::Green);
    QFont font = p.font();
    font.setPointSize(50);
    font.setBold(true);
    apply_global_font_strategy(font);
    p.setFont(font);

    p.drawText(_viewport->_view.get_view_rect(),
               Qt::AlignCenter | Qt::AlignVCenter,
               QString::number(progress100) + "%");
    _viewport->prgRate(progress100);
  }

  p.setPen(QPen(View::Blue, 4, Qt::SolidLine));
  const int int_radius = max(radius - 4, 0);
  p.drawArc(cenPos.x() - int_radius, cenPos.y() - int_radius,
            2 * int_radius, 2 * int_radius, 180 * 16,
            -captured_progress * 3.6 * 16);
  QFont font = QApplication::font();
  p.setFont(font);

  p.setRenderHint(QPainter::Antialiasing, false);
}

void ViewportPainter::paintMeasure(QPainter &p, QColor fore, QColor back) {
  QColor active_color = back.black() > 0x80 ? View::Orange : View::Purple;
  _viewport->_hover_hit = false;

  int v_offset = _viewport->_view.get_vOffset();
  int screen_midY = _viewport->_cur_midY - v_offset;
  int screen_preY = _viewport->_cur_preY - v_offset;
  int screen_aftY = _viewport->_cur_aftY - v_offset;
  QPointF screen_hover_point =
      _viewport->_view.hover_point() - QPointF(0, v_offset);

  if (_viewport->_action_type == NO_ACTION &&
      _viewport->_measure_type == LOGIC_FREQ) {
    p.setPen(active_color);
    p.drawLine(QLineF(_viewport->_cur_preX, screen_midY,
                      _viewport->_cur_aftX, screen_midY));
    p.drawLine(QLineF(_viewport->_cur_preX, screen_midY, _viewport->_cur_preX + 2,
                      screen_midY - 2));
    p.drawLine(QLineF(_viewport->_cur_preX, screen_midY, _viewport->_cur_preX + 2,
                      screen_midY + 2));
    p.drawLine(QLineF(_viewport->_cur_aftX - 2, screen_midY - 2,
                      _viewport->_cur_aftX, screen_midY));
    p.drawLine(QLineF(_viewport->_cur_aftX - 2, screen_midY + 2,
                      _viewport->_cur_aftX, screen_midY));
    if (_viewport->_thd_sample != 0) {
      p.drawLine(QLineF(_viewport->_cur_aftX, screen_midY, _viewport->_cur_thdX,
                        screen_midY));
      p.drawLine(QLineF(_viewport->_cur_aftX, screen_midY, _viewport->_cur_aftX + 2,
                        screen_midY - 2));
      p.drawLine(QLineF(_viewport->_cur_aftX, screen_midY, _viewport->_cur_aftX + 2,
                        screen_midY + 2));
      p.drawLine(QLineF(_viewport->_cur_thdX - 2, screen_midY - 2,
                        _viewport->_cur_thdX, screen_midY));
      p.drawLine(QLineF(_viewport->_cur_thdX - 2, screen_midY + 2,
                        _viewport->_cur_thdX, screen_midY));
    }

    if (_viewport->_measure_en) {
      vector<pair<QString, QString>> rows = {
          {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FREQUENCY), "Frequency: "),
           _viewport->_mm_freq},
          {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PERIOD), "Period: "),
           _viewport->_mm_period},
          {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_WIDTH), "Width: "),
           _viewport->_mm_width},
          {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DUTY_CYCLE), "Duty Cycle: "),
           _viewport->_mm_duty}};

      drawFloatingPanel(p, screen_hover_point,
                        _viewport->_view.get_view_width(),
                        _viewport->_view.viewport()->height(), back,
                        _viewport->_panelBgColor, _viewport->_panelTextColor,
                        rows);
    }
  }

  if (_viewport->_action_type == NO_ACTION &&
      _viewport->_measure_type == DSO_VALUE) {

    for (auto s : _viewport->_view.get_own_signals()) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        uint64_t index;
        double value;
        view::DsoSignal *dsoSig = (DsoSignal *)s;
        QPointF hpoint;
        if (dsoSig->get_hover(index, hpoint, value)) {
          p.setPen(QPen(fore, 1, Qt::DashLine));
          p.setBrush(Qt::NoBrush);
          p.drawLine(hpoint.x(), dsoSig->get_view_rect().top(), hpoint.x(),
                     dsoSig->get_view_rect().bottom());
        }
      } else if (s->signal_type() == SR_CHANNEL_ANALOG) {
        uint64_t index;
        double value;
        QPointF hpoint;
        view::AnalogSignal *analogSig = (AnalogSignal *)s;
        if (analogSig->get_hover(index, hpoint, value)) {
          p.setPen(QPen(fore, 1, Qt::DashLine));
          p.setBrush(Qt::NoBrush);
          p.drawLine(hpoint.x(), analogSig->get_view_rect().top(), hpoint.x(),
                     analogSig->get_view_rect().bottom());
        }
      }
    }
  }

  if (_viewport->_dso_ym_valid) {
    for (auto s : _viewport->_view.get_own_signals()) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (DsoSignal *)s;
        if (dsoSig->get_index() == _viewport->_dso_ym_sig_index) {
          p.setPen(QPen(dsoSig->get_colour(), 1, Qt::DotLine));
          QFontMetrics fm(p.font());
          const int text_height = fm.height();
          const int64_t x =
              _viewport->_view.index2pixel(_viewport->_dso_ym_index);
          p.drawLine(x - 10, _viewport->_dso_ym_start, x + 10,
                     _viewport->_dso_ym_start);
          p.drawLine(x, _viewport->_dso_ym_start, x, _viewport->_dso_ym_end);
          p.drawLine(0, _viewport->_dso_ym_end,
                     _viewport->_view.get_view_width(),
                     _viewport->_dso_ym_end);

          // -- vertical delta value
          double hrate = (_viewport->_dso_ym_start - _viewport->_dso_ym_end) *
                         1.0f / _viewport->_view.get_view_height();
          double value = hrate * dsoSig->get_vDialValue() *
                         dsoSig->get_factor() * DS_CONF_DSO_VDIVS;
          QString value_str =
              abs(value) > 1000
                  ? QString::number(value / 1000.0, 'f', 2) + "V"
                  : QString::number(value, 'f', 2) + "mV";
          int value_rect_width = p.boundingRect(
                                      0, 0, INT_MAX, INT_MAX,
                                      Qt::AlignLeft | Qt::AlignVCenter, value_str)
                                      .width();
          p.drawText(QRect(x + 10,
                           abs(_viewport->_dso_ym_start +
                                _viewport->_dso_ym_end) / 2,
                           value_rect_width, text_height),
                     value_str);

          // -- start value
          value_str = abs(_viewport->_dso_ym_sig_value) > 1000
                          ? QString::number(
                                _viewport->_dso_ym_sig_value / 1000.0, 'f', 2) +
                                "V"
                          : QString::number(_viewport->_dso_ym_sig_value, 'f',
                                            2) +
                                "mV";
          value_rect_width = p.boundingRect(
                                 0, 0, INT_MAX, INT_MAX,
                                 Qt::AlignLeft | Qt::AlignVCenter, value_str)
                                 .width();
          int str_y = value > 0 ? _viewport->_dso_ym_start
                                : _viewport->_dso_ym_start - text_height;
          p.drawText(QRect(x - 0.5 * value_rect_width, str_y,
                           value_rect_width, text_height),
                     value_str);

          // -- end value
          double end_value = _viewport->_dso_ym_sig_value + value;
          value_str = abs(end_value) > 1000
                          ? QString::number(end_value / 1000.0, 'f', 2) + "V"
                          : QString::number(end_value, 'f', 2) + "mV";
          value_rect_width = p.boundingRect(
                                 0, 0, INT_MAX, INT_MAX,
                                 Qt::AlignLeft | Qt::AlignVCenter, value_str)
                                 .width();
          str_y = value > 0 ? _viewport->_dso_ym_end - text_height
                            : _viewport->_dso_ym_end;
          p.drawText(QRect(x - 0.5 * value_rect_width, str_y,
                           value_rect_width, text_height),
                     value_str);
          break;
        }
      }
    }
  }

  if (_viewport->_dso_xm_valid) {
    p.setPen(QPen(Qt::red, 1, Qt::DotLine));
    int measure_line_count = 6;
    const int text_height =
        p.boundingRect(0, 0, INT_MAX, INT_MAX, Qt::AlignLeft | Qt::AlignTop,
                       "W")
            .height();
    const uint64_t sample_rate =
        _viewport->_view.session().cur_snap_samplerate();
    QLineF *line;
    QLineF *const measure_lines = new QLineF[measure_line_count];
    line = measure_lines;
    int64_t x[Viewport::DsoMeasureStages];
    int dso_xm_stage = 0;
    if (_viewport->_action_type == DSO_XM_STEP1)
      dso_xm_stage = 1;
    else if (_viewport->_action_type == DSO_XM_STEP2)
      dso_xm_stage = 2;
    else
      dso_xm_stage = 3;

    for (int i = 0; i < dso_xm_stage; i++) {
      x[i] = _viewport->_view.index2pixel(_viewport->_dso_xm_index[i]);
    }
    measure_line_count = 0;
    if (dso_xm_stage > 0) {
      *line++ = QLine(x[0], _viewport->_dso_xm_y - 10, x[0],
                      _viewport->_dso_xm_y + 10);
      measure_line_count += 1;
    }
    if (dso_xm_stage > 1) {
      *line++ = QLine(x[1], _viewport->_dso_xm_y - 10, x[1],
                      _viewport->_dso_xm_y + 10);
      *line++ = QLine(x[0], _viewport->_dso_xm_y, x[1], _viewport->_dso_xm_y);
      _viewport->_mm_width = _viewport->_view.get_ruler()->format_real_time(
          _viewport->_dso_xm_index[1] - _viewport->_dso_xm_index[0],
          sample_rate);

      // -- width show
      const QString w_ctr = "W=" + _viewport->_mm_width;
      int w_rect_width = p.boundingRect(
                             0, 0, INT_MAX, INT_MAX,
                             Qt::AlignLeft | Qt::AlignVCenter, w_ctr)
                             .width();
      p.drawText(QRect(x[0] + 10, _viewport->_dso_xm_y - text_height,
                       w_rect_width, text_height),
                 w_ctr);
      measure_line_count += 2;
    }
    if (dso_xm_stage > 2) {
      *line++ = QLineF(x[0], _viewport->_dso_xm_y + 20, x[0],
                       _viewport->_dso_xm_y + 40);
      *line++ = QLineF(x[0], _viewport->_dso_xm_y + 30, x[2],
                       _viewport->_dso_xm_y + 30);
      *line++ = QLineF(x[2], _viewport->_dso_xm_y + 20, x[2],
                       _viewport->_dso_xm_y + 40);
      _viewport->_mm_period = _viewport->_view.get_ruler()->format_real_time(
          _viewport->_dso_xm_index[2] - _viewport->_dso_xm_index[0],
          sample_rate);
      _viewport->_mm_freq = _viewport->_view.get_ruler()->format_real_freq(
          _viewport->_dso_xm_index[2] - _viewport->_dso_xm_index[0],
          sample_rate);
      _viewport->_mm_duty =
          QString::number((_viewport->_dso_xm_index[1] -
                            _viewport->_dso_xm_index[0]) *
                               100.0 /
                               (_viewport->_dso_xm_index[2] -
                                _viewport->_dso_xm_index[0]),
                           'f', 2) +
          "%";

      // -- period show
      const QString p_ctr = "P=" + _viewport->_mm_period;
      int p_rect_width = p.boundingRect(
                             0, 0, INT_MAX, INT_MAX,
                             Qt::AlignLeft | Qt::AlignVCenter, p_ctr)
                             .width();
      p.drawText(QRect(x[0] + 10, _viewport->_dso_xm_y + 30 - text_height,
                       p_rect_width, text_height),
                 p_ctr);

      // -- frequency show
      const QString f_ctr = "F=" + _viewport->_mm_freq;
      int f_rect_width = p.boundingRect(
                             0, 0, INT_MAX, INT_MAX,
                             Qt::AlignLeft | Qt::AlignVCenter, f_ctr)
                             .width();
      p.drawText(QRect(x[0] + 20 + p_rect_width,
                       _viewport->_dso_xm_y + 30 - text_height, f_rect_width,
                       text_height),
                 f_ctr);

      // -- duty show
      const QString d_ctr = "D=" + _viewport->_mm_duty;
      int d_rect_width = p.boundingRect(
                             0, 0, INT_MAX, INT_MAX,
                             Qt::AlignLeft | Qt::AlignVCenter, d_ctr)
                             .width();
      p.drawText(QRect(x[1] + 10, _viewport->_dso_xm_y - 0.5 * text_height,
                       d_rect_width, text_height),
                 d_ctr);

      measure_line_count += 3;
    }
    p.drawLines(measure_lines, measure_line_count);
    if (dso_xm_stage < Viewport::DsoMeasureStages) {
      p.drawLine(x[dso_xm_stage - 1], _viewport->_dso_xm_y,
                 _viewport->_mouse_point.x(), _viewport->_dso_xm_y);
      p.drawLine(_viewport->_mouse_point.x(), 0,
                 _viewport->_mouse_point.x(), _viewport->height());
    }
    _viewport->measure_updated();
  }

  if (_viewport->_action_type == LOGIC_EDGE &&
      _viewport->_view.session().have_view_data()) {
    p.setPen(active_color);
    p.drawLine(
        QLineF(_viewport->_cur_preX, screen_midY - 5, _viewport->_cur_preX,
               screen_midY + 5));
    p.drawLine(
        QLineF(_viewport->_cur_aftX, screen_midY - 5, _viewport->_cur_aftX,
               screen_midY + 5));
    p.drawLine(QLineF(_viewport->_cur_preX, screen_midY, _viewport->_cur_aftX,
                      screen_midY));

    vector<pair<QString, QString>> rows = {{"", _viewport->_em_edges},
                                           {"", _viewport->_em_rising},
                                           {"", _viewport->_em_falling}};

    drawFloatingPanel(p, screen_hover_point,
                      _viewport->_view.get_view_width(),
                      _viewport->_view.viewport()->height(), back,
                      _viewport->_panelBgColor, _viewport->_panelTextColor,
                      rows);
  }

  if (_viewport->_action_type == LOGIC_JUMP) {
    p.setPen(active_color);
    p.setBrush(Qt::NoBrush);
    const QPoint pre_points[] = {
        QPoint(_viewport->_cur_preX, screen_preY),
        QPoint(_viewport->_cur_preX - 1, screen_preY - 1),
        QPoint(_viewport->_cur_preX + 1, screen_preY - 1),
        QPoint(_viewport->_cur_preX - 1, screen_preY + 1),
        QPoint(_viewport->_cur_preX + 1, screen_preY + 1),
        QPoint(_viewport->_cur_preX - 2, screen_preY - 2),
        QPoint(_viewport->_cur_preX + 2, screen_preY - 2),
        QPoint(_viewport->_cur_preX - 2, screen_preY + 2),
        QPoint(_viewport->_cur_preX + 2, screen_preY + 2),
    };
    p.drawPoints(pre_points, countof(pre_points));
    if (abs(_viewport->_cur_aftX - _viewport->_cur_preX) +
            abs(_viewport->_cur_aftY - _viewport->_cur_preY) >
        20) {
      if (_viewport->_edge_hit) {
        const QPoint aft_points[] = {
            QPoint(_viewport->_cur_aftX, screen_aftY),
            QPoint(_viewport->_cur_aftX - 1, screen_aftY - 1),
            QPoint(_viewport->_cur_aftX + 1, screen_aftY - 1),
            QPoint(_viewport->_cur_aftX - 1, screen_aftY + 1),
            QPoint(_viewport->_cur_aftX + 1, screen_aftY + 1),
            QPoint(_viewport->_cur_aftX - 2, screen_aftY - 2),
            QPoint(_viewport->_cur_aftX + 2, screen_aftY - 2),
            QPoint(_viewport->_cur_aftX - 2, screen_aftY + 2),
            QPoint(_viewport->_cur_aftX + 2, screen_aftY + 2),
        };
        p.drawPoints(aft_points, countof(aft_points));
      }
      int64_t delta = max(_viewport->_edge_start, _viewport->_edge_end) -
                      min(_viewport->_edge_start, _viewport->_edge_end);
      QString delta_text =
          _viewport->_view.get_index_delta(_viewport->_edge_start,
                                           _viewport->_edge_end) +
          "/" + QString::number(delta);

      vector<pair<QString, QString>> rows = {{"", delta_text}};

      drawFloatingPanel(p, screen_hover_point,
                        _viewport->_view.get_view_width(),
                        _viewport->_view.viewport()->height(), back,
                        _viewport->_panelBgColor, _viewport->_panelTextColor,
                        rows);

      QPainterPath path(QPoint(_viewport->_cur_preX, screen_preY));
      QPoint c1((_viewport->_cur_preX + _viewport->_cur_aftX) / 2,
                screen_preY);
      QPoint c2((_viewport->_cur_preX + _viewport->_cur_aftX) / 2,
                screen_aftY);
      path.cubicTo(c1, c2, QPoint(_viewport->_cur_aftX, screen_aftY));
      p.drawPath(path);
    }
  }
}

} // namespace view
} // namespace pv
