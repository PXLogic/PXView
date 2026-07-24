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

#include "cursorregistry.h"

namespace pv {
namespace core {

int CursorRegistry::add_cursor(uint64_t sample_position)
{
    CursorEntry e;
    e.sample_position = sample_position;
    e.visible = true;
    _cursors.push_back(e);
    return static_cast<int>(_cursors.size() - 1);
}

bool CursorRegistry::remove_cursor(int index)
{
    if (index < 0 || index >= static_cast<int>(_cursors.size()))
        return false;
    _cursors.erase(_cursors.begin() + index);
    return true;
}

std::vector<CursorEntry> CursorRegistry::get_cursors() const
{
    std::vector<CursorEntry> out;
    out.reserve(_cursors.size());
    for (std::size_t i = 0; i < _cursors.size(); ++i) {
        CursorEntry e = _cursors[i];
        e.index = static_cast<int>(i);
        out.push_back(e);
    }
    return out;
}

bool CursorRegistry::set_cursor_position(int index, uint64_t sample_position)
{
    if (index < 0 || index >= static_cast<int>(_cursors.size()))
        return false;
    _cursors[index].sample_position = sample_position;
    return true;
}

void CursorRegistry::clear()
{
    _cursors.clear();
}

std::size_t CursorRegistry::size() const
{
    return _cursors.size();
}

} // namespace core
} // namespace pv
