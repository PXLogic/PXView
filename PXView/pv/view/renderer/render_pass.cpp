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

#include "pv/view/renderer/render_pass.h"
#include "pv/view/renderer/rasterize.h"
#include "pv/view/renderer/render_worker.h"
#include "pv/view/viewport/viewport.h"
#include "pv/view/view.h"
#include "pv/view/trace/trace.h"
#include "pv/view/signal/signal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/cursor/cursor.h"
#include "pv/view/cursor/xcursor.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/component/ruler.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <list>
#include <vector>

#include "pv/config/appconfig.h"
#include "pv/session/sigsession.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/fn.h"
#include "pv/ui/langresource.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/deviceagent.h"
#include <libsigrok/libsigrok.h>

namespace pv {
namespace view {

// ---------------------------------------------------------------------------
// Static helpers (migrated from viewport_painter.cpp for MeasureOverlayPass)
// ---------------------------------------------------------------------------

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
                              const std::vector<std::pair<QString, QString>> &rows) {
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

// ---------------------------------------------------------------------------
// GroupCardBackgroundPass
// ---------------------------------------------------------------------------

bool GroupCardBackgroundPass::should_run(const RenderContext &ctx) const {
  return ctx.type == 0 /* TIME_VIEW */ && ctx.is_logic_mode &&
         ctx.groups && !ctx.groups->empty();
}

void GroupCardBackgroundPass::render(QPainter &p, const RenderContext &ctx) {
  if (!ctx.groups || !ctx.view)
    return;

  // Sort group indices by their first trace's v_offset
  std::vector<size_t> group_indices(ctx.groups->size());
  for (size_t i = 0; i < ctx.groups->size(); i++)
    group_indices[i] = i;
  std::sort(group_indices.begin(), group_indices.end(),
            [&groups = *ctx.groups](size_t a, size_t b) {
              if (groups[a].traces.empty())
                return false;
              if (groups[b].traces.empty())
                return true;
              return groups[a].traces[0]->get_v_offset() <
                     groups[b].traces[0]->get_v_offset();
            });

  for (size_t idx = 0; idx < group_indices.size(); idx++) {
    const auto &group = (*ctx.groups)[group_indices[idx]];
    if (group.traces.empty())
      continue;

    double groupTop = 1e9;
    double groupBottom = -1e9;
    for (auto gt : group.traces) {
      double traceTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 -
                        View::SignalMargin;
      double traceBottom = gt->get_v_offset() +
                           gt->get_totalHeight() * 0.5 + View::SignalMargin;
      groupTop = std::min(groupTop, traceTop);
      groupBottom = std::max(groupBottom, traceBottom);
    }

    double cardTop = groupTop - View::GroupGap * 0.5;
    double cardHeight = groupBottom - groupTop + View::GroupGap;

    QRectF cardRect(-View::GroupCardRadius, cardTop,
                    ctx.viewWidth + View::GroupCardRadius + 1,
                    cardHeight);
    QPainterPath groupPath;
    groupPath.addRoundedRect(cardRect, View::GroupCardRadius,
                             View::GroupCardRadius);

    if (ctx.view->is_colored_card_mode()) {
      // Per-trace colored rectangles clipped within the card path
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
                         ctx.viewWidth + View::GroupCardRadius + 1,
                         tBottom - tTop);
        p.setBrush(ctx.view->get_trace_card_color(gt));
        p.drawRect(traceRect);
      }
      p.restore();
    } else {
      // Single-color filled card
      p.setPen(Qt::NoPen);
      p.setBrush(ctx.view->get_group_card_color());
      p.drawPath(groupPath);
    }
  }
}

// ---------------------------------------------------------------------------
// SignalPixmapPass — cached pixmap rebuild + blit for signal waveforms.
// In logic mode: logic signals use paint_mid_align_sample, non-decoder traces
// use paint_mid, decoder traces are excluded (handled by DecodeTracePass).
// In non-logic mode: all enabled traces (except Lissajous-skipped) go into
// the pixmap via paint_mid.
// ---------------------------------------------------------------------------

bool SignalPixmapPass::should_run(const RenderContext &ctx) const {
  return ctx.viewport && ctx.view && ctx.traces && !ctx.traces->empty();
}

void SignalPixmapPass::render(QPainter &p, const RenderContext &ctx) {
  Viewport *vp = ctx.viewport;
  View *view = ctx.view;
  const auto &traces = *ctx.traces;

  if (ctx.is_logic_mode) {
    // Determine if view parameters changed (requires full logic signal rebuild)
    bool view_params_changed =
        (view->scale() != vp->curScale() ||
         view->offset() != vp->curOffset() ||
         view->get_signalHeight() != vp->curSignalHeight() ||
         view->get_vOffset() != vp->curVOffset());

    const qreal dpr = vp->devicePixelRatioF();
    const QSize pixmapSize = (QSizeF(vp->size()) * dpr).toSize();
    // P1: "pixmap changed" = no published worker frame of the current size yet
    // (cold start / resize). Frames are matched by size+DPR only: during a
    // zoom/scroll (same size) the newest published frame is blitted even
    // though its view params are one frame stale, so the GUI thread never
    // blocks on waveform rasterization (the async frame catches up ~1 frame
    // later via on_frame_published). The RenderWorker triple buffer replaces
    // the old cached QPixmap for the logic layer.
    const bool pixmap_changed = !vp->render_worker().has_frame(pixmapSize, dpr);

    // P2: fixed-FPS gate for data-driven rebuilds. Interaction dirty sources
    // (view params / pixmap size changed -> zoom/scroll/resize/first paint)
    // always rebuild immediately. A purely data-driven need_update rebuilds at
    // most once per Viewport::PixmapRebuildIntervalMs, so streaming frames are
    // merged into a fixed cadence (~30 FPS, matching the RefreshRtTimer) instead
    // of rebuilding at up to 60 FPS. When the rate gate skips a data-driven
    // rebuild, need_update stays set so a later paint (interval elapsed, or any
    // interaction) completes it — the final capture frame is never lost.
    const bool interaction_dirty = view_params_changed || pixmap_changed;
    const bool decode_only_skip =
        ctx.decode_only && !interaction_dirty && !vp->need_update();
    const auto now = std::chrono::steady_clock::now();
    const bool rate_ok =
        (now - vp->last_pixmap_rebuild() >=
         std::chrono::milliseconds(Viewport::PixmapRebuildIntervalMs));
    const bool rebuild = !decode_only_skip &&
                         (interaction_dirty || (vp->need_update() && rate_ok));

    // P1: background rasterization of the static layer. The expensive waveform
    // rebuild is submitted to the RenderWorker (value-captured ops calling the
    // same rasterize_logic_channel pure function), published into the triple
    // buffer, and blitted here. A cold start (first paint / resize with no
    // completed frame yet) runs the same ops synchronously so the screen is
    // never blank; the worker publishes the async baseline right after.
    std::vector<std::function<void(QPainter &)>> built_ops;
    if (rebuild) {
      vp->curScale() = view->scale();
      vp->curOffset() = view->offset();
      vp->curSignalHeight() = view->get_signalHeight();
      vp->curVOffset() = view->get_vOffset();
      vp->last_pixmap_rebuild() = now;

      RenderWorker::Job job;
      job.size = pixmapSize;
      job.dpr = dpr;
      job.vOffset = view->get_vOffset();

      const PaintContext pctx = ctx.pctx;
      const double scale = pctx.scale;
      const int64_t offset = pctx.offset;
      bool bFirst = true;
      uint64_t end_align_sample = 0;

      for (auto t : traces) {
        if (!t->enabled())
          continue;
        if (auto *logic_signal = t->as_logic()) {
          if (bFirst && logic_signal->data())
            end_align_sample = logic_signal->data()->get_ring_sample_count();

          std::list<int> _index_list = t->get_index_list();
          int idx = *_index_list.begin() % 8;
          QString token = QString("@logic-channel-%1").arg(idx);
          QColor theme_color = AppConfig::Instance().GetThemeColor(token);
          if (!theme_color.isValid())
            theme_color = Viewport::PROBE_COLORS[idx];
          // Same FINAL pen colour the adapter computes:
          // _colour.isValid() ? _colour : fore.
          const QColor colour = logic_signal->get_colour().isValid()
                                    ? logic_signal->get_colour()
                                    : theme_color;

          auto snap = logic_signal->data_ref();
          const int channel_index =
              logic_signal->model() ? logic_signal->model()->index() : 0;
          const int right = t->get_view_rect().right();
          const int y = logic_signal->get_y() +
                        (int)(logic_signal->get_totalHeight() * 0.5);
          const int total_height = logic_signal->get_totalHeight();

          std::vector<GlitchRange> preview;
          if (const auto *pv_ranges = view->get_preview_ranges(logic_signal);
              pv_ranges && !pv_ranges->empty()) {
            preview.reserve(pv_ranges->size());
            for (const auto &pulse : *pv_ranges)
              preview.push_back({pulse.start, pulse.end});
          }

          job.ops.push_back(
              [snap, channel_index, right, y, total_height, colour, scale,
               offset, end_align_sample, pctx, preview](QPainter &qp) {
                if (!snap)
                  return;
                const std::vector<GlitchRange> *pp =
                    preview.empty() ? nullptr : &preview;
                rasterize_logic_channel(qp, snap.get(), channel_index, 0,
                                        right, y, total_height, colour, scale,
                                        offset, end_align_sample, pctx, pp);
              });
          bFirst = false;
        }
      }

      // Keep a copy of the ops ONLY for the cold-start sync fallback (no
      // published frame of this size yet). During zoom/scroll a stale frame is
      // blitted instead, so the copy is skipped (avoids per-frame vector copy).
      if (pixmap_changed)
        built_ops = job.ops;
      vp->render_pending() = true;
      vp->render_pending_seq() = vp->data_seq();
      vp->render_worker().submit(std::move(job));
    }

    // Blit the newest completed worker frame of this size (cheap: drawImage).
    // During interaction (zoom/scroll) the blitted frame may be one view-step
    // stale; the worker's async frame with the current params arrives ~1 frame
    // later and repaints. try_acquire returns false only when no frame of this
    // size is published yet (cold start / resize), in which case the rebuild
    // above is rendered synchronously so the screen is never blank.
    const QImage *img = nullptr;
    if (vp->render_worker().try_acquire(pixmapSize, dpr, &img)) {
      p.drawImage(0, 0, *img);
      vp->render_worker().release();
    } else if (rebuild) {
      // Cold start: render the SAME ops synchronously so the first frame of
      // this size is never blank.
      QImage local(pixmapSize, QImage::Format_ARGB32_Premultiplied);
      local.setDevicePixelRatio(dpr);
      local.fill(Qt::transparent);
      QPainter qp(&local);
      qp.translate(0, -view->get_vOffset());
      for (auto &op : built_ops)
        op(qp);
      p.drawImage(0, 0, local);
    }

    // Non-logic, non-decoder traces (analog/dso/math in MSO mode) paint
    // synchronously on top of the blit, in the same translated space they
    // occupied inside the old cached pixmap. Z-order is unchanged (different
    // signal rows never overlap).
    {
      bool has_non_logic = false;
      for (auto t : traces)
        if (t->enabled() && !t->as_logic() && !t->as_decode()) {
          has_non_logic = true;
          break;
        }
      if (has_non_logic) {
        p.save();
        p.translate(0, -view->get_vOffset());
        for (auto t : traces) {
          if (t->enabled() && !t->as_logic() && !t->as_decode())
            t->paint_mid(p, 0, t->get_view_rect().right(), ctx.fore,
                         ctx.back, ctx.pctx);
        }
        p.restore();
      }
    }
  } else {
    // Non-logic mode (DSO/analog)
    const qreal dpr = vp->devicePixelRatioF();
    const QSize pixmapSize = (QSizeF(vp->size()) * dpr).toSize();
    const bool pixmap_changed =
        vp->pixmap().isNull() ||
        vp->pixmap().size() != pixmapSize ||
        !qFuzzyCompare(vp->pixmap().devicePixelRatioF(), dpr);

    // P2: same fixed-FPS gate as the logic branch — interaction (view params /
    // pixmap size) rebuilds immediately; data-driven need_update rebuilds at
    // most once per Viewport::PixmapRebuildIntervalMs (DSO continuous mode is
    // additionally pre-throttled to 33ms upstream in ViewDataSync::data_updated).
    const bool interaction_dirty =
        (view->scale() != vp->curScale() ||
         view->offset() != vp->curOffset() ||
         view->get_signalHeight() != vp->curSignalHeight() ||
         view->get_vOffset() != vp->curVOffset() ||
         pixmap_changed);
    const auto now = std::chrono::steady_clock::now();
    const bool rate_ok =
        (now - vp->last_pixmap_rebuild() >=
         std::chrono::milliseconds(Viewport::PixmapRebuildIntervalMs));
    const bool rebuild =
        interaction_dirty || (vp->need_update() && rate_ok);

    if (rebuild) {

      vp->curScale() = view->scale();
      vp->curOffset() = view->offset();
      vp->curSignalHeight() = view->get_signalHeight();
      vp->curVOffset() = view->get_vOffset();

      // Reuse the existing QPixmap when size & DPR match (avoids heap
      // alloc/dealloc on every frame in DSO continuous mode).
      if (pixmap_changed) {
        vp->pixmap() = QPixmap(pixmapSize);
        vp->pixmap().setDevicePixelRatio(dpr);
      }
      vp->pixmap().fill(Qt::transparent);

      QPainter dbp(&vp->pixmap());
      dbp.translate(0, -view->get_vOffset());

      bool isLissa = false;

      if (view->get_work_mode() == DSO) {
        auto lis_trace = view->get_own_lissajous_trace();
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
          t->paint_mid(dbp, 0, t->get_view_rect().right(), ctx.fore,
                       ctx.back, ctx.pctx);
        }
      }
      vp->need_update() = false;
      vp->last_pixmap_rebuild() = now;
    }
    p.drawPixmap(0, 0, vp->pixmap());
  }
}

// ---------------------------------------------------------------------------
// DecodeTracePass — renders decode traces directly on the widget (not via
// QPixmap) to ensure crisp text rendering. Only runs in logic mode; in
// non-logic mode, decoder traces are included in the cached pixmap.
// ---------------------------------------------------------------------------

bool DecodeTracePass::should_run(const RenderContext &ctx) const {
  if (!ctx.traces || !ctx.is_logic_mode)
    return false;
  for (auto t : *ctx.traces) {
    if (t->enabled() && t->signal_type() == SR_CHANNEL_DECODER)
      return true;
  }
  return false;
}

void DecodeTracePass::render(QPainter &p, const RenderContext &ctx) {
  if (!ctx.traces)
    return;

  p.save();
  p.translate(0, -ctx.vOffset);

  for (auto t : *ctx.traces) {
    if (t->enabled() && t->signal_type() == SR_CHANNEL_DECODER) {
      t->paint_mid(p, 0, t->get_view_rect().right(), ctx.fore, ctx.back, ctx.pctx);
    }
  }
  p.restore();
}

// ---------------------------------------------------------------------------
// CursorOverlayPass — renders regular cursors, xcursors, trigger cursor,
// and search cursor on top of all signal content.
// ---------------------------------------------------------------------------

bool CursorOverlayPass::should_run(const RenderContext &ctx) const {
  if (ctx.type != TIME_VIEW || !ctx.viewport || !ctx.view)
    return false;
  // Skip entirely if no cursor type is visible — avoids entering render()
  // (which does multiple if-branch checks) on every paint frame.
  View *view = ctx.view;
  return view->cursors_shown() || view->xcursors_shown() ||
         view->trig_cursor_shown() || view->search_cursor_shown();
}

void CursorOverlayPass::render(QPainter &p, const RenderContext &ctx) {
  View *view = ctx.view;
  const QRect xrect = view->get_view_rect();
  const QPoint &hover = view->hover_point();

  // 1. Regular cursors
  if (view->cursors_shown()) {
    auto &cursor_list = view->get_cursorList();
    for (auto &cursor : cursor_list) {
      const int64_t cursorX = view->index2pixel(cursor->index());
      if (xrect.contains(hover.x(), hover.y()) &&
          qAbs(cursorX - hover.x()) <= Viewport::HitCursorMargin)
        cursor->paint(p, xrect, 1,
                      view->session().is_stopped_status());
      else
        cursor->paint(p, xrect, 0,
                      view->session().is_stopped_status());
    }
  }

  // 2. X-cursors
  if (view->xcursors_shown()) {
    auto &xcursor_list = view->get_xcursorList();
    auto i = xcursor_list.begin();
    bool hovered = false;

    while (i != xcursor_list.end()) {
      const double cursorX =
          xrect.left() + (*i)->value(XCursor::XCur_Y) * xrect.width();
      const double cursorY0 =
          xrect.top() + (*i)->value(XCursor::XCur_X0) * xrect.height();
      const double cursorY1 =
          xrect.top() + (*i)->value(XCursor::XCur_X1) * xrect.height();

      if (!hovered &&
          ((*i)->get_close_rect(xrect).contains(hover) ||
           (*i)->get_map_rect(xrect).contains(hover))) {
        (*i)->paint(p, xrect, XCursor::XCur_All);
        hovered = true;
      } else if (!hovered && xrect.contains(hover)) {
        if (qAbs(cursorX - hover.x()) <= Viewport::HitCursorMargin &&
hover.y() > std::min(cursorY0, cursorY1) &&
hover.y() < std::max(cursorY0, cursorY1)) {
          (*i)->paint(p, xrect, XCursor::XCur_Y);
          hovered = true;
        } else if (qAbs(cursorY0 - hover.y()) <=
                   Viewport::HitCursorMargin) {
          (*i)->paint(p, xrect, XCursor::XCur_X0);
          hovered = true;
        } else if (qAbs(cursorY1 - hover.y()) <=
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
    }
  }

  // 3. Trigger cursor
  if (view->trig_cursor_shown()) {
    view->get_trig_cursor()->paint(p, xrect, 0, false);
  }

  // 4. Search cursor
  if (view->search_cursor_shown()) {
    const int64_t searchX =
        view->index2pixel(view->get_search_cursor()->index());
    if (xrect.contains(hover.x(), hover.y()) &&
        qAbs(searchX - hover.x()) <= Viewport::HitCursorMargin)
      view->get_search_cursor()->paint(p, xrect, 1, -1);
    else
      view->get_search_cursor()->paint(p, xrect, 0, -1);
  }
}

// ---------------------------------------------------------------------------
// MeasureOverlayPass — renders measurement overlays: logic frequency arrows,
// DSO hover lines, DSO Y-measure, DSO X-measure, logic edge/jump markers.
// ---------------------------------------------------------------------------

bool MeasureOverlayPass::should_run(const RenderContext &ctx) const {
  if (ctx.type != TIME_VIEW || !ctx.viewport || !ctx.view)
    return false;
  // Skip entirely if no measurement mode is active — avoids entering
  // render() (which checks 6 separate if-branches) on every paint frame.
  Viewport *vp = ctx.viewport;
  return vp->measure_en() || vp->action_type() != NO_ACTION ||
         vp->dso_ym_valid() || vp->dso_xm_valid() ||
         vp->measure_type() != NO_MEASURE;
}

void MeasureOverlayPass::draw_logic_freq(QPainter &p,
                                           const RenderContext &ctx,
                                           const MeasureCtx &m) {
  Viewport *vp = m.vp;
  if (!(vp->action_type() == NO_ACTION && vp->measure_type() == LOGIC_FREQ))
    return;

  p.setPen(m.active_color);
  p.drawLine(QLineF(vp->cur_preX(), m.screen_midY,
                    vp->cur_aftX(), m.screen_midY));
  p.drawLine(QLineF(vp->cur_preX(), m.screen_midY, vp->cur_preX() + 2,
                    m.screen_midY - 2));
  p.drawLine(QLineF(vp->cur_preX(), m.screen_midY, vp->cur_preX() + 2,
                    m.screen_midY + 2));
  p.drawLine(QLineF(vp->cur_aftX() - 2, m.screen_midY - 2,
                    vp->cur_aftX(), m.screen_midY));
  p.drawLine(QLineF(vp->cur_aftX() - 2, m.screen_midY + 2,
                    vp->cur_aftX(), m.screen_midY));
  if (vp->thd_sample() != 0) {
    p.drawLine(QLineF(vp->cur_aftX(), m.screen_midY, vp->cur_thdX(),
                      m.screen_midY));
    p.drawLine(QLineF(vp->cur_aftX(), m.screen_midY, vp->cur_aftX() + 2,
                      m.screen_midY - 2));
    p.drawLine(QLineF(vp->cur_aftX(), m.screen_midY, vp->cur_aftX() + 2,
                      m.screen_midY + 2));
    p.drawLine(QLineF(vp->cur_thdX() - 2, m.screen_midY - 2,
                      vp->cur_thdX(), m.screen_midY));
    p.drawLine(QLineF(vp->cur_thdX() - 2, m.screen_midY + 2,
                      vp->cur_thdX(), m.screen_midY));
  }

  if (vp->measure_en()) {
    std::vector<std::pair<QString, QString>> rows = {
        {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FREQUENCY), "Frequency: "),
         vp->mm_freq()},
        {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PERIOD), "Period: "),
         vp->mm_period()},
        {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_WIDTH), "Width: "),
         vp->mm_width()},
        {L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DUTY_CYCLE), "Duty Cycle: "),
         vp->mm_duty()}};

    drawFloatingPanel(p, m.screen_hover_point,
                      m.view->get_view_width(),
                      m.view->viewport()->height(), ctx.back,
                      vp->panelBgColor(), vp->panelTextColor(),
                      rows);
  }
}

void MeasureOverlayPass::draw_dso_hover_lines(QPainter &p,
                                                const RenderContext &ctx,
                                                const MeasureCtx &m) {
  Viewport *vp = m.vp;
  if (!(vp->action_type() == NO_ACTION && vp->measure_type() == DSO_VALUE))
    return;

  for (auto &s : m.view->get_own_signals()) {
    if (auto *dsoSig = s->as_dso()) {
      uint64_t index;
      double value;
      QPointF hpoint;
      if (dsoSig->get_hover(index, hpoint, value)) {
        p.setPen(QPen(ctx.fore, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawLine(hpoint.x(), s->get_view_rect().top(), hpoint.x(),
                   s->get_view_rect().bottom());
      }
    } else if (auto *analogSig = s->as_analog()) {
      uint64_t index;
      double value;
      QPointF hpoint;
      if (analogSig->get_hover(index, hpoint, value)) {
        p.setPen(QPen(ctx.fore, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawLine(hpoint.x(), s->get_view_rect().top(), hpoint.x(),
                   s->get_view_rect().bottom());
      }
    }
  }
}

void MeasureOverlayPass::draw_dso_y_measure(QPainter &p,
                                              const RenderContext &ctx,
                                              const MeasureCtx &m) {
  (void)ctx;
  Viewport *vp = m.vp;
  if (!vp->dso_ym_valid())
    return;

  for (auto &s : m.view->get_own_signals()) {
    if (auto *dsoSig = s->as_dso()) {
      if (dsoSig->get_index() == vp->dso_ym_sig_index()) {
        p.setPen(QPen(dsoSig->get_colour(), 1, Qt::DotLine));
        QFontMetrics fm(p.font());
        const int text_height = fm.height();
        const int64_t x = m.view->index2pixel(vp->dso_ym_index());
        p.drawLine(x - 10, vp->dso_ym_start(), x + 10,
                   vp->dso_ym_start());
        p.drawLine(x, vp->dso_ym_start(), x, vp->dso_ym_end());
        p.drawLine(0, vp->dso_ym_end(),
                   m.view->get_view_width(),
                   vp->dso_ym_end());

        double hrate = (vp->dso_ym_start() - vp->dso_ym_end()) *
                       1.0f / m.view->get_view_height();
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
                         abs(vp->dso_ym_start() +
                              vp->dso_ym_end()) / 2,
                         value_rect_width, text_height),
                   value_str);

        value_str = abs(vp->dso_ym_sig_value()) > 1000
                        ? QString::number(
                              vp->dso_ym_sig_value() / 1000.0, 'f', 2) +
                              "V"
                        : QString::number(vp->dso_ym_sig_value(), 'f',
                                          2) +
                              "mV";
        value_rect_width = p.boundingRect(
                               0, 0, INT_MAX, INT_MAX,
                               Qt::AlignLeft | Qt::AlignVCenter, value_str)
                               .width();
        int str_y = value > 0 ? vp->dso_ym_start()
                              : vp->dso_ym_start() - text_height;
        p.drawText(QRect(x - 0.5 * value_rect_width, str_y,
                         value_rect_width, text_height),
                   value_str);

        double end_value = vp->dso_ym_sig_value() + value;
        value_str = abs(end_value) > 1000
                        ? QString::number(end_value / 1000.0, 'f', 2) + "V"
                        : QString::number(end_value, 'f', 2) + "mV";
        value_rect_width = p.boundingRect(
                               0, 0, INT_MAX, INT_MAX,
                               Qt::AlignLeft | Qt::AlignVCenter, value_str)
                               .width();
        str_y = value > 0 ? vp->dso_ym_end() - text_height
                          : vp->dso_ym_end();
        p.drawText(QRect(x - 0.5 * value_rect_width, str_y,
                         value_rect_width, text_height),
                   value_str);
        break;
      }
    }
  }
}

void MeasureOverlayPass::draw_dso_x_measure(QPainter &p,
                                              const RenderContext &ctx,
                                              const MeasureCtx &m) {
  (void)ctx;
  Viewport *vp = m.vp;
  if (!vp->dso_xm_valid())
    return;

  p.setPen(QPen(Qt::red, 1, Qt::DotLine));
  int measure_line_count = 6;
  const int text_height =
      p.boundingRect(0, 0, INT_MAX, INT_MAX, Qt::AlignLeft | Qt::AlignTop,
                     "W")
          .height();
  auto *src = m.view->document_snapshot_source();
  if (!src)
      return;
  const uint64_t sample_rate = src->cur_snap_samplerate();
  std::vector<QLineF> measure_lines_vec(measure_line_count);
  QLineF *const measure_lines = measure_lines_vec.data();
  QLineF *line = measure_lines;
  int64_t x[Viewport::DsoMeasureStages];
  int dso_xm_stage = 0;
  if (vp->action_type() == DSO_XM_STEP1)
    dso_xm_stage = 1;
  else if (vp->action_type() == DSO_XM_STEP2)
    dso_xm_stage = 2;
  else
    dso_xm_stage = 3;

  for (int i = 0; i < dso_xm_stage; i++) {
    x[i] = m.view->index2pixel(vp->dso_xm_index()[i]);
  }
  measure_line_count = 0;
  if (dso_xm_stage > 0) {
    *line++ = QLine(x[0], vp->dso_xm_y() - 10, x[0],
                    vp->dso_xm_y() + 10);
    measure_line_count += 1;
  }
  if (dso_xm_stage > 1) {
    *line++ = QLine(x[1], vp->dso_xm_y() - 10, x[1],
                    vp->dso_xm_y() + 10);
    *line++ = QLine(x[0], vp->dso_xm_y(), x[1], vp->dso_xm_y());
    vp->mm_width() = m.view->get_ruler()->format_real_time(
        vp->dso_xm_index()[1] - vp->dso_xm_index()[0],
        sample_rate);

    const QString w_ctr = "W=" + vp->mm_width();
    int w_rect_width = p.boundingRect(
                           0, 0, INT_MAX, INT_MAX,
                           Qt::AlignLeft | Qt::AlignVCenter, w_ctr)
                           .width();
    p.drawText(QRect(x[0] + 10, vp->dso_xm_y() - text_height,
                     w_rect_width, text_height),
               w_ctr);
    measure_line_count += 2;
  }
  if (dso_xm_stage > 2) {
    *line++ = QLineF(x[0], vp->dso_xm_y() + 20, x[0],
                     vp->dso_xm_y() + 40);
    *line++ = QLineF(x[0], vp->dso_xm_y() + 30, x[2],
                     vp->dso_xm_y() + 30);
    *line++ = QLineF(x[2], vp->dso_xm_y() + 20, x[2],
                     vp->dso_xm_y() + 40);
    vp->mm_period() = m.view->get_ruler()->format_real_time(
        vp->dso_xm_index()[2] - vp->dso_xm_index()[0],
        sample_rate);
    vp->mm_freq() = m.view->get_ruler()->format_real_freq(
        vp->dso_xm_index()[2] - vp->dso_xm_index()[0],
        sample_rate);
    vp->mm_duty() =
        QString::number((vp->dso_xm_index()[1] -
                          vp->dso_xm_index()[0]) *
                             100.0 /
                             (vp->dso_xm_index()[2] -
                              vp->dso_xm_index()[0]),
                         'f', 2) +
        "%";

    const QString p_ctr = "P=" + vp->mm_period();
    int p_rect_width = p.boundingRect(
                           0, 0, INT_MAX, INT_MAX,
                           Qt::AlignLeft | Qt::AlignVCenter, p_ctr)
                           .width();
    p.drawText(QRect(x[0] + 10, vp->dso_xm_y() + 30 - text_height,
                     p_rect_width, text_height),
               p_ctr);

    const QString f_ctr = "F=" + vp->mm_freq();
    int f_rect_width = p.boundingRect(
                           0, 0, INT_MAX, INT_MAX,
                           Qt::AlignLeft | Qt::AlignVCenter, f_ctr)
                           .width();
    p.drawText(QRect(x[0] + 20 + p_rect_width,
                     vp->dso_xm_y() + 30 - text_height, f_rect_width,
                     text_height),
               f_ctr);

    const QString d_ctr = "D=" + vp->mm_duty();
    int d_rect_width = p.boundingRect(
                           0, 0, INT_MAX, INT_MAX,
                           Qt::AlignLeft | Qt::AlignVCenter, d_ctr)
                           .width();
    p.drawText(QRect(x[1] + 10, vp->dso_xm_y() - 0.5 * text_height,
                     d_rect_width, text_height),
               d_ctr);

    measure_line_count += 3;
  }
  p.drawLines(measure_lines, static_cast<int>(measure_line_count));
  if (dso_xm_stage < Viewport::DsoMeasureStages) {
    p.drawLine(x[dso_xm_stage - 1], vp->dso_xm_y(),
               vp->mouse_point().x(), vp->dso_xm_y());
    p.drawLine(vp->mouse_point().x(), 0,
               vp->mouse_point().x(), vp->height());
  }
  vp->measure_updated();
}

void MeasureOverlayPass::draw_logic_edge(QPainter &p,
                                           const RenderContext &ctx,
                                           const MeasureCtx &m) {
  Viewport *vp = m.vp;
  if (!(vp->action_type() == LOGIC_EDGE &&
        m.view->session().have_view_data()))
    return;

  p.setPen(m.active_color);
  p.drawLine(
      QLineF(vp->cur_preX(), m.screen_midY - 5, vp->cur_preX(),
             m.screen_midY + 5));
  p.drawLine(
      QLineF(vp->cur_aftX(), m.screen_midY - 5, vp->cur_aftX(),
             m.screen_midY + 5));
  p.drawLine(QLineF(vp->cur_preX(), m.screen_midY, vp->cur_aftX(),
                    m.screen_midY));

  std::vector<std::pair<QString, QString>> rows = {{"", vp->em_edges()},
                                       {"", vp->em_rising()},
                                       {"", vp->em_falling()}};

  drawFloatingPanel(p, m.screen_hover_point,
                    m.view->get_view_width(),
                    m.view->viewport()->height(), ctx.back,
                    vp->panelBgColor(), vp->panelTextColor(),
                    rows);
}

void MeasureOverlayPass::draw_logic_jump(QPainter &p,
                                           const RenderContext &ctx,
                                           const MeasureCtx &m) {
  Viewport *vp = m.vp;
  if (vp->action_type() != LOGIC_JUMP)
    return;

  p.setPen(m.active_color);
  p.setBrush(Qt::NoBrush);
  const QPointF pre_points[] = {
      QPointF(vp->cur_preX(), m.screen_preY),
      QPointF(vp->cur_preX() - 1, m.screen_preY - 1),
      QPointF(vp->cur_preX() + 1, m.screen_preY - 1),
      QPointF(vp->cur_preX() - 1, m.screen_preY + 1),
      QPointF(vp->cur_preX() + 1, m.screen_preY + 1),
      QPointF(vp->cur_preX() - 2, m.screen_preY - 2),
      QPointF(vp->cur_preX() + 2, m.screen_preY - 2),
      QPointF(vp->cur_preX() - 2, m.screen_preY + 2),
      QPointF(vp->cur_preX() + 2, m.screen_preY + 2),
  };
  p.drawPoints(pre_points, countof(pre_points));
  if (std::abs(vp->cur_aftX() - vp->cur_preX()) +
          std::abs((double)(vp->cur_aftY() - vp->cur_preY())) >
      20) {
    if (vp->edge_hit()) {
      const QPointF aft_points[] = {
          QPointF(vp->cur_aftX(), m.screen_aftY),
          QPointF(vp->cur_aftX() - 1, m.screen_aftY - 1),
          QPointF(vp->cur_aftX() + 1, m.screen_aftY - 1),
          QPointF(vp->cur_aftX() - 1, m.screen_aftY + 1),
          QPointF(vp->cur_aftX() + 1, m.screen_aftY + 1),
          QPointF(vp->cur_aftX() - 2, m.screen_aftY - 2),
          QPointF(vp->cur_aftX() + 2, m.screen_aftY - 2),
          QPointF(vp->cur_aftX() - 2, m.screen_aftY + 2),
          QPointF(vp->cur_aftX() + 2, m.screen_aftY + 2),
      };
      p.drawPoints(aft_points, countof(aft_points));
    }
    int64_t delta = std::max(vp->edge_start(), vp->edge_end()) -
                    std::min(vp->edge_start(), vp->edge_end());
    QString delta_text =
        m.view->get_index_delta(vp->edge_start(),
                                vp->edge_end()) +
        "/" + QString::number(delta);

    std::vector<std::pair<QString, QString>> rows = {{"", delta_text}};

    drawFloatingPanel(p, m.screen_hover_point,
                      m.view->get_view_width(),
                      m.view->viewport()->height(), ctx.back,
                      vp->panelBgColor(), vp->panelTextColor(),
                      rows);

    QPainterPath path(QPointF(vp->cur_preX(), m.screen_preY));
    QPointF c1((vp->cur_preX() + vp->cur_aftX()) / 2.0,
              m.screen_preY);
    QPointF c2((vp->cur_preX() + vp->cur_aftX()) / 2.0,
              m.screen_aftY);
    path.cubicTo(c1, c2, QPointF(vp->cur_aftX(), m.screen_aftY));
    p.drawPath(path);
  }
}

static QString zbFormatDecodedDuration(double captureSamples, uint64_t sampleRate) {
  if (!std::isfinite(captureSamples) || captureSamples < 0.0 || sampleRate == 0)
    return QStringLiteral("--");
  const double seconds = captureSamples / static_cast<double>(sampleRate);
  const double mag = std::abs(seconds);
  double scale = 1.0;
  const char *suffix = "s";
  if (mag < 1e-9) { scale = 1e12; suffix = "ps"; }
  else if (mag < 1e-6) { scale = 1e9; suffix = "ns"; }
  else if (mag < 1e-3) { scale = 1e6; suffix = "us"; }
  else if (mag < 1.0) { scale = 1e3; suffix = "ms"; }
  return QString::number(seconds * scale, 'g', 7) + " " + suffix;
}

void MeasureOverlayPass::draw_decoder_analog_hover(
    QPainter &p, const RenderContext &ctx, const MeasureCtx &m) {
  Viewport *vp = m.vp;
  if (!vp || vp->action_type() != NO_ACTION ||
      ctx.type != TIME_VIEW || !ctx.is_logic_mode || !ctx.traces)
    return;

  const QPoint hover = m.view->hover_point();
  const uint64_t capture_sample = m.view->pixel2index(hover.x());
  for (Trace *trace : *ctx.traces) {
    DecodeTrace *dt = trace ? trace->as_decode() : nullptr;
    if (!dt || !dt->enabled()) continue;

    int channel = -1;
    pv::data::DecoderAnalogSample sample{};
    double engineering = 0.0;
    std::string unit;
    QPointF sample_point;
    QRectF row_rect;
    if (!dt->get_analog_hover(hover.x(), hover.y(), m.v_offset,
                              capture_sample, channel, sample, engineering,
                              unit, sample_point, row_rect))
      continue;

    const QColor c = DecodeTrace::getChannelColor(channel % 16);
    p.setPen(QPen(c, 1, Qt::DashLine));
    p.setBrush(c);
    p.drawLine(QPointF(sample_point.x(), row_rect.top()),
               QPointF(sample_point.x(), row_rect.bottom()));
    p.drawEllipse(sample_point, 3.5, 3.5);

    auto *src = m.view->document_snapshot_source();
    const uint64_t sr = src ? src->cur_snap_samplerate() : 1;
    const QString timeText = Ruler::format_real_time(sample.start_sample, sr) +
                             " / " + QString::number(sample.start_sample);
    const QString engText = QString::number(engineering, 'g', 7) +
        (unit.empty() ? QString() : " " + QString::fromStdString(unit));
    const AnalogMeasurementV2Options &options = vp->analog_measure_options();
    std::vector<std::pair<QString, QString>> rows;
    if (options.show_channel)
      rows.push_back({QStringLiteral("通道"), QString("DecCh%1").arg(channel)});
    if (options.show_time)
      rows.push_back({QStringLiteral("时间/采样点"), timeText});
    if (options.show_normalized)
      rows.push_back({QStringLiteral("归一化值"),
                      QString::number(sample.value, 'g', 7)});
    if (options.show_engineering_value)
      rows.push_back({QStringLiteral("工程值"), engText});
    if (rows.empty())
      break;
    drawFloatingPanel(p, m.screen_hover_point, m.view->get_view_width(),
                      m.view->viewport()->height(), ctx.back,
                      vp->panelBgColor(), vp->panelTextColor(), rows);
    break;
  }
}

void MeasureOverlayPass::draw_decoder_analog_range(
    QPainter &p, const RenderContext &ctx, const MeasureCtx &m) {
  Viewport *vp = m.vp;
  if (!vp || !vp->analog_measure_valid() || !vp->analog_measure_data() ||
      ctx.type != TIME_VIEW || !ctx.is_logic_mode || !ctx.traces)
    return;

  DecodeTrace *owner = nullptr;
  QRectF row_rect;
  for (Trace *trace : *ctx.traces) {
    DecodeTrace *dt = trace ? trace->as_decode() : nullptr;
    if (!dt || !dt->enabled()) continue;
    const auto channels = dt->decoder()->analog_data_copy();
    if (std::find(channels.begin(), channels.end(), vp->analog_measure_data()) == channels.end())
      continue;
    if (dt->get_analog_channel_rect(vp->analog_measure_channel(), m.v_offset, row_rect)) {
      owner = dt;
      break;
    }
  }
  if (!owner) return;

  const uint64_t a = std::min(vp->analog_measure_start(), vp->analog_measure_end());
  const uint64_t b = std::max(vp->analog_measure_start(), vp->analog_measure_end());
  const qreal x0 = m.view->index2pixel(a);
  const qreal x1 = m.view->index2pixel(b);
  const QColor c = DecodeTrace::getChannelColor(vp->analog_measure_channel() % 16);

  QRectF sel(QPointF(std::min(x0, x1), row_rect.top()),
             QPointF(std::max(x0, x1), row_rect.bottom()));
  QRectF clipped = sel.intersected(QRectF(m.view->get_view_rect()));
  if (clipped.isValid()) {
    QColor fill = c; fill.setAlpha(38); p.fillRect(clipped, fill);
  }
  p.setPen(QPen(c, 1.5, Qt::DashLine));
  p.drawLine(QPointF(x0, row_rect.top()), QPointF(x0, row_rect.bottom()));
  p.drawLine(QPointF(x1, row_rect.top()), QPointF(x1, row_rect.bottom()));
  p.setPen(Qt::NoPen); p.setBrush(c);
  p.drawEllipse(QPointF(x0, row_rect.top() + 5.0), 3.5, 3.5);
  p.drawEllipse(QPointF(x1, row_rect.top() + 5.0), 3.5, 3.5);

  auto *src = m.view->document_snapshot_source();
  const uint64_t sr = src ? src->cur_snap_samplerate() : 1;
  std::vector<std::pair<QString, QString>> rows;
  rows.push_back({QStringLiteral("通道"), QString("DecCh%1").arg(vp->analog_measure_channel())});
  rows.push_back({QStringLiteral("区间"), Ruler::format_real_time(b - a, sr)});

  const auto &st = vp->analog_measure_stats();
  const auto data = vp->analog_measure_data();
  if (st.valid && data) {
    const double lo = data->engineering_minimum();
    const double hi = data->engineering_maximum();
    const double gain = (hi - lo) * 0.5;
    const double offset = (hi + lo) * 0.5;
    auto eng = [gain, offset](double n) { return gain * n + offset; };
    const double vmin = eng(st.minimum);
    const double vmax = eng(st.maximum);
    const double avg = eng(st.mean);
    const double rms2 = gain * gain * st.rms * st.rms +
                        2.0 * gain * offset * st.mean + offset * offset;
    const double rms = std::sqrt(std::max(0.0, rms2));
    const QString unit = QString::fromStdString(data->engineering_unit());
    auto txt = [&unit](double v) {
      return QString::number(v, 'g', 7) + (unit.isEmpty() ? QString() : " " + unit);
    };
    rows.push_back({QStringLiteral("采样数"), QString::number(st.sample_count)});
    rows.push_back({QStringLiteral("最小值"), txt(vmin)});
    rows.push_back({QStringLiteral("最大值"), txt(vmax)});
    rows.push_back({QStringLiteral("峰峰值"), txt(vmax - vmin)});
    rows.push_back({QStringLiteral("平均值"), txt(avg)});
    rows.push_back({QStringLiteral("RMS"), txt(rms)});

    if (st.sample_count > 1 && sr > 0 && st.last_sample > st.first_sample) {
      const double decoded_rate =
          (st.sample_count - 1.0) * static_cast<double>(sr) /
          static_cast<double>(st.last_sample - st.first_sample);
      if (decoded_rate > 0.0)
        rows.push_back({QStringLiteral("解码采样率"),
                        Ruler::format_freq(1.0 / decoded_rate)});
    }

    const auto &cy = vp->analog_measure_cycle();
    const AnalogMeasurementV2Options &options = vp->analog_measure_options();
    const QString unavailable = QStringLiteral("--");
    if (options.rise_time)
      rows.push_back({QStringLiteral("上升时间"), cy.rise_valid
          ? zbFormatDecodedDuration(cy.rise_samples, sr) : unavailable});
    if (options.fall_time)
      rows.push_back({QStringLiteral("下降时间"), cy.fall_valid
          ? zbFormatDecodedDuration(cy.fall_samples, sr) : unavailable});
    if (options.positive_overshoot)
      rows.push_back({QStringLiteral("正过冲"), cy.overshoot_valid
          ? QString::number(cy.positive_overshoot, 'g', 6) + " %"
          : unavailable});
    if (options.negative_overshoot)
      rows.push_back({QStringLiteral("负过冲"), cy.overshoot_valid
          ? QString::number(cy.negative_overshoot, 'g', 6) + " %"
          : unavailable});
    if (options.period)
      rows.push_back({QStringLiteral("周期"), cy.time_valid
          ? zbFormatDecodedDuration(cy.period_samples, sr) : unavailable});
    if (options.frequency)
      rows.push_back({QStringLiteral("频率"), cy.time_valid && sr > 0
          ? Ruler::format_freq(cy.period_samples / static_cast<double>(sr))
          : unavailable});
    if (options.positive_width)
      rows.push_back({QStringLiteral("正脉宽"), cy.time_valid
          ? zbFormatDecodedDuration(cy.positive_width_samples, sr)
          : unavailable});
    if (options.negative_width)
      rows.push_back({QStringLiteral("负脉宽"), cy.time_valid
          ? zbFormatDecodedDuration(cy.negative_width_samples, sr)
          : unavailable});
    if (options.positive_duty_cycle)
      rows.push_back({QStringLiteral("正占空比"), cy.time_valid
          ? QString::number(cy.positive_duty_cycle, 'g', 6) + " %"
          : unavailable});
    if (options.negative_duty_cycle)
      rows.push_back({QStringLiteral("负占空比"), cy.time_valid
          ? QString::number(cy.negative_duty_cycle, 'g', 6) + " %"
          : unavailable});
    if (options.cycle_rms) {
      QString cycleRms = unavailable;
      if (cy.cycle_rms_valid) {
        const double crms2 = gain * gain * cy.cycle_rms * cy.cycle_rms +
                             2.0 * gain * offset * cy.cycle_mean +
                             offset * offset;
        cycleRms = txt(std::sqrt(std::max(0.0, crms2)));
      }
      rows.push_back({QStringLiteral("整周期 RMS"), cycleRms});
    }
  } else {
    rows.push_back({QStringLiteral("测量"), QStringLiteral("拖动中…")});
  }

  const qreal anchor = std::clamp(std::max(x0, x1), 0.0,
                                  static_cast<double>(m.view->get_view_width()));
  drawFloatingPanel(p, QPointF(anchor, row_rect.center().y()),
                    m.view->get_view_width(), m.view->viewport()->height(),
                    ctx.back, vp->panelBgColor(), vp->panelTextColor(), rows);
}

void MeasureOverlayPass::render(QPainter &p, const RenderContext &ctx) {
  Viewport *vp = ctx.viewport;
  View *view = ctx.view;

  MeasureCtx m;
  m.vp = vp;
  m.view = view;
  m.active_color = ctx.back.black() > 0x80 ? View::Orange : View::Purple;
  m.v_offset = view->get_vOffset();
  m.screen_midY = vp->cur_midY() - m.v_offset;
  m.screen_preY = vp->cur_preY() - m.v_offset;
  m.screen_aftY = vp->cur_aftY() - m.v_offset;
  m.screen_hover_point = view->hover_point() - QPointF(0, m.v_offset);

  vp->hover_hit() = false;

  draw_logic_freq(p, ctx, m);
  draw_dso_hover_lines(p, ctx, m);
  draw_dso_y_measure(p, ctx, m);
  draw_dso_x_measure(p, ctx, m);
draw_logic_edge(p, ctx, m);
draw_logic_jump(p, ctx, m);
draw_decoder_analog_hover(p, ctx, m);
draw_decoder_analog_range(p, ctx, m);
}

// ---------------------------------------------------------------------------
// TriggerInfoPass — renders DSO trigger status text and out-of-range warning.
// ---------------------------------------------------------------------------

bool TriggerInfoPass::should_run(const RenderContext &ctx) const {
  if (ctx.type != TIME_VIEW || !ctx.viewport || !ctx.view)
    return false;
  auto *dev = ctx.view->data_source()->device();
  return ctx.view->get_work_mode() == DSO &&
         ctx.view->session().is_running_status() && dev &&
         dev->is_dsl_device();
}

void TriggerInfoPass::render(QPainter &p, const RenderContext &ctx) {
  Viewport *vp = ctx.viewport;
  View *view = ctx.view;

  auto *dev = view->data_source()->device();
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

      if (view->session().is_instant()) {
        type_str += ", ";
        type_str += L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VIEW_CAPTURE),
                        "Capturing");
        bDot = true;
      }
    } else if (type == DSO_TRIGGER_AUTO &&
               !view->session().trigd()) {
      type_str = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO), "Auto");

      if (view->session().is_instant()) {
        type_str += ", ";
        type_str += L_S(STR_PAGE_DLG, S_ID(IDS_DLG_VIEW_CAPTURE),
                        "Capturing");
        bDot = true;
      }
    } else if (vp->waiting_trig() > 0) {
      type_str = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_WAITING_TRIG),
                     "Waiting Trig");
      bDot = true;
    } else {
      type_str = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIG_D), "Trig'd");
    }

    if (bDot) {
      for (int i = 0; i < vp->tigger_wait_times(); i++) {
        type_str += ".";
      }

      high_resolution_clock::time_point cur_time =
          high_resolution_clock::now();
      milliseconds timeInterval = std::chrono::duration_cast<milliseconds>(
          cur_time - vp->lst_wait_tigger_time());
      int64_t time_keep = timeInterval.count();

      if (time_keep >= 500) {
        vp->tigger_wait_times()++;
        vp->lst_wait_tigger_time() = cur_time;
      }

      if (vp->tigger_wait_times() > 4)
        vp->tigger_wait_times() = 0;
    }
  }
  p.setPen(ctx.fore);
  p.drawText(view->get_view_rect(),
             Qt::AlignLeft | Qt::AlignTop, type_str);

  if (dev->is_hardware()) {
    if (view->session().dso_data_is_out_off_range()) {
      QString data_status = L_S(STR_PAGE_DLG,
                                S_ID(IDS_DLG_DATA_OUT_OFF_RANGE),
                                "Out off range");
      data_status += "! ";
      QColor warnRed = AppConfig::Instance().GetThemeColor("@warn-red");
      if (!warnRed.isValid())
        warnRed = QColor(255, 0, 0, 200);
      p.setPen(warnRed);
      p.drawText(view->get_view_rect(),
                 Qt::AlignRight | Qt::AlignTop, data_status);
      p.setPen(ctx.fore);
    }
  }
}

} // namespace view
} // namespace pv
