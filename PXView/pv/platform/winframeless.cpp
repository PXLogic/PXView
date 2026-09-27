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

#include "pv/platform/winframeless.h"

#ifdef _WIN32

#include <QColor>
#include <QLibrary>

#include <windows.h>
#include <dwmapi.h>

#endif

namespace pv {

namespace {

#ifdef _WIN32

// This is the "aero borderless" combination used by the reference
// implementation (Reference/atk-logic-master/pv/draw/frameless_window.cpp).
//
// WS_CAPTION gives the window a non-client area, which WM_NCCALCSIZE then
// removes so only the shadow is left.
//
// WS_THICKFRAME, WS_MAXIMIZEBOX and WS_MINIMIZEBOX must all be present: DWM
// only paints its full, soft window shadow for windows that look like a
// normal resizable top-level window. Drop the maximize/minimize boxes and it
// falls back to the old layered dialog shadow, which shows visible banding.
// The frame itself is not visible as long as the 1px border line is painted
// in the window background color (see setBorderColor), and resizing is
// disabled separately in WM_NCHITTEST.
const DWORD kFrameStyle = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU
                        | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;

using DwmIsCompositionEnabledPtr      = HRESULT (WINAPI *)(BOOL *);
using DwmExtendFrameIntoClientAreaPtr = HRESULT (WINAPI *)(HWND, const MARGINS *);
using DwmSetWindowAttributePtr        = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

// dwmapi is resolved at runtime instead of being linked, the same way
// WinNativeWidget does it.
template <typename T>
T resolveDwm(const char *symbol)
{
    return reinterpret_cast<T>(QLibrary::resolve("dwmapi", symbol));
}

bool isCompositionEnabled()
{
    static const auto pDwmIsCompositionEnabled =
        resolveDwm<DwmIsCompositionEnabledPtr>("DwmIsCompositionEnabled");

    if (pDwmIsCompositionEnabled == nullptr)
        return false;

    BOOL enabled = FALSE;
    return pDwmIsCompositionEnabled(&enabled) == S_OK && enabled != FALSE;
}

// A non-zero margin extends the client area into the frame, which is what
// makes DWM render its shadow around the window. The margin itself stays
// invisible because WM_NCCALCSIZE removes the non-client area.
void extendFrame(HWND hwnd, bool enabled)
{
    static const auto pDwmExtendFrameIntoClientArea =
        resolveDwm<DwmExtendFrameIntoClientAreaPtr>("DwmExtendFrameIntoClientArea");

    if (pDwmExtendFrameIntoClientArea == nullptr || !isCompositionEnabled())
        return;

    const MARGINS margins = enabled ? MARGINS{1, 1, 1, 1} : MARGINS{0, 0, 0, 0};
    pDwmExtendFrameIntoClientArea(hwnd, &margins);
}

HWND hwndOf(QWidget *w)
{
    return w != nullptr ? reinterpret_cast<HWND>(w->winId()) : nullptr;
}

#endif

} // namespace

void WinFrameless::apply(QWidget *w)
{
#ifdef _WIN32
    HWND hwnd = hwndOf(w);
    if (hwnd == nullptr)
        return;

    const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));

    // Only touch windows that really are frameless, so nothing changes for
    // dialogs that still use the system frame.
    if ((style & WS_CAPTION) == 0)
    {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE,
            static_cast<LONG_PTR>(style | kFrameStyle));

        // Qt::Dialog can leave WS_EX_DLGMODALFRAME behind, which makes DWM
        // pick the layered dialog shadow again. Clear it so the window is
        // treated like an ordinary top-level one.
        const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_DLGMODALFRAME) != 0)
            ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                static_cast<LONG_PTR>(exStyle & ~WS_EX_DLGMODALFRAME));

        extendFrame(hwnd, true);

        ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#else
    (void)w;
#endif
}

bool WinFrameless::handleMessage(void *message, qintptr *result, QWidget *w)
{
#ifdef _WIN32
    if (message == nullptr || result == nullptr)
        return false;

    MSG *msg = static_cast<MSG *>(message);

    switch (msg->message)
    {
    case WM_NCCALCSIZE:
    {
        if (msg->wParam != TRUE)
            break;

        // Same as the main window (WinNativeWidget::WndProc): keep a 1px
        // non-client frame instead of removing it entirely. DWM paints its
        // full soft window shadow only while the window still has a frame;
        // with a zero-sized frame it falls back to the layered dialog shadow,
        // which shows visible banding. The 1px frame line itself is painted
        // in the window background color (setBorderColor), so it stays
        // invisible.
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        RECT *rect = &params->rgrc[0];

        const int k = w != nullptr ? w->devicePixelRatio() : 1;
        rect->left   += 1 * k;
        rect->top    += 1;
        rect->right  -= 1 * k;
        rect->bottom -= 1 * k;

        *result = WVR_VALIDRECTS;
        return true;
    }

    case WM_ACTIVATE:
        // DWM drops the extended frame on some state changes; re-apply it.
        extendFrame(hwndOf(w), true);
        return true;

    case WM_NCACTIVATE:
        // Avoid the flicker of an inactive frame when there is no composition.
        if (!isCompositionEnabled())
        {
            *result = 1;
            return true;
        }
        break;

    case WM_NCHITTEST:
        // Dialogs keep their size: never report a sizing border. Dragging is
        // done by the Qt-drawn title bar (toolbars::TitleBar), so there is no
        // need to return HTCAPTION here.
        *result = HTCLIENT;
        return true;

    default:
        break;
    }
#else
    (void)message;
    (void)result;
    (void)w;
#endif

    return false;
}

void WinFrameless::setBorderColor(QWidget *w, const QColor &color)
{
#ifdef _WIN32
    HWND hwnd = hwndOf(w);
    if (hwnd == nullptr)
        return;

    static const auto pDwmSetWindowAttribute =
        resolveDwm<DwmSetWindowAttributePtr>("DwmSetWindowAttribute");

    if (pDwmSetWindowAttribute == nullptr)
        return;

    // DWMWA_BORDER_COLOR, available since Windows 11.
    const DWORD DWMWA_BORDER_COLOR = 34;
    const COLORREF cr = RGB(color.red(), color.green(), color.blue());

    pDwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &cr, sizeof(cr));
#else
    (void)w;
    (void)color;
#endif
}

} // namespace pv
