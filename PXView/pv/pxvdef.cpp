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

#include "pxvdef.h"
#include <cstring>

#ifdef DS_DEBUG_TRACE
#include <cstdio>
void ds_print(const char *s) {
    std::printf("%s", s);
}
#endif

// NOTE: kDevModeLogic / kDevModeAnalog / kDevModeDso / kDevModeMso are
// defined in deviceagent.cpp (not here) to avoid multiple definition errors.

// --- DecoderDataFormat::Parse ---

namespace DecoderDataFormat {
    int Parse(const char *name) {
        if (std::strcmp(name, "dec") == 0)   return static_cast<int>(dec);
        if (std::strcmp(name, "hex") == 0)   return static_cast<int>(hex);
        if (std::strcmp(name, "oct") == 0)   return static_cast<int>(oct);
        if (std::strcmp(name, "bin") == 0)   return static_cast<int>(bin);
        if (std::strcmp(name, "ascii") == 0) return static_cast<int>(ascii);
        return static_cast<int>(hex);
    }
}
