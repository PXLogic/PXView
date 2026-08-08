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

#ifndef PXVIEW_PV_MAINWINDOW_SHORTCUT_MANAGER_H
#define PXVIEW_PV_MAINWINDOW_SHORTCUT_MANAGER_H

class QEvent;
class QObject;

namespace pv {

class MainWindow;

/**
 * @brief Keyboard shortcut and event filter delegate for MainWindow.
 *
 * Phase 2 refactoring: extracts the eventFilter() key press handling
 * and resolveShortcutAction() logic from MainWindow (~340 lines).
 *
 * The delegate is a friend of MainWindow so it can access private
 * members (_sampling_bar, _trig_bar, _file_bar, _dock_manager,
 * _tab_manager, current_view(), switchTheme(), etc.) needed to
 * dispatch shortcut actions.
 *
 * MainWindow::eventFilter() forwards KeyPress events to this delegate's
 * handleKeyPress(); all other event types are returned as unhandled.
 */
class MainWindowShortcutManager {
public:
    explicit MainWindowShortcutManager(MainWindow *wnd);

    /**
     * Resolve a key + modifiers combination to a shortcut action ID.
     * Checks user-configured shortcuts first, falls back to defaults.
     * Returns 0 if no match found.
     */
    int resolveShortcutAction(int key, int modifiers);

    /**
     * Handle a KeyPress event. Returns true if the event was consumed
     * (shortcut dispatched or event forwarded to a focused input widget),
     * false if the event should propagate further.
     *
     * Logic:
     * 1. If a SearchPatternInput is focused, forward the event to it.
     * 2. If a QLineEdit / QAbstractSpinBox / QComboBox / QAbstractButton
     *    or any dock widget child is focused, forward the event (with
     *    Windows VK code mapping for native widget compatibility).
     * 3. Handle Ctrl+Z undo for glitch filter.
     * 4. Resolve the shortcut action and dispatch it.
     */
    bool handleKeyPress(QObject *object, QEvent *event);

private:
    MainWindow *_wnd;
};

} // namespace pv

#endif // PXVIEW_PV_MAINWINDOW_SHORTCUT_MANAGER_H
