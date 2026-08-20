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

// modernize-thread-model Task 2: pure-function waveform rasterization.
//
// The logic-channel waveform painting logic was extracted from
// LogicSignal::paint_mid_align so it can be:
//   * unit-tested without constructing a View/QWidget (just a snapshot +
//     value parameters), and
//   * executed on a background render thread (all local buffers, no
//     Signal/View member mutation).
//
// The rendering must remain PIXEL-IDENTICAL to the original member method.
// Do not "improve" any arithmetic here — copy it verbatim.

#ifndef PXVIEW_PV_VIEW_RENDERER_RASTERIZE_H
#define PXVIEW_PV_VIEW_RENDERER_RASTERIZE_H

#include <cstdint>
#include <vector>

#include <QColor>
#include <QPainter>

#include "pv/view/trace/paint_context.h"

namespace pv {

namespace data {
class LogicSnapshot;
class DsoSnapshot;
class AnalogSnapshot;
} // namespace data

namespace view {

// A glitch-overlay range in sample indices [start, end). Compatible with
// LogicSnapshot::FillRange and PulseAnalyzer::Pulse (both carry
// start/end/level; level is unused by the rasterizer).
struct GlitchRange {
    uint64_t start;
    uint64_t end;
};

// Rasterize one logic channel's waveform (extracted from
// LogicSignal::paint_mid_align). Pure function:
//   * reads only the snapshot + value parameters passed in,
//   * uses only local buffers (never writes Signal members),
//   * has no View/QWidget dependency,
//   * is thread-safe and directly unit-testable.
//
// The snapshot pointer is non-const to match the original paint_mid_align
// (which held a non-const _data) and because LogicSnapshot's read accessors
// (get_display_edges / is_glitch_filtered) are non-const forwarders to
// mutable helpers; they are internally synchronized and never mutate the
// snapshot. Purity here means "no Signal/View member mutation, only reads".
//
// Parameters mirror the original member state:
//   snapshot       — LogicSnapshot holding the channel data (thread-safe read)
//   channel_index  — SignalModel::index() / LogicSnapshot sig_index
//   left/right     — horizontal paint extent (original call used left == 0)
//   y              — channel vertical center == (int)(get_y() + total_height*0.5)
//   total_height   — channel total height
//   colour         — FINAL waveform pen colour; the caller computes
//                    (_colour.isValid() ? _colour : fore) exactly as the
//                    original paint_mid_align did
//   scale/offset   — ctx scale / pixel offset (same as PaintContext)
//   end_align_sample — alignment sample (ring_sample_count-1 etc.)
//   ctx            — PaintContext (show_glitch_overlay drives the
//                    "already-filtered" overlay)
//   preview_ranges — optional glitch live-preview ranges (previously from
//                    View::get_preview_ranges; GUI state — pass nullptr to
//                    skip). GlitchRange values are converted by the caller.
void rasterize_logic_channel(
    QPainter &p,
    data::LogicSnapshot *snapshot,
    int channel_index,
    int left,
    int right,
    int y,
    int total_height,
    const QColor &colour,
    double scale,
    int64_t offset,
    uint64_t end_align_sample,
    const PaintContext &ctx,
    const std::vector<GlitchRange> *preview_ranges = nullptr);

// Rasterize one DSO channel's waveform (extracted from
// DsoSignal::paint_per_pixel). Pure function with the same guarantees as
// rasterize_logic_channel. All GUI state that the original read from the
// Signal/View is passed as value parameters:
//   zeroY          — get_zero_vpos() (computed on GUI thread)
//   hw_offset      — get_hw_offset() (cached, GUI-thread prepare)
//   top/bottom     — get_view_rect().top()/.bottom()
//   scale          — _scale
//   colour         — _colour
//   channel_index  — get_index()
void rasterize_dso_channel(
    QPainter &p,
    data::DsoSnapshot *snapshot,
    int zeroY,
    int left,
    int right,
    int64_t start,
    int64_t end,
    int hw_offset,
    double samples_per_pixel,
    int channel_index,
    float top,
    float bottom,
    float scale,
    const QColor &colour);

// Rasterize one analog channel's waveform (extracted from
// AnalogSignal::paint_per_pixel). Pure function with the same guarantees.
// GUI/state values passed as parameters:
//   zeroY          — ratio2pos(get_zero_ratio())
//   hw_offset      — get_hw_offset() (GUI-thread prepare)
//   scale          — _scale (ADC integer path)
//   float_scale    — _float_scale (float-voltage path, recomputed on GUI
//                    thread inside paint_mid)
//   top/bottom     — get_y() ± total_height*0.5
//   colour         — _colour
void rasterize_analog_channel(
    QPainter &p,
    data::AnalogSnapshot *snapshot,
    int zeroY,
    int left,
    int right,
    uint64_t start_index,
    int64_t sample_count,
    double samples_per_pixel,
    int order,
    float top,
    float bottom,
    int hw_offset,
    float scale,
    float float_scale,
    const QColor &colour);

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_RENDERER_RASTERIZE_H
