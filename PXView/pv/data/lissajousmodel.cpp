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

#include "lissajousmodel.h"

namespace pv {
namespace data {

LissajousModel::LissajousModel()
    : _enabled(false)
    , _x_index(0)
    , _y_index(0)
    , _percent(100)
{
}

LissajousModel::~LissajousModel()
{
}

void LissajousModel::set_enabled(bool enabled) { _enabled = enabled; }
void LissajousModel::set_x_index(int index) { _x_index = index; }
void LissajousModel::set_y_index(int index) { _y_index = index; }
void LissajousModel::set_percent(int percent) { _percent = percent; }

} // namespace data
} // namespace pv
