#ifndef PXVIEW_CORE_CURSORREGISTRY_H
#define PXVIEW_CORE_CURSORREGISTRY_H

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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pv {
namespace core {

/**
 * CursorEntry — pure-data description of one measurement cursor.
 *
 * Introduced by Task C2 of plan-core-view-split-and-mcp-coverage. The
 * cursor *position state* (which cursors exist and where each one sits
 * in the sample stream) previously lived only in the View layer
 * (view::Cursor / View::_logic_cursors / View::_dso_cursors). MCP
 * clients running headless could not enumerate or mutate cursors
 * because no View was instantiated.
 *
 * CursorEntry is the Core-layer mirror of that state. It holds only the
 * fields required for MCP/API consumption — the sample position and a
 * visibility flag. Visual properties (colour, label text, QPainter
 * geometry) remain on view::Cursor, which is now a pure rendering
 * object that reads its position from CursorEntry via the DataSource
 * interface.
 *
 * The `index` field is the positional index (0-based) of the entry in
 * the CursorRegistry vector. It is recomputed by CursorRegistry::
 * get_cursors() on each call so it stays consistent after removals
 * (which shift subsequent entries down). This matches the positional
 * semantics of the existing MCP API (SessionService::remove_cursor(int
 * index)) and the View layer (ViewCursors::get_cursor_by_index).
 */
struct CursorEntry
{
    /// Positional index (0-based) within the registry vector. Filled in
    /// by CursorRegistry::get_cursors(); ignored on input to
    /// add_cursor / set_cursor_position.
    int      index = 0;

    /// Sample-stream position (sample index) of this cursor.
    uint64_t sample_position = 0;

    /// Whether the cursor is visible. The View layer honours this when
    /// painting; MCP clients treat it as metadata.
    bool     visible = true;
};

/**
 * CursorRegistry — Core-layer store of measurement cursor positions.
 *
 * Owned by SessionStateContext (one instance per session). The View
 * layer and the MCP API both read/write through the DataSource
 * interface, which forwards to SigSession::cursor_registry() and hence
 * to this class.
 *
 * Thread-safety: all public methods are non-const and rely on the
 * caller holding the SessionStateContext data mutex when cross-thread
 * access is needed. In practice cursors are mutated from the GUI
 * thread (user drag) or the MCP RPC thread; the MCP path takes
 * SessionStateContext::data_mutex() inside SigSession::add_cursor /
 * remove_cursor / set_cursor_position overrides.
 *
 * The registry uses positional indexing: add_cursor appends and
 * returns size-1; remove_cursor erases at the given position (shifting
 * subsequent entries down). This mirrors the positional semantics of
 * ViewCursors::get_cursor_by_index and the MCP remove_cursor tool.
 */
class CursorRegistry
{
public:
    CursorRegistry() = default;
    ~CursorRegistry() = default;

    CursorRegistry(const CursorRegistry &) = delete;
    CursorRegistry &operator=(const CursorRegistry &) = delete;

    /// Append a new cursor at @param sample_position. Returns the
    /// positional index of the newly added entry (size-1 after append).
    int add_cursor(uint64_t sample_position);

    /// Remove the cursor at positional @param index. Returns true if
    /// the index was valid and the entry was removed; false otherwise.
    /// Subsequent entries shift down by one (their positional indices
    /// decrease by 1).
    bool remove_cursor(int index);

    /// Return a snapshot of all cursors, with each entry's `index`
    /// field set to its current positional index. The returned vector
    /// is a copy; callers may iterate without holding any lock.
    std::vector<CursorEntry> get_cursors() const;

    /// Update the sample position of the cursor at positional @param
    /// index. Returns true if the index was valid; false otherwise.
    bool set_cursor_position(int index, uint64_t sample_position);

    /// Remove all cursors.
    void clear();

    /// Number of cursors currently registered.
    std::size_t size() const;

private:
    std::vector<CursorEntry> _cursors;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_CURSORREGISTRY_H
