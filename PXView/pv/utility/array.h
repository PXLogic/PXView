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

#ifndef UTILITY_ARRAY_H
#define UTILITY_ARRAY_H

#include <cstdint>
#include <span>
#include <algorithm>

namespace pv
{
    namespace array
    {
        // C++20: std::span replaces raw pointer + size parameters.
        // Overloads for backward compat with existing callers that pass
        // raw pointers; new code should pass containers directly.

        [[nodiscard]] inline uint64_t find_min_uint64(std::span<const uint64_t> arr) noexcept
        {
            if (arr.empty()) return 0;
            return *std::ranges::min_element(arr);
        }

        [[nodiscard]] inline uint64_t find_max_uint64(std::span<const uint64_t> arr) noexcept
        {
            if (arr.empty()) return 0;
            return *std::ranges::max_element(arr);
        }

        // Backward-compat overloads for raw pointer + size callers.
        [[nodiscard]] inline uint64_t find_min_uint64(const uint64_t *arr, int size) noexcept
        {
            if (!arr || size <= 0) return 0;
            return find_min_uint64(std::span<const uint64_t>{arr, static_cast<size_t>(size)});
        }

        [[nodiscard]] inline uint64_t find_max_uint64(const uint64_t *arr, int size) noexcept
        {
            if (!arr || size <= 0) return 0;
            return find_max_uint64(std::span<const uint64_t>{arr, static_cast<size_t>(size)});
        }
    }
}

#endif
