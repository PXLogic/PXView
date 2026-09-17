/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PV_DATA_CURSOR_TYPES_H
#define PV_DATA_CURSOR_TYPES_H

#include <cstdint>

namespace pv {
namespace data {

/**
 * CursorEntry - pure-data description of one measurement cursor.
 *
 * Lives in the DATA layer because it is what the DataSource contract hands out
 * (DataSource::get_cursors / IMeasureSource::get_cursors): the sample position
 * plus a visibility flag, and nothing else. Visual properties (colour, label
 * text, QPainter geometry) stay on view::Cursor, which is a pure rendering
 * object reading its position from here.
 *
 * It used to be declared in pv/core/cursorregistry.h, which made the DATA layer
 * include a CORE header (datasource.h / imeasure_source.h) purely to name a
 * return type: pxview-data and pxview-core then depended on each other, i.e. a
 * static-archive cycle. A value type shared by data, core, session, view and
 * api belongs in the lowest layer that all of them already depend on.
 *
 * The `index` field is the positional index (0-based) of the entry in the
 * registry vector. CursorRegistry::get_cursors() recomputes it on every call so
 * it stays consistent after removals (which shift subsequent entries down).
 * This matches the positional semantics of the MCP API
 * (SessionService::remove_cursor(int index)) and of the View layer
 * (ViewCursors::get_cursor_by_index).
 */
struct CursorEntry
{
    /// Positional index (0-based) within the registry vector. Filled in by
    /// CursorRegistry::get_cursors(); ignored on input to
    /// add_cursor / set_cursor_position.
    int      index = 0;

    /// Sample-stream position (sample index) of this cursor.
    uint64_t sample_position = 0;

    /// Whether the cursor is visible. The View layer honours this when
    /// painting; MCP clients treat it as metadata.
    bool     visible = true;
};

} // namespace data
} // namespace pv

#endif // PV_DATA_CURSOR_TYPES_H
