/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_PLATFORM_WINFRAMELESS_H
#define PXVIEW_PV_PLATFORM_WINFRAMELESS_H

#include <QWidget>

class QColor;

namespace pv {

// Native window frame for frameless Qt dialogs.
//
// A Qt frameless window (Qt::FramelessWindowHint) gets no shadow at all on
// Windows. To get the same decoration the main window has, the HWND is given
// back a caption/sizing frame so DWM renders its native shadow and rounded
// corners, and WM_NCCALCSIZE then shrinks the non-client area to nothing so
// only the Qt-drawn title bar remains. This is the same technique
// WinNativeWidget uses for the main window.
//
// Everywhere else this is a no-op: macOS keeps the NSWindow shadow for free,
// and on Linux the caller draws its own shadow (see PxDialog/DSMessageBox).
//
// References: Reference/atk-logic-master/pv/draw/frameless_window.cpp
class WinFrameless
{
public:
    // Applies the native frame. Must be called once the window is visible,
    // since winId() forces the HWND into existence.
    static void apply(QWidget *w);

    // Call from QWidget::nativeEvent(). Returns true when handled.
    static bool handleMessage(void *message, qintptr *result, QWidget *w);

    // DWMWA_BORDER_COLOR (Windows 11+), the attribute the main window uses for
    // its #808080 frame (WinNativeWidget::SetBorderColor()). Dialogs pass their
    // own background color here so the frame, if the system draws one at all,
    // blends into the window and stays invisible.
    static void setBorderColor(QWidget *w, const QColor &color);
};

} // namespace pv

#endif // PXVIEW_PV_PLATFORM_WINFRAMELESS_H
