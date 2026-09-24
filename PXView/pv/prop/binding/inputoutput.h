/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2015 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#ifndef PXVIEW_PV_PROP_BINDING_INPUTOUTPUT_H
#define PXVIEW_PV_PROP_BINDING_INPUTOUTPUT_H

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <QVariantMap>

#include <map>
#include <string>

#include "pv/prop/binding/binding.h"
// binding.h only forward-declares Property, but the getter/setter parameter
// types below need the complete class.
#include "pv/prop/property.h"

namespace pv {
namespace prop {
namespace binding {

/**
 * A binding of libsigrok input/output module options (`struct sr_option`) to
 * property widgets.
 *
 * This is the PXView port of PulseView's pv::binding::InputOutput: the module's
 * own option declarations drive the form, so no per-format code is needed and a
 * new libsigrok module shows up in the dialog automatically. Headerless formats
 * such as "binary" use it to ask the user for numchannels/samplerate instead of
 * guessing (PulseView does exactly this; PXView used to auto-detect from the
 * current device, which is wrong whenever the file was not exported from the
 * device that happens to be open).
 *
 * The option declarations are only read in the constructor, so the caller may
 * release them (sr_input_options_free()/sr_output_options_free()) right after
 * constructing the dialog.
 */
class InputOutput : public Binding
{
public:
    /**
     * @param options the module's option array (sr_input_options_get() /
     *                sr_output_options_get() return value). Only read here.
     * @param prefill per-option initial values (side-car for the "guess from
     *                the current device" defaults). Values that do not fit the
     *                option's declared GVariant type are ignored.
     */
    InputOutput(const struct sr_option **options,
                const QVariantMap &prefill = QVariantMap());

    ~InputOutput() override;

    /// id -> value. Every entry is a non-floating GVariant owned by this object.
    const std::map<std::string, GVariant *> &values() const { return _values; }

    /// Number of options turned into widgets (0 when the module takes none).
    int option_count() const { return static_cast<int>(_values.size()); }

    /**
     * Newly built option table for sr_input_new() / sr_output_new().
     * The caller owns the table; destroy it with g_hash_table_destroy() after
     * sr_input_new()/sr_output_new() returned (both copy what they need).
     */
    GHashTable *make_options_table() const;

private:
    Property *bind_enum(const QString &label, const struct sr_option *option,
                        Property::Getter getter, Property::Setter setter);

    std::map<std::string, GVariant *> _values;

    /// id -> the GVariant type the module declared for that option. The widgets
    /// are type-agnostic, so the declared type is the only thing that can tell
    /// make_options_table() what to convert the values back into.
    std::map<std::string, GVariantType *> _declared_types;
};

} // binding
} // prop
} // pv

#endif // PXVIEW_PV_PROP_BINDING_INPUTOUTPUT_H
