/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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


#include <cstdint>
#include <cassert>
#include <cmath>

#include "pv/ui/dsspinbox.h"
#include "pv/ui/intspinbox.h"
#include "pv/prop/int.h"

using std::optional;
using namespace std;

//#define INT8_MIN    (-0x7f - 1)
//#define INT16_MIN   (-0x7fff - 1)
//#define INT32_MIN   (-0x7fffffff - 1)
//#define INT64_MIN   (-0x7fffffffffffffff - 1)
//
//#define INT8_MAX    0x7f
//#define INT16_MAX   0x7fff
//#define INT32_MAX   0x7fffffff
//#define INT64_MAX   0x7fffffffffffffff
//
//#define UINT8_MAX   0xff
//#define UINT16_MAX  0xffff
//#define UINT32_MAX  0xffffffff
//#define UINT64_MAX  0xffffffffffffffff

namespace pv {
namespace prop {

Int::Int(QString name, QString label,
    QString suffix,
    std::optional< pair<int64_t, int64_t> > range,
    Getter getter,
    Setter setter) :
    Property(name, label, getter, setter),
	_suffix(suffix),
	_range(range),
    _value(nullptr),
	_spin_box(nullptr)
{
}

Int::~Int()
{
    if (_value)
        g_variant_unref(_value);
}

QWidget* Int::get_widget(QWidget *parent, bool auto_commit)
{
    if (_editor)
        return _editor;

    if (_value)
        g_variant_unref(_value);

    _value = _getter ? _getter() : nullptr;
    if (!_value)
        return nullptr;

    const GVariantType *const type = g_variant_get_type(_value);
    if (!type)
        return nullptr;
    assert(type);

    _editor = needs_wide_spin_box(type)
        ? create_wide_spin_box(parent, auto_commit)
        : create_spin_box(parent, auto_commit);

    return _editor;
}

bool Int::needs_wide_spin_box(const GVariantType *type) const
{
    if (!type)
        return false;

    // byte/int16/uint16/int32 always fit into a QSpinBox; uint32/int64/uint64
    // can hold values it cannot express (a 4 GHz samplerate, UINT64_MAX as
    // VCD's "skip" sentinel). An explicit range that stays inside int proves the
    // value never leaves the QSpinBox's reach, so the familiar editor is kept.
    const bool wide =
        g_variant_type_equal(type, G_VARIANT_TYPE_UINT32) ||
        g_variant_type_equal(type, G_VARIANT_TYPE_INT64) ||
        g_variant_type_equal(type, G_VARIANT_TYPE_UINT64);
    if (!wide)
        return false;

    if (_range && _range->first >= INT_MIN && _range->second <= INT_MAX)
        return false;

    return true;
}

QWidget* Int::create_spin_box(QWidget *parent, bool auto_commit)
{
    int64_t int_val = 0, range_min = 0, range_max = 0;

    _spin_box = new pv::ui::DsSpinBox(parent);
    _spin_box->setSuffix(_suffix);

    const GVariantType *const type = g_variant_get_type(_value);

    if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE))
    {
        int_val = g_variant_get_byte(_value);
        range_min = 0, range_max = UINT8_MAX;
    }
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16))
    {
        int_val = g_variant_get_int16(_value);
        range_min = INT16_MIN, range_max = INT16_MAX;
    }
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16))
    {
        int_val = g_variant_get_uint16(_value);
        range_min = 0, range_max = UINT16_MAX;
    }
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32))
    {
        int_val = g_variant_get_int32(_value);
        range_min = INT32_MIN, range_max = INT32_MAX;
    }
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32))
    {
        // Reached only with an explicit int-sized range (needs_wide_spin_box()),
        // so the value cannot exceed what the widget holds.
        int_val = g_variant_get_uint32(_value);
        range_min = 0, range_max = UINT32_MAX;
    }
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64))
    {
        int_val = g_variant_get_int64(_value);
        range_min = INT64_MIN, range_max = INT64_MAX;
    }
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64))
    {
        // Cap the RAW value at INT_MAX instead of letting it narrow further
        // down: UINT64_MAX assigned to the int64_t range_max becomes -1, the
        // clamps below then leave setRange(0, -1), and Qt makes min the only
        // legal value when max < min — so the editor was pinned at 0 no matter
        // what value it was given. Reached only with an explicit int-sized
        // range; without one, uint64 gets the text editor above.
        const guint64 raw = g_variant_get_uint64(_value);
        const guint64 capped = raw > static_cast<guint64>(INT_MAX)
            ? static_cast<guint64>(INT_MAX) : raw;
        int_val = static_cast<int64_t>(capped);
        range_min = 0, range_max = INT_MAX;
    }
    else
    {
        // Unexpected value type.
        assert(0);
    }

    range_min = max(range_min, static_cast<int64_t>(INT_MIN));
    range_max = min(range_max, static_cast<int64_t>(INT_MAX));

    // A value the widget cannot represent must be clamped BEFORE the narrowing
    // cast below: a uint32 above INT_MAX would otherwise wrap negative and land
    // on the lower bound (the same failure mode the uint64 branch above fixes).
    int_val = min(max(int_val, range_min), range_max);

    if (_range)
        _spin_box->setRange(static_cast<int>(_range->first), static_cast<int>(_range->second));
    else
        _spin_box->setRange(static_cast<int>(range_min), static_cast<int>(range_max));

    _displayed_value = static_cast<int>(int_val);
    _spin_box->setValue(_displayed_value);

    if (auto_commit)
        connect(_spin_box, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &Int::on_value_changed);

    return _spin_box;
}

QWidget* Int::create_wide_spin_box(QWidget *parent, bool auto_commit)
{
    const GVariantType *const type = g_variant_get_type(_value);

    _int_spin_box = new pv::ui::IntSpinBox(parent);
    // QAbstractSpinBox has the suffix support a QLineEdit would have needed a
    // composite widget for.
    _int_spin_box->setSuffix(_suffix);

    if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64)) {
        // The accepted range: whatever the declared type can hold, narrowed by
        // an explicit option range when there is one.
        int64_t minimum = INT64_MIN, maximum = INT64_MAX;
        if (_range) {
            minimum = max<int64_t>(minimum, static_cast<int64_t>(_range->first));
            maximum = min<int64_t>(maximum, static_cast<int64_t>(_range->second));
        }
        _int_spin_box->setSigned(true);
        _int_spin_box->setSignedRange(minimum, maximum);
        _int_spin_box->setSignedValue(g_variant_get_int64(_value));
    } else {
        uint64_t minimum = 0, maximum = UINT64_MAX;
        const bool is_uint32 = g_variant_type_equal(type, G_VARIANT_TYPE_UINT32);
        if (is_uint32)
            maximum = UINT32_MAX;
        if (_range) {
            // The explicit range is a signed int64 pair; an unsigned option
            // cannot go below zero, so negative bounds are clamped away.
            minimum = max<uint64_t>(minimum, static_cast<uint64_t>(
                max<int64_t>(0, static_cast<int64_t>(_range->first))));
            maximum = min<uint64_t>(maximum, static_cast<uint64_t>(
                max<int64_t>(0, static_cast<int64_t>(_range->second))));
        }

        _int_spin_box->setSigned(false);
        _int_spin_box->setUnsignedRange(minimum, maximum);
        _int_spin_box->setUnsignedValue(is_uint32 ? g_variant_get_uint32(_value)
                                                  : g_variant_get_uint64(_value));
    }

    if (auto_commit) {
        // IntSpinBox reports settled values only (editing finished / step), so
        // this cannot push a half-typed number to the device.
        connect(_int_spin_box, &pv::ui::IntSpinBox::valueChanged,
            this, &Int::on_wide_value_changed);
    }

    return _int_spin_box;
}

void Int::commit()
{
    if (!_setter)
        return;
    assert(_setter);

    if (!_spin_box && !_int_spin_box)
        return;

    if (!_value)
        return;
    assert(_value);

    // An untouched QSpinBox must hand over the module's declared value verbatim:
    // it can only hold an int, so a wider declared value would be shown clamped
    // and submitting the clamp would silently change the meaning. The 64-bit
    // editor keeps the value exactly, so it never needs that rescue.
    const bool wide = (_int_spin_box != nullptr);
    const bool untouched = !wide && (_spin_box->value() == _displayed_value);

    // Whichever editor is in use; for the narrow spin box the value is an int.
    const int64_t spin_value = _spin_box ? _spin_box->value() : 0;

    GVariant *new_value = nullptr;
    const GVariantType *const type = g_variant_get_type(_value);
    if (!type)
        return;
    assert(type);

    if (g_variant_type_equal(type, G_VARIANT_TYPE_BYTE))
        new_value = g_variant_new_byte(untouched
            ? g_variant_get_byte(_value) : static_cast<guint8>(spin_value));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT16))
        new_value = g_variant_new_int16(untouched
            ? g_variant_get_int16(_value) : static_cast<gint16>(spin_value));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT16))
        new_value = g_variant_new_uint16(untouched
            ? g_variant_get_uint16(_value) : static_cast<guint16>(spin_value));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT32))
        new_value = g_variant_new_int32(untouched
            ? g_variant_get_int32(_value) : static_cast<gint32>(spin_value));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT32))
        // Note the declared type: this used to build an int32 for a uint32
        // option (sr_input_new() rejects a mistyped option and fails the whole
        // import, which is why the option table is coerced again on its way out).
        new_value = g_variant_new_uint32(
            wide ? static_cast<guint32>(_int_spin_box->unsignedValue())
                 : (untouched ? g_variant_get_uint32(_value)
                              : static_cast<guint32>(spin_value)));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_INT64))
        new_value = g_variant_new_int64(
            wide ? _int_spin_box->signedValue()
                 : (untouched ? g_variant_get_int64(_value)
                              : static_cast<gint64>(spin_value)));
    else if (g_variant_type_equal(type, G_VARIANT_TYPE_UINT64))
        new_value = g_variant_new_uint64(
            wide ? _int_spin_box->unsignedValue()
                 : (untouched ? g_variant_get_uint64(_value)
                              : static_cast<guint64>(spin_value)));
    else
    {
        // Unexpected value type.
        assert(0);
    }

    if (!new_value)
        return;
    assert(new_value);

    g_variant_unref(_value);
    g_variant_ref(new_value);
    _value = new_value;

    _setter(new_value);
    emit committed();
}

void Int::on_value_changed(int)
{
    commit();
}

void Int::on_wide_value_changed()
{
    commit();
}

} // prop
} // pv
