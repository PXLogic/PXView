/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_VIEW_TRACE_PAINT_CONTEXT_H
#define PXVIEW_PV_VIEW_TRACE_PAINT_CONTEXT_H

#include <cstdint>
#include <QPointF>

namespace pv {
namespace view {

// PaintContext — a snapshot of the rendering environment passed to
// Trace::paint_back / paint_mid / paint_fore. By passing rendering
// parameters as a struct instead of having each Trace read them from
// View*, we achieve:
//
// 1. Thread safety: the values are snapshotted before paint begins,
//    so concurrent View mutations don't cause torn reads.
// 2. Testability: paint methods can be unit-tested by constructing a
//    PaintContext without a View.
// 3. Decoupling: paint methods declare their dependencies explicitly
//    via the struct fields rather than reaching into the View God-class.
//
// Fields that remain on _view (data access / side effects):
// - set_back(): side effect during DsoSignal::paint_back
// - get_preview_ranges(): data access in LogicSignal::paint_mid_align
// - get_own_signals(): data access in SpectrumTrace::paint_mid
// - auto_set(): side effect in DsoSignal::paint_fore (throttled)
struct PaintContext {
    // --- Rendering coordinate system (from View) ---
    double scale = 0.0;           // pixels per sample
    int64_t offset = 0;           // horizontal pixel offset
    double trig_hoff = 0.0;       // trigger horizontal offset
    int signal_height = 0;        // height of one signal row
    int view_width = 0;           // width of the viewport

    // --- Rendering mode flags (from View / Session) ---
    bool is_logic_mode = false;           // logic rendering mode
    bool is_stopped_status = false;       // session is stopped
    bool is_loop_mode = false;            // session is in loop mode
    bool dso_trig_moved = false;          // DSO trigger was moved
    bool show_glitch_overlay = false;     // glitch filter overlay visible

    // --- Mouse position (from View) ---
    QPointF hover_point;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_TRACE_PAINT_CONTEXT_H
