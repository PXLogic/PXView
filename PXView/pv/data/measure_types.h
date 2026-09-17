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

#ifndef PV_DATA_MEASURE_TYPES_H
#define PV_DATA_MEASURE_TYPES_H

#include <cstdint>
#include <string>

namespace pv {
namespace data {

/**
 * MeasurementValue - one DSO measurement result (type enum + value + unit).
 *
 * Lives in the DATA layer because it is the shape the DataSource contract hands
 * out: DataSource::get_measurements() / IMeasureSource::get_measurements()
 * return a flat list of these, and Core's MeasureCalculator produces them. The
 * API layer only SERIALISES them (RpcDispatcher::to_json) and the View only
 * reads them.
 *
 * It used to be declared in pv/api/types.h, which made the DATA layer include a
 * header from the API layer purely to name a return type
 * (datasource.h / imeasure_source.h), i.e. the same inverted-edge pattern that
 * CursorEntry had with pv/core/cursorregistry.h. A value type shared by data,
 * core, session, api and view belongs in the lowest layer all of them already
 * depend on (pxview-data is linked PUBLIC by pxview-core; api and view depend
 * on it too), so nothing gains a new dependency from the move.
 *
 * `type` stays an int32_t (the DSO_MS_* measurement type), matching the wire
 * contract in pv/api/types.h; `unit` is the SI base-unit string, and `value` is
 * already scaled to that base unit.
 */
struct MeasurementValue {
    int32_t     type  = 0;
    double      value = 0.0;
    std::string unit  = "";
    bool        valid = false;
};

} // namespace data
} // namespace pv

#endif // PV_DATA_MEASURE_TYPES_H
