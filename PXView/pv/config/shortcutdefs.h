/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#pragma once

#include <QString>

namespace pv {

enum ShortcutActionId {
    SHORTCUT_RUN_STOP = 1,
    SHORTCUT_INSTANT = 2,
    SHORTCUT_TRIGGER = 3,
    SHORTCUT_DECODE = 4,
    SHORTCUT_MEASURE = 5,
    SHORTCUT_SEARCH = 6,
    SHORTCUT_OPTIONS = 7,
    SHORTCUT_DEVICE_SELECT = 8,
    SHORTCUT_PAGE_UP = 9,
    SHORTCUT_PAGE_DOWN = 10,
    SHORTCUT_ZOOM_IN = 11,
    SHORTCUT_ZOOM_OUT = 12,
    SHORTCUT_DSO_CH0 = 13,
    SHORTCUT_DSO_CH1 = 14,
    SHORTCUT_DSO_VUP = 15,
    SHORTCUT_DSO_VDOWN = 16,
    SHORTCUT_FILE_OPEN = 17,
    SHORTCUT_FILE_SAVE = 18,
    SHORTCUT_FILE_EXPORT = 19,
    SHORTCUT_FILE_LOAD = 20,
    SHORTCUT_FILE_STORE = 21,
    SHORTCUT_SCREENSHOT = 22,
    SHORTCUT_FFT = 23,
    SHORTCUT_MATH = 24,
    SHORTCUT_LISSAJOUS = 25,
    SHORTCUT_SETTINGS = 26,
    SHORTCUT_LOG = 27,
    SHORTCUT_FUNCTION = 28,
    SHORTCUT_THEME_TOGGLE = 29,
    SHORTCUT_NEW_TAB = 30,
    SHORTCUT_CLOSE_TAB = 31,
    SHORTCUT_ZOOM_FIT = 32,
    SHORTCUT_COUNT = 33
};

struct ShortcutActionInfo {
    int actionId;
    const char* keySequence;
    const char* displayName;
};

const ShortcutActionInfo* GetShortcutActionInfos(int* count);

} // namespace pv
