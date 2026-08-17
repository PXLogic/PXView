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

#ifndef PXVIEW_PV_DATA_LOGICSNAPSHOT_EDGE_SCAN_H
#define PXVIEW_PV_DATA_LOGICSNAPSHOT_EDGE_SCAN_H

#include <cstdint>
#include <utility>
#include <vector>

// This header needs the full definition of LogicSnapshot because the API
// methods reference the LogicSnapshot-owned sample/edge data and the helper
// reaches host private state through the back-pointer. The dependency is
// one-way: logicsnapshot.h forward-declares LogicSnapshotEdgeScan and holds it
// via unique_ptr; this header includes logicsnapshot.h.
#include "pv/data/snapshot/logicsnapshot.h"

namespace pv {
namespace data {

// Extracted display-edge scan subsystem from LogicSnapshot. Holds a back-pointer
// to the host (which grants friendship) to access private state; LogicSnapshot
// owns this via unique_ptr and forwards the three public edge-scan methods
// (get_display_edges / get_nxt_edge / get_pre_edge).
class LogicSnapshotEdgeScan
{
public:
    explicit LogicSnapshotEdgeScan(LogicSnapshot *host);
    ~LogicSnapshotEdgeScan();

    // ---- Edge-scan API (forwarded by LogicSnapshot) ----
    bool get_display_edges(std::vector<std::pair<bool, bool>> &edges,
                           std::vector<std::pair<uint16_t, bool>> &togs,
                           uint64_t start, uint64_t end, uint16_t width,
                           uint16_t max_togs, double pixels_offset,
                           double min_length, uint16_t sig_index);

    bool get_nxt_edge(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

private:
    // C3 (P9-on-raw): shared display-edge scan body, operating on PHYSICAL
    // sample coordinates. Caller guarantees start/end are physical (loop:
    // pre-adjusted by +_loop_offset; finite: user coords == physical) and the
    // required synchronization is in effect (finite: lock-free via
    // committed_sample_count(); loop: caller holds _mutex and has temporarily
    // added _loop_offset to _ring_sample_count). `sample_count` is the upper
    // bound the scan may touch.
    bool get_display_edges_common(
        std::vector<std::pair<bool, bool>> &edges,
        std::vector<std::pair<uint16_t, bool>> &togs,
        uint64_t start, uint64_t end, uint16_t width, uint16_t max_togs,
        double pixels_offset, double min_length, uint16_t sig_index,
        uint64_t sample_count);

    bool get_nxt_edge_unlock(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);
    bool get_nxt_edge_self(uint64_t &index, bool last_sample, uint64_t end,
                      double min_length, int sig_index);

    bool get_pre_edge_self(uint64_t &index, bool last_sample,
                      double min_length, int sig_index);

    bool lbp_nxt_edge(uint64_t &index, uint64_t root_index, uint64_t lbp_tog, uint8_t lbp_tog_pos,
                      bool aft_tog, uint8_t aft_pos, bool last_sample, int sig_index);

    bool block_nxt_edge(uint64_t *lbp, uint64_t &index, uint64_t block_end, bool last_sample,
                        unsigned int min_level);

    bool lbp_pre_edge(uint64_t &index, uint64_t root_index, uint64_t lbp_tog, uint8_t &lbp_tog_pos,
                      bool pre_tog, uint8_t pre_pos, bool last_sample, int sig_index);

    bool block_pre_edge(uint64_t *lbp, uint64_t &index, bool last_sample,
                        unsigned int min_level, int sig_index);

private:
    LogicSnapshot *_host;
};

}  // namespace data
}  // namespace pv

#endif  // PXVIEW_PV_DATA_LOGICSNAPSHOT_EDGE_SCAN_H
