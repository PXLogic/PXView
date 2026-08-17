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

#ifndef PXVIEW_PV_DATA_LOGICSNAPSHOT_PATTERN_SEARCH_H
#define PXVIEW_PV_DATA_LOGICSNAPSHOT_PATTERN_SEARCH_H

#include <cstdint>
#include <map>

// This header needs the full definition of LogicSnapshot because the API
// methods reference the LogicSnapshot-owned signature
// (std::map<uint16_t, QString>) and the helper reaches host private state
// through the back-pointer. The dependency is one-way:
// logicsnapshot.h forward-declares LogicSnapshotPatternSearch and holds it via
// unique_ptr; this header includes logicsnapshot.h.
#include "pv/data/snapshot/logicsnapshot.h"

namespace pv {
namespace data {

// Extracted pattern-search subsystem from LogicSnapshot. Holds a back-pointer
// to the host (which grants friendship) to access private state; LogicSnapshot
// owns this via unique_ptr and forwards the public pattern_search method.
class LogicSnapshotPatternSearch
{
public:
    explicit LogicSnapshotPatternSearch(LogicSnapshot *host);
    ~LogicSnapshotPatternSearch();

    // ---- Pattern-search API (forwarded by LogicSnapshot) ----
    bool pattern_search(int64_t start, int64_t end, int64_t &index,
                        std::map<uint16_t, QString> &pattern, bool isNext);

private:
    // Shared search body (was private on LogicSnapshot).
    bool pattern_search_self(int64_t start, int64_t end, int64_t &index,
                             std::map<uint16_t, QString> &pattern, bool isNext);

private:
    LogicSnapshot *_host;
};

}  // namespace data
}  // namespace pv

#endif  // PXVIEW_PV_DATA_LOGICSNAPSHOT_PATTERN_SEARCH_H
