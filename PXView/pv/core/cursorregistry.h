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

#include "pv/data/cursor_types.h"

namespace pv {
namespace core {

/**
 * CursorEntry (the pure-data value handed out through the DataSource
 * interface, and the Core-layer mirror of measurement cursor state) now
 * lives in pv/data/cursor_types.h - see that file for the rationale.
 * It moved out of this header so the DATA layer no longer includes a CORE
 * header just to name a return type.

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
    std::vector<data::CursorEntry> get_cursors() const;

    /// Update the sample position of the cursor at positional @param
    /// index. Returns true if the index was valid; false otherwise.
    bool set_cursor_position(int index, uint64_t sample_position);

    /// Remove all cursors.
    void clear();

    /// Number of cursors currently registered.
    std::size_t size() const;

private:
    std::vector<data::CursorEntry> _cursors;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_CURSORREGISTRY_H
