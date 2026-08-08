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

#ifndef PXVIEW_PV_GVARPTR_H
#define PXVIEW_PV_GVARPTR_H

#include <glib.h>
#include <QMetaType>

namespace pv {

/// GVarPtr — RAII wrapper for GVariant* with automatic reference counting.
///
/// GVariant uses reference-counted ownership. Manual g_variant_ref /
/// g_variant_unref pairs are error-prone (forget to unref → leak; unref
/// too many times → crash; forget to ref when storing → use-after-free).
/// GVarPtr eliminates these issues by automatically incrementing the ref
/// count on construction/copy and decrementing on destruction.
///
/// Usage:
///   GVarPtr p(g_variant_new_string("hello"));  // takes ownership (no extra ref)
///   GVarPtr p2(p);                              // ref count incremented
///   GVariant *raw = p.get();                    // raw access (no transfer)
///   GVariant *raw = p.release();                // transfer ownership to caller
///
/// When integrating with APIs that return a new reference (e.g.
/// g_variant_get), construct with adopt=true to take ownership without
/// adding a ref:
///   GVarPtr p(g_variant_get_child_value(gvar, 0));  // new ref, adopt it
class GVarPtr
{
public:
    /// Default constructor — null pointer.
    GVarPtr() : _ptr(nullptr) {}

    /// Construct from a raw GVariant*. By default, takes a new reference
    /// using g_variant_ref_sink (handles both floating refs from
    /// g_variant_new_* and regular refs). Use adopt=true when the
    /// GVariant was obtained via transfer-full (e.g. g_variant_iter_next_value,
    /// g_variant_get_child_value) and you want to take ownership without
    /// an extra ref.
    explicit GVarPtr(GVariant *ptr, bool adopt = false) : _ptr(ptr) {
        if (_ptr && !adopt)
            g_variant_ref_sink(_ptr);
    }

    /// Copy constructor — increments ref count.
    /// Uses g_variant_ref (not ref_sink) because the source is already
    /// sunk (non-floating); ref_sink would also work but is semantically
    /// misleading for an already-sunk variant.
    GVarPtr(const GVarPtr &other) : _ptr(other._ptr) {
        if (_ptr)
            g_variant_ref(_ptr);
    }

    /// Move constructor — steals the pointer without ref/unref.
    GVarPtr(GVarPtr &&other) noexcept : _ptr(other._ptr) {
        other._ptr = nullptr;
    }

    /// Destructor — decrements ref count.
    ~GVarPtr() {
        if (_ptr)
            g_variant_unref(_ptr);
    }

    /// Copy assignment — increments ref on new, decrements on old.
    GVarPtr &operator=(const GVarPtr &other) {
        if (this != &other) {
            if (_ptr)
                g_variant_unref(_ptr);
            _ptr = other._ptr;
            if (_ptr)
                g_variant_ref(_ptr);
        }
        return *this;
    }

    /// Move assignment — steals the pointer without ref/unref.
    GVarPtr &operator=(GVarPtr &&other) noexcept {
        if (this != &other) {
            if (_ptr)
                g_variant_unref(_ptr);
            _ptr = other._ptr;
            other._ptr = nullptr;
        }
        return *this;
    }

    /// Raw pointer access (no ownership transfer).
    GVariant *get() const { return _ptr; }

    /// Release ownership — caller becomes responsible for g_variant_unref.
    GVariant *release() {
        GVariant *p = _ptr;
        _ptr = nullptr;
        return p;
    }

    /// Reset to null (unrefs current if any).
    void reset() {
        if (_ptr) {
            g_variant_unref(_ptr);
            _ptr = nullptr;
        }
    }

    /// Boolean conversion — true if non-null.
    explicit operator bool() const { return _ptr != nullptr; }

private:
    GVariant *_ptr;
};

} // namespace pv

Q_DECLARE_METATYPE(pv::GVarPtr)

#endif // PXVIEW_PV_GVARPTR_H
