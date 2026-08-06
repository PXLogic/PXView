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

#ifndef PXVIEW_PV_DATA_LOGICSNAPSHOT_GLITCH_FILTER_H
#define PXVIEW_PV_DATA_LOGICSNAPSHOT_GLITCH_FILTER_H

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

// This header needs the full definition of LogicSnapshot because the public
// API references the nested type LogicSnapshot::FillRange (kept on
// LogicSnapshot so external callers continue to use
// `LogicSnapshot::FillRange`). The dependency is one-way:
// logicsnapshot.h forward-declares LogicSnapshotGlitchFilter and holds it via
// unique_ptr; this header includes logicsnapshot.h to obtain FillRange.
#include "logicsnapshot.h"

namespace pv {
namespace data {

// Extracted glitch-filter subsystem (cluster C) from LogicSnapshot.
// Owns the per-channel filtered-range list, the glitch-filter state flag, and
// the apply/invert/recalc logic. LogicSnapshot holds this via unique_ptr and
// forwards the public methods; chunk-tree state (cluster A) stays in
// LogicSnapshot and is accessed through the back-pointer (friend).
class LogicSnapshotGlitchFilter
{
public:
    explicit LogicSnapshotGlitchFilter(LogicSnapshot *host);
    ~LogicSnapshotGlitchFilter();

    // ---- Glitch filter API (forwarded by LogicSnapshot) ----
    void apply_glitch_filter(int sig_index, uint32_t threshold,
                             std::function<void(int)> progress_callback,
                             GlitchFilterMode filter_mode = GlitchFilterMode::Both);
    // 架构修复：thresholds/modes 用 channel_index 作 key，消除 View/Core 位置序号错位
    void apply_glitch_filter_all(const std::map<int, uint32_t> &thresholds,
                                 std::function<void(int)> progress_callback,
                                 const std::map<int, GlitchFilterMode> &filter_modes = {});
    bool is_glitch_filtered() const;
    void set_glitch_filtered(bool filtered);

    // Persisted filtered ranges for View-layer overlay rendering.
    const std::vector<LogicSnapshot::FillRange>& get_filtered_ranges(int sig_index) const;
    void clear_filtered_ranges();

    // Signal invert (rebuilds mipmap per block).
    void invert_channel(int sig_index);

private:
    // Recompute mipmap for a single leaf block. Extracted from
    // LogicSnapshot (was private there) — used by invert_channel and
    // apply_glitch_filter's batch flush.
    void recalc_mipmap(unsigned int order, uint64_t index0, uint64_t index1);

private:
    LogicSnapshot *_host;

    bool        _glitch_filtered;
    std::map<int, std::vector<LogicSnapshot::FillRange>> _filtered_ranges_per_channel;
    static const std::vector<LogicSnapshot::FillRange> _empty_filtered_ranges;
};

}  // namespace data
}  // namespace pv

#endif  // PXVIEW_PV_DATA_LOGICSNAPSHOT_GLITCH_FILTER_H
