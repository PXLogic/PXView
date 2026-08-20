/*
 * This file is part of the PXView project.
 * PXView is based on PulseView.
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

#ifndef PXVIEW_PV_UTILITY_ATOMIC_SHARED_PTR_H
#define PXVIEW_PV_UTILITY_ATOMIC_SHARED_PTR_H

#include <atomic>
#include <memory>
#include <mutex>
#include <version>

namespace pv {

// ---------------------------------------------------------------------------
// atomic_shared_ptr<T>: portable stand-in for std::atomic<std::shared_ptr<T>>
// (C++20 P0718R2). libstdc++ (GCC 12+) implements that specialization, but
// libc++ (Apple clang on the macOS CI runners) does NOT: the primary
// template is selected instead and hard-errors with "_Atomic cannot be applied
// to a type which is not trivially copyable". The fallback below keeps the same
// load()/store()/exchange() API — mutex-backed, which is also how libstdc++
// implements it internally for non-lock-free platforms — so call sites are
// identical on every toolchain.
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
template <typename T>
using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
#else
template <typename T>
class atomic_shared_ptr
{
public:
    atomic_shared_ptr() = default;
    atomic_shared_ptr(std::shared_ptr<T> p) : _p(std::move(p)) {}

    atomic_shared_ptr(const atomic_shared_ptr&) = delete;
    atomic_shared_ptr& operator=(const atomic_shared_ptr&) = delete;

    std::shared_ptr<T> load(
        std::memory_order = std::memory_order_seq_cst) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _p;
    }

    void store(std::shared_ptr<T> p,
               std::memory_order = std::memory_order_seq_cst)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _p = std::move(p);
    }

    std::shared_ptr<T> exchange(
        std::shared_ptr<T> p,
        std::memory_order = std::memory_order_seq_cst)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _p.swap(p);
        return p;
    }

private:
    mutable std::mutex  _mutex;
    std::shared_ptr<T>  _p;
};
#endif

} // namespace pv

#endif // PXVIEW_PV_UTILITY_ATOMIC_SHARED_PTR_H
