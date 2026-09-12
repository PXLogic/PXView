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

/*
 * Task C2 (plan-core-view-split-and-mcp-coverage): cursor *position state*
 * has been migrated to the Core layer. The authoritative store is now
 * pv::core::CursorRegistry, owned by SessionStateContext. The View and
 * MCP API both read/write positions through the DataSource interface
 * (get_cursors / add_cursor / remove_cursor / set_cursor_position /
 * clear_cursors), which SigSession overrides to forward to the registry.
 *
 * view::Cursor remains a pure rendering object — it holds the QPainter
 * geometry, label text, colour (via Ruler::GetColorByCursorOrder), and
 * the close-button hit-test. Its sample position is inherited from
 * TimeMarker::_index, which is kept in sync with the Core CursorEntry
 * via ViewCursors::sync_cursor_position_to_core() on drag-release, and
 * via ViewCursors::sync_cursors_from_core() on data-source binding
 * (covers the headless -> GUI transition where MCP added cursors before
 * the View existed).
 *
 * Visual-only fields (_order, _text_size, colour) are NOT mirrored in
 * Core — they are presentation details with no MCP/API relevance.
 */

#include "pv/view/cursor/cursor.h"

// Task 3.3: widget-free — cursor.cpp moved from gui_sources to pxview-render
// (same pattern as timemarker/xcursor in Task 3.2). View/Ruler access now
// goes through IRenderView / ruler_format's free functions:
//   * View::width()                      -> IRenderView::get_view_width()
//   * View::LabelPadding                 -> local constant copy (values equal)
//   * View::Orange / View::Red           -> IRenderView::theme_orange/red()
//   * Ruler::GetColorByCursorOrder       -> ruler_format::cursor_hsb_color
//   * Ruler::format_real_time (static)   -> ruler_format::format_real_time
#include "pv/view/iview_delegates.h"
#include "pv/view/component/ruler_format.h"
#include "pv/data/datasource.h"

#include <QBrush>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <cassert>
#include <stdio.h>
#include "pv/base/pxvdef.h"

namespace pv {
namespace view {

// View::LabelPadding 的本地常量副本（view.cpp L109；View 静态成员位于 GUI
// 归档，值必须与 view.cpp 保持一致）。
static const QSizeF kLabelPadding(4, 4);

const QColor Cursor::LineColour(32, 74, 135);
const QColor Cursor::FillColour(52, 101, 164);
const QColor Cursor::HighlightColour(83, 130, 186);
const QColor Cursor::TextColour(Qt::white);
const int Cursor::Offset = 1;
const int Cursor::ArrowSize = 10;
const int Cursor::CloseSize = 10;

Cursor::Cursor(IRenderView &view, int order, uint64_t sampleIndex) :
    TimeMarker(view, sampleIndex)
{
   (void)order;
   _order = _order;
}

QRect Cursor::get_label_rect(const QRect &rect, bool &visible, bool has_hoff)
{
    auto *src = _view.document_snapshot_source();
    if (!src) {
        visible = false;
        return QRect(-1, -1, 0, 0);
    }
    const double samples_per_pixel =
        src->cur_snap_samplerate() * _view.scale();
    const double cur_offset = _index / samples_per_pixel;
    // Task 3.3: _view is the widget-free IRenderView; the visible-width
    // query (ex View::width()) goes through get_view_width().
    if (cur_offset < _view.offset() ||
        cur_offset >
            (_view.offset() + _view.get_view_width())) {
        visible = false;
        return QRect(-1, -1, 0, 0);
    }
    const int64_t x = _view.index2pixel(_index, has_hoff);

    const QSize label_size(
		_text_size.width() + kLabelPadding.width() * 2,
		_text_size.height() + kLabelPadding.height() * 2);
    const int top = rect.height() - label_size.height() -
		Cursor::Offset - Cursor::ArrowSize - 0.5f;
    const int height = label_size.height();

    visible = true;
    return QRect(x - label_size.width() / 2, top, label_size.width(), height);
}

QRect Cursor::get_close_rect(const QRect &rect)
{
    return QRect(rect.right() - CloseSize, rect.top(), CloseSize, CloseSize);
}

void Cursor::paint_label(QPainter &p, const QRect &rect,
            unsigned int prefix, bool has_hoff)
{
    bool visible;

    compute_text_size(p, prefix);
    const QRect r(get_label_rect(rect, visible, has_hoff));
    if (!visible)
        return;
    const QRect close(get_close_rect(r));

    p.setPen(Qt::transparent);

    if (close.contains(QPoint(_view.hover_point().x(), _view.hover_point().y())))
        p.setBrush(cursor_hsb_color(_order));
    else if (r.contains(QPoint(_view.hover_point().x(), _view.hover_point().y())))
        p.setBrush(_view.theme_orange());
    else
        p.setBrush(cursor_hsb_color(_order));

    p.drawRect(r);

    const QPoint points[] = {
        QPoint(r.left() + r.width() / 2 - ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2 + ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2, rect.bottom()),
    };
    p.drawPolygon(points, countof(points));

    if (close.contains(QPoint(_view.hover_point().x(), _view.hover_point().y())))
        p.setBrush(_view.theme_red());
    else
        p.setBrush(_view.theme_orange());
    p.drawRect(close);
    p.setPen(Qt::black);
    p.drawLine(close.left() + 2, close.top() + 2, close.right() - 2, close.bottom() - 2);
    p.drawLine(close.left() + 2, close.bottom() - 2, close.right() - 2, close.top() + 2);

    auto *src = _view.document_snapshot_source();
    if (!src)
        return;
    p.drawText(r, Qt::AlignCenter | Qt::AlignVCenter,
        format_real_time(_index,
        src->cur_snap_samplerate()));

    const QRect arrowRect = QRect(r.bottomLeft().x(), r.bottomLeft().y(), r.width(), ArrowSize);
    p.drawText(arrowRect, Qt::AlignCenter | Qt::AlignVCenter, QString::number(_order));
}

void Cursor::paint_fix_label(QPainter &p, const QRect &rect,
    unsigned int prefix, QChar label, QColor color, bool has_hoff)
{
    bool visible;

    compute_text_size(p, prefix);
    const QRect r(get_label_rect(rect, visible, has_hoff));
    if (!visible)
        return;

    p.setPen(Qt::transparent);
    p.setBrush(color);
    p.drawRect(r);

    const QPoint points[] = {
        QPoint(r.left() + r.width() / 2 - ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2 + ArrowSize, r.bottom()),
        QPoint(r.left() + r.width() / 2, rect.bottom()),
    };
    p.drawPolygon(points, countof(points));

    p.setPen(Qt::white);
    if (has_hoff) {
        auto *src = _view.document_snapshot_source();
        if (src) {
            p.drawText(r, Qt::AlignCenter | Qt::AlignVCenter,
                format_real_time(_index,
                src->cur_snap_samplerate()));
        }
    }

    const QRect arrowRect = QRect(r.bottomLeft().x(), r.bottomLeft().y(), r.width(), ArrowSize);
    p.drawText(arrowRect, Qt::AlignCenter | Qt::AlignVCenter, label);
}

void Cursor::compute_text_size(QPainter &p, unsigned int prefix)
{
    (void)prefix;
    auto *src = _view.document_snapshot_source();
    if (!src)
        return;
    _text_size = p.boundingRect(QRect(), 0,
        format_real_time(_index,
        src->cur_snap_samplerate())).size();
}
 
} // namespace view
} // namespace pv
