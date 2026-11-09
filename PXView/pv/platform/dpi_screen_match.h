/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace pv {
namespace platform {

// Index of the QScreen whose native HMONITOR equals `target`, or -1.
//
// HMONITOR is the identity of a physical display; it does not depend on the
// coordinate system. The old implementation compared MONITORINFO.rcMonitor
// (physical pixels) with QScreen::geometry().topLeft() (Qt logical pixels),
// which cannot match on a mixed-DPI desktop: with a 4K@150% primary and a
// 1080p@100% secondary to its right, Win32 reports left=3840 for the
// secondary while Qt reports left=2560.
//
// Handles are passed as `const void*` so this file stays platform-neutral and
// unit-testable; the caller casts HMONITOR (a Win32 pointer type) at the edge.
inline int select_screen_by_monitor(const std::vector<const void *> &monitor_handles,
                                    const void *target)
{
    if (target == nullptr)
        return -1;

    for (std::size_t i = 0; i < monitor_handles.size(); ++i) {
        if (monitor_handles[i] == target)
            return static_cast<int>(i);
    }
    return -1;
}

// Index of the QScreen whose devicePixelRatio is closest to `target_dpr`,
// used only when the native HMONITOR match above is unavailable.
//
// Deliberately does NOT default to index 0 (the primary screen). On the
// mixed-DPI desktop above the primary is the 4K/1.5x one; silently picking it
// is exactly what kept the high-DPI scale factor alive after the window was
// dragged onto the 1080p/1.0x monitor. Returns -1 when nothing is within
// `tolerance` so the caller can keep the current screen instead of guessing.
inline int select_screen_by_dpr(const std::vector<double> &screen_dprs,
                                double target_dpr,
                                double tolerance = 0.01)
{
    if (target_dpr <= 0.0 || !(tolerance >= 0.0))
        return -1;

    int best = -1;
    double best_delta = std::numeric_limits<double>::infinity();

    for (std::size_t i = 0; i < screen_dprs.size(); ++i) {
        if (!(screen_dprs[i] > 0.0))
            continue;
        const double delta = std::fabs(screen_dprs[i] - target_dpr);
        if (delta <= tolerance && delta < best_delta) {
            best_delta = delta;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Qt logical point local to the embedded Qt child window, derived from a
// Win32 WM_NCHITTEST physical SCREEN point.
//
// Qt6 on Windows keeps a screen's virtual-desktop ORIGIN in physical pixels
// but its SIZE in logical pixels, so `physical / dpr` is NOT a Qt global
// coordinate once a secondary monitor has a non-zero/negative physical origin
// or a different DPR. Dividing the physical SCREEN coordinate was the bug
// that made the title row unclickable: mapFromGlobal() produced a local Y
// hundreds of pixels outside the 32px title bar, childAt() never found the
// QTabBar, and hitTest() fell through to HTCAPTION (Windows then treats the
// press as a window drag and Qt sees no mouse event).
//
// Subtract the window's own physical origin FIRST, then divide. The Qt child
// HWND fills the client area and its devicePixelRatio equals `dpr`, so the
// result is a valid local logical coordinate on any monitor.
struct LogicalPoint {
    int x;
    int y;
};

inline LogicalPoint hit_test_local_point(long physical_x, long physical_y,
                                         int window_left, int window_top,
                                         double dpr)
{
    const double scale = dpr > 0.0 ? dpr : 1.0;
    return {static_cast<int>(static_cast<double>(physical_x - window_left) / scale),
            static_cast<int>(static_cast<double>(physical_y - window_top) / scale)};
}

} // namespace platform
} // namespace pv
