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

#include "pv/view/renderer/viewport_painter.h"
#include "pv/view/renderer/render_pass.h"
#include "pv/view/viewport/viewport.h"
#include "pv/view/component/viewstatus.h"

#include "pv/session/sigsession.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/signal.h"

#include <QPainter>
#include <QStyleOption>
#include <cmath>
#include <set>

#include "pv/config/appconfig.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/base/perflog.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/langresource.h"

using namespace std;

// Thread-local DSO sub-timing (defined here, declared extern in viewport.h).
thread_local DsoPaintTiming s_dso_timing;

namespace pv {
namespace view {

ViewportPainter::ViewportPainter(Viewport *viewport) : _viewport(viewport) {}

ViewportPainter::~ViewportPainter() {}

void ViewportPainter::paintEvent(QPaintEvent *event) {
  PXV_PERF_SCOPE_VIEWPORT();
  if (_viewport->drag_active() && !_viewport->drag_snapshot().isNull()) {
    QPainter p(_viewport);
    p.drawPixmap(0, 0, _viewport->drag_snapshot());
    return;
  }

  (void)event;

  _viewport->paint_in_this_second()++;
  if (_viewport->is_idle() || !_viewport->frame_interval_timer().isValid()) {
    _viewport->frame_interval_timer().restart();
    _viewport->is_idle() = false;
  } else {
    int elapsed =
        static_cast<int>(_viewport->frame_interval_timer().restart());
    if (elapsed > _viewport->max_frame_time()) {
      _viewport->max_frame_time() = elapsed;
    }
  }

  doPaint(event->rect());
}

void ViewportPainter::doPaint(const QRect & /* dirtyRect */) {
  using pv::view::Signal;

  QStyleOption o;
  o.initFrom(_viewport);
  QPainter p(_viewport);
  _viewport->style()->drawPrimitive(QStyle::PE_Widget, &o, &p, _viewport);

  QFont font = theme_font_cursor();
  p.setFont(font);

  _viewport->view().session().check_update();

  QColor fore(_viewport->palette().color(_viewport->foregroundRole()));
  QColor back(_viewport->palette().color(_viewport->backgroundRole()));
  fore.setAlpha(View::ForeAlpha);
  _viewport->view().set_back(false);

  std::vector<Trace *> traces;
  _viewport->view().get_traces(_viewport->type(), traces);

  // Build PaintContext snapshot for all paint calls in this frame.
  pv::view::PaintContext pctx;
  pctx.scale = _viewport->view().scale();
  pctx.offset = _viewport->view().offset();
  pctx.trig_hoff = _viewport->view().trig_hoff();
  pctx.signal_height = _viewport->view().get_signalHeight();
  pctx.view_width = _viewport->view().get_view_width();
  pctx.is_logic_mode = _viewport->view().is_logic_rendering_mode();
  pctx.is_stopped_status = _viewport->view().session().is_stopped_status();
  pctx.is_loop_mode = _viewport->view().session().is_loop_mode();
  pctx.dso_trig_moved = _viewport->view().get_dso_trig_moved();
  pctx.show_glitch_overlay = _viewport->view().session().show_glitch_filter_overlay();
  pctx.hover_point = _viewport->view().hover_point();

  p.save();
  p.translate(0, -_viewport->view().get_vOffset());

  // Phase 5: Group card background rendering via RenderPass.
  // All six passes are now wired in: GroupCardBackgroundPass (here in
  // doPaint), SignalPixmapPass/DecodeTracePass/CursorOverlayPass/
  // MeasureOverlayPass/TriggerInfoPass (in paintSignals).
  {
    GroupCardBackgroundPass cardPass;
    RenderContext ctx;
    ctx.view = &_viewport->view();
    ctx.viewport = _viewport;
    ctx.type = _viewport->type();
    ctx.viewWidth = _viewport->width();
    ctx.is_logic_mode = _viewport->view().is_logic_rendering_mode();
    if (ctx.type == TIME_VIEW && ctx.is_logic_mode)
      ctx.groups = &_viewport->view().get_signal_groups();
    if (cardPass.should_run(ctx))
      cardPass.render(p, ctx);
  }

  QColor dividerColor =
      AppConfig::Instance().GetThemeColor("@border-strong");
  if (!dividerColor.isValid()) {
    double lum =
        back.red() * 0.299 + back.green() * 0.587 + back.blue() * 0.114;
    dividerColor =
        lum < 128 ? QColor(0x37, 0x37, 0x3b) : QColor(0xd5, 0xd5, 0xd5);
  }

  std::set<Trace *> lastInGroup;
  if (_viewport->type() == TIME_VIEW &&
      _viewport->view().is_logic_rendering_mode()) {
    const auto &groups = _viewport->view().get_signal_groups();
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
    if ((*it)->enabled() || (*it)->signal_type() == SR_CHANNEL_DSO) {
      lastEnabledTrace = *it;
      break;
    }
  }

  p.setPen(QPen(dividerColor, 1));
  for (auto t : traces) {
    if (!t->enabled() && t->signal_type() != SR_CHANNEL_DSO)
      continue;
    if (lastInGroup.count(t))
      continue;
    if (t == lastEnabledTrace)
      continue;
    int traceBottom =
        t->get_v_offset() + t->get_totalHeight() / 2 + View::SignalMargin;
    p.drawLine(0, traceBottom, _viewport->view().get_view_width(),
               traceBottom);
  }

  for (auto t : traces) {
    if (!t->enabled() && t->signal_type() != SR_CHANNEL_DSO)
      continue;
    t->paint_back(p, 0, _viewport->view().get_view_width(), fore, back, pctx);
    if (_viewport->view().back_ready())
      break;
  }

  p.restore();

  if (_viewport->view().is_logic_rendering_mode() ||
      _viewport->view().session().is_instant()) {
    if (_viewport->view().session().is_init_status()) {
      paintCursors(p);
    } else if (_viewport->view().session().is_stopped_status()) {
      {
        const auto _ps_t0 = std::chrono::steady_clock::now();
        paintSignals(p, fore, back);
        const auto _ps_t1 = std::chrono::steady_clock::now();
        pv::base::perf::record_track(
            "V_PAINTSIGNALS", 0.0, 0.0,
            std::chrono::duration<double, std::milli>(_ps_t1 - _ps_t0).count(), 0);
      }
    } else if (_viewport->view().session().is_realtime_refresh()) {
      _viewport->view().session().have_new_realtime_refresh(false);

      if (_viewport->view().session().have_view_data() ||
          _viewport->view().session().is_instant())
        paintSignals(p, fore, back);
      else
        paintProgress(p, fore, back);
    } else if (_viewport->view().session().is_running_status()) {
      if (_viewport->view().session().is_repeat_mode()) {
        paintSignals(p, fore, back);

        if (!_viewport->transfer_started()) {
          bool triggered;
          int captured_progress;

          if (_viewport->view().session().get_capture_status(
                  triggered, captured_progress)) {
            _viewport->view().show_captured_progress(triggered,
                                                    captured_progress);
          }
        }
      } else if (_viewport->type() == TIME_VIEW) {
        _viewport->view().repeat_unshow();
        paintProgress(p, fore, back);
      }
    }
  } else {
    paintSignals(p, fore, back);
  }

  p.save();
  p.translate(0, -_viewport->view().get_vOffset());
  for (auto t : traces) {
    if (t->enabled())
      t->paint_fore(p, 0, _viewport->view().get_view_width(), fore, back, pctx);
  }
  p.restore();

  if (_viewport->view().get_signalHeight() != _viewport->curSignalHeight())
    _viewport->curSignalHeight() = _viewport->view().get_signalHeight();

  p.end();
}

void ViewportPainter::paintCursors(QPainter &p) {
  // Phase 5: cursor overlay rendering now via CursorOverlayPass.
  // Handles regular cursors, xcursors, trigger cursor, and search cursor.
  RenderContext cctx;
  cctx.view = &_viewport->view();
  cctx.viewport = _viewport;
  cctx.type = _viewport->type();

  CursorOverlayPass cursorPass;
  if (cursorPass.should_run(cctx))
    cursorPass.render(p, cctx);
}

void ViewportPainter::paintSignals(QPainter &p, QColor fore, QColor back) {
  std::vector<Trace *> traces;
  _viewport->view().get_traces(_viewport->type(), traces);

  // Build PaintContext snapshot for paint calls in this frame.
  PaintContext pctx;
  pctx.scale = _viewport->view().scale();
  pctx.offset = _viewport->view().offset();
  pctx.trig_hoff = _viewport->view().trig_hoff();
  pctx.signal_height = _viewport->view().get_signalHeight();
  pctx.view_width = _viewport->view().get_view_width();
  pctx.is_logic_mode = _viewport->view().is_logic_rendering_mode();
  pctx.is_stopped_status = _viewport->view().session().is_stopped_status();
  pctx.is_loop_mode = _viewport->view().session().is_loop_mode();
  pctx.dso_trig_moved = _viewport->view().get_dso_trig_moved();
  pctx.show_glitch_overlay = _viewport->view().session().show_glitch_filter_overlay();
  pctx.hover_point = _viewport->view().hover_point();

  // Phase 5: Signal pixmap rebuild + blit via SignalPixmapPass.
  {
    RenderContext sctx;
    sctx.view = &_viewport->view();
    sctx.viewport = _viewport;
    sctx.type = _viewport->type();
    sctx.vOffset = _viewport->view().get_vOffset();
    sctx.fore = fore;
    sctx.back = back;
    sctx.traces = &traces;
    sctx.is_logic_mode = _viewport->view().is_logic_rendering_mode();
    sctx.viewWidth = _viewport->view().get_view_width();
    sctx.pctx = pctx;

    SignalPixmapPass pixmapPass;
    if (pixmapPass.should_run(sctx))
      pixmapPass.render(p, sctx);
  }

  // Phase 5: Decode trace rendering via DecodeTracePass.
  // Paint decode traces directly on the widget (not via QPixmap) to
  // ensure crisp text rendering.
  {
    RenderContext dctx;
    dctx.view = &_viewport->view();
    dctx.viewport = _viewport;
    dctx.type = _viewport->type();
    dctx.vOffset = _viewport->view().get_vOffset();
    dctx.fore = fore;
    dctx.back = back;
    dctx.traces = &traces;
    dctx.is_logic_mode = _viewport->view().is_logic_rendering_mode();
    dctx.pctx = pctx;

    DecodeTracePass decodePass;
    if (decodePass.should_run(dctx)) {
      p.save();
      p.setFont(theme_font_trace_label());
      decodePass.render(p, dctx);
      p.restore();
    }
  }

  // Phase 5: cursor overlay (regular + xcursor + trigger + search) via CursorOverlayPass.
  paintCursors(p);

  if (_viewport->type() == TIME_VIEW) {
    // plot zoom rect
    if (_viewport->action_type() == LOGIC_ZOOM) {
      p.setPen(Qt::NoPen);
      p.setBrush(View::LightBlue);
      p.drawRect(
          QRectF(_viewport->mouse_down_point(), _viewport->mouse_point()));
    }

    // Phase 5: measurement overlay via MeasureOverlayPass.
    {
RenderContext mctx;
mctx.view = &_viewport->view();
mctx.viewport = _viewport;
mctx.type = _viewport->type();
mctx.fore = fore;
mctx.back = back;
mctx.traces = &traces;
mctx.is_logic_mode = _viewport->view().is_logic_rendering_mode();
mctx.viewWidth = _viewport->view().get_view_width();

MeasureOverlayPass measurePass;
      if (measurePass.should_run(mctx))
        measurePass.render(p, mctx);
    }

    // Phase 5: DSO trigger info via TriggerInfoPass.
    {
      RenderContext tctx;
      tctx.view = &_viewport->view();
      tctx.viewport = _viewport;
      tctx.type = _viewport->type();
      tctx.fore = fore;
      tctx.back = back;

      TriggerInfoPass trigPass;
      if (trigPass.should_run(tctx))
        trigPass.render(p, tctx);
    }
  }
}

void ViewportPainter::paintProgress(QPainter &p, QColor fore, QColor back) {
  (void)back;

  if (_viewport->view().is_logic_rendering_mode() &&
      _viewport->view().session().is_repeat_mode()) {
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
      QPoint(_viewport->view().get_view_width() / 2, _viewport->height() / 2);
  const int radius =
      min(0.3 * _viewport->view().get_view_width(), 0.3 * _viewport->height());
  p.drawEllipse(cenPos, radius - 2, radius - 2);
  p.setPen(QPen(View::Green, 4, Qt::SolidLine));
  p.drawArc(cenPos.x() - radius, cenPos.y() - radius, 2 * radius, 2 * radius,
            180 * 16, progress);

  if (!_viewport->transfer_started()) {
    const int width = _viewport->view().get_view_width();
    const QPoint cenLeftPos =
        QPoint(static_cast<int>(width / 2.0 - 0.05 * width), _viewport->height() / 2);
    const QPoint cenRightPos =
        QPoint(static_cast<int>(width / 2.0 + 0.05 * width), _viewport->height() / 2);
    const int trigger_radius = min(0.02 * width, 0.02 * _viewport->height());

    QColor foreBack = fore;
    foreBack.setAlpha(View::BackAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush((_viewport->timer_cnt() % 3) == 0 ? fore : foreBack);
    p.drawEllipse(cenLeftPos, trigger_radius, trigger_radius);
    p.setBrush((_viewport->timer_cnt() % 3) == 1 ? fore : foreBack);
    p.drawEllipse(cenPos, trigger_radius, trigger_radius);
    p.setBrush((_viewport->timer_cnt() % 3) == 2 ? fore : foreBack);
    p.drawEllipse(cenRightPos, trigger_radius, trigger_radius);

    bool triggered;

    if (_viewport->view().session().get_capture_status(
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

    p.drawText(_viewport->view().get_view_rect(),
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

} // namespace view
} // namespace pv
